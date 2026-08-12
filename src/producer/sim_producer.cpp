// A simulated mPMT node for ToolDAQ.
//
// Same shape as mpmt-daq-interface's producer.cpp: it registers with
// ServiceDiscovery through DAQInterface, exposes Start/Stop/Status run
// control, and streams DAQHeader-wrapped buffers to the RBU through
// DataSender. The one thing it does differently is where the payload comes
// from — a PayloadGenerator instead of the DMA-proxy device, since the DMA
// seam is the one that cannot be faked without a kernel driver.
//
// DAQInterface, DataSender, DAQHeader, DataMessages, MSSIntegration and
// MSSSlowControl are all compiled from mpmt-daq-interface itself (see
// CMakeLists.txt), not copied, so both the protocol on the wire and the slow
// control surface are the real ones.
//
// Point --mss-config at the simulated mss (mpmt-sim's compose.yml) and the
// node exposes the same ~200 PMT/LED/fpga slow control variables and the same
// monitoring snapshot a real mPMT would, against simulated hardware.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <DataSender.h>
#include <MSSIntegration.h>
#include <MSSSlowControl.h>
#include <MSSStatusCache.h>
#include <Store.h>

#include "../payload/mpmt_payload.h"

using namespace ToolFramework;

namespace {

// The DMA block the FPGA delivers: 32768 bytes, i.e. 8192 32-bit words.
constexpr size_t kBlockWords = 8192;
constexpr size_t kBlockBytes = kBlockWords * sizeof(uint32_t);

// The format sheet asks for a message roughly every 100 ms.
constexpr int kSendPeriodMs = 100;

std::atomic<bool> keep_running{true};

// Written from three places - the Start/Stop button thread, the RunStart and
// RunStop alert callbacks (which run on the framework's own thread), and read
// by the send loop - so it has to be atomic.
std::atomic<bool> taking_data{false};

DAQInterface* daq_inter = nullptr;

mpmt_mss::MSSClient* mss_client = nullptr;
int mss_monitor_period_sec = 10;
MssStatusCache mss_status_cache;   // populated by MssMonitor, read by the
                                   // slow control getters

void IntHandler(int) { keep_running = false; }

// mss monitoring THREAD - every mss_monitor_period_sec, polls mss and
// forwards a raw snapshot, same as producer.cpp's mss_monitor.
void MssMonitor() {
  auto last = std::chrono::steady_clock::now() - std::chrono::seconds(mss_monitor_period_sec);

  while (keep_running) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= mss_monitor_period_sec) {
      last = now;
      try {
        std::string snapshot = BuildMssMonitoringSnapshot(*mss_client, mss_status_cache);
        daq_inter->SendMonitoringData(snapshot, "mss");
      } catch (const std::exception& e) {
        daq_inter->SendLog(std::string("mss: monitoring cycle failed: ") + e.what(),
                           LogLevel::Warning);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

// Unix time in milliseconds, truncated to 32 bits. The format sheet defines
// DAQ header word 1 as "unix time of message sent, resolution 1 ms", while
// producer.cpp passes a free-running counter there — a known gap noted in
// its README. DataSender takes this as its `coarse_counter` argument and
// writes it through unchanged, so passing the timestamp here satisfies the
// spec without touching DataSender.
uint32_t UnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now);
  return static_cast<uint32_t>(ms.count() & 0xFFFFFFFF);
}

// One place that flips data taking and publishes the matching Status, so the
// buttons and the run alerts cannot disagree about what the node is doing.
void SetTakingData(bool on, const char* status) {
  const bool was = taking_data.exchange(on);
  if (was == on) return;
  daq_inter->sc_vars["Status"]->SetValue(status);
  std::cout << "run state -> " << status << std::endl;
}

// Polls the Start/Stop buttons, mirroring producer.cpp's control thread.
void Control() {
  while (keep_running) {
    if (daq_inter->sc_vars["Start"]->GetValue<bool>()) {
      daq_inter->sc_vars["Start"]->SetValue(false);
      SetTakingData(true, "Running");
    }

    if (daq_inter->sc_vars["Stop"]->GetValue<bool>()) {
      daq_inter->sc_vars["Stop"]->SetValue(false);
      SetTakingData(false, "Stopped");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void Usage(const char* argv0) {
  std::cout
      << "usage: " << argv0 << " [options]\n"
      << "  --daq-config <file>   DAQInterface config   (default ./InterfaceConfig)\n"
      << "  --data-config <file>  DataSender config     (default ./DataSenderConfig)\n"
      << "  --mss-config <file>   mss client config     (default ./MSSConfig)\n"
      << "  --rate <hz>           simulated hit rate    (default 10000)\n"
      << "  --disable-rc          skip run control, start sending immediately\n"
      << "  --disable-mss         skip the mss slow controls and monitoring\n"
      << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGINT, IntHandler);
  signal(SIGTERM, IntHandler);

  std::string daq_config = "./InterfaceConfig";
  std::string data_config = "./DataSenderConfig";
  std::string mss_config = "./MSSConfig";
  uint32_t hit_rate_hz = 10000;
  bool runcontrol = true;
  bool use_mss = true;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--daq-config" && i + 1 < argc) daq_config = argv[++i];
    else if (arg == "--data-config" && i + 1 < argc) data_config = argv[++i];
    else if (arg == "--mss-config" && i + 1 < argc) mss_config = argv[++i];
    else if (arg == "--rate" && i + 1 < argc) hit_rate_hz = std::stoul(argv[++i]);
    else if (arg == "--disable-rc") runcontrol = false;
    else if (arg == "--disable-mss") use_mss = false;
    else { Usage(argv[0]); return arg == "--help" ? 0 : 1; }
  }

  mpmt_sim::Config config;
  config.hit_rate_hz = hit_rate_hz;
  mpmt_sim::PayloadGenerator generator(config);

  daq_inter = new DAQInterface(daq_config);
  DataSender data_sender(daq_inter, data_config);

  std::thread control_thread;
  std::thread mss_thread;

  if (use_mss) {

    Store mss_vars;
    std::string mss_url = "http://localhost:8000/rpc";
    double mss_timeout_sec = 5.0;
    if (mss_vars.Initialise(mss_config)) {
      mss_vars.Get("mss_url", mss_url);
      mss_vars.Get("mss_timeout_sec", mss_timeout_sec);
      mss_vars.Get("monitor_period_sec", mss_monitor_period_sec);
    }
    mss_client = new mpmt_mss::MSSClient(mss_url, mss_timeout_sec);

    // apply the DAQ device config (pmt_channels/led_channels) to mss at
    // startup, and again on every live config change
    std::string mss_config_json;
    if (daq_inter->GetDeviceConfig(mss_config_json, -1) && !mss_config_json.empty()) {
      try {
        ApplyMssConfig(*mss_client, *daq_inter, mss_config_json);
      } catch (const std::exception& e) {
        daq_inter->SendLog(std::string("mss: initial config apply failed: ") + e.what(),
                           LogLevel::Error);
      }
    }

    daq_inter->SetChangeConfigFunc([](std::string json) -> bool {
      try {
        ApplyMssConfig(*mss_client, *daq_inter, json);
        return true;
      } catch (const std::exception& e) {
        daq_inter->SendLog(std::string("mss: config apply failed: ") + e.what(),
                           LogLevel::Error);
        return false;
      }
    });

    // Exposes each mss PMT/LED/fpga parameter as its own DAQInterface slow
    // control variable, so the WebServer control UI (which only understands
    // DAQInterface slow control, not mss's own JSON-RPC) can drive them.
    RegisterMssSlowControls(*daq_inter, *mss_client, mss_status_cache);

    mss_thread = std::thread(MssMonitor);
  }

  // Status is published in both modes. producer.cpp only registers it when
  // run control is on, which leaves a --disable-rc node reporting "N/A" to
  // ServiceDiscovery while it is actually streaming — the monitoring pages
  // then show it as idle. A node that is taking data should say so.
  daq_inter->sc_vars.Add("Status", INFO);
  daq_inter->sc_vars["Status"]->SetValue("Initialising");

  if (runcontrol) {
    daq_inter->sc_vars.Add("Start", BUTTON);
    daq_inter->sc_vars["Start"]->SetValue(false);

    daq_inter->sc_vars.Add("Stop", BUTTON);
    daq_inter->sc_vars["Stop"]->SetValue(false);

    // Join the ToolDAQ run lifecycle, not just the manual buttons. The
    // WebServer's run page broadcasts alerts rather than addressing nodes
    // one by one, so a node that does not subscribe simply never starts or
    // stops with the run - which is where producer.cpp stands today.
    //
    // Of the four alerts a run start sends, the framework already handles
    // two: LoadConfig fetches this device's resolved configuration into
    // m_local_config by itself, and ChangeConfig calls the callback set
    // through SetChangeConfigFunc above. RunStop only needs the hook below.
    // RunStart has no hook at all, so it is subscribed by hand.
    daq_inter->AlertSubscribe("RunStart", [](const char*, const char*) -> bool {
      SetTakingData(true, "Running");
      return true;
    });

    // SetRunStopFunc also clears the cached base/runmode config ids, so the
    // next ChangeConfig re-runs the callback instead of being skipped as a
    // no-op - which is what lets a node be reconfigured between runs.
    daq_inter->SetRunStopFunc([]() -> bool {
      SetTakingData(false, "Stopped");
      return true;
    });

    daq_inter->sc_vars["Status"]->SetValue("Ready");

    control_thread = std::thread(Control);
  } else {
    SetTakingData(true, "Running");
  }

  std::cout << "sim-producer up: " << daq_inter->GetDeviceName()
            << " — " << kBlockWords << " words per block, "
            << hit_rate_hz << " Hz simulated hit rate" << std::endl;

  uint64_t sent = 0;

  while (keep_running) {
    if (!taking_data) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    // DataSender owns the buffer until it is acked or dropped, and frees it
    // through the callback — the same contract producer.cpp relies on to
    // recycle its DMA buffer ids.
    uint32_t* words = new uint32_t[kBlockWords];
    const size_t written = generator.Fill(words, kBlockWords);

    // Short-fill the tail rather than send uninitialised words: Fill never
    // splits an event, so the last few words of a block can be unused.
    if (written < kBlockWords)
      std::memset(words + written, 0, (kBlockWords - written) * sizeof(uint32_t));

    data_sender.Add(words, kBlockBytes, UnixMillis(),
                    [](void* p) { delete[] static_cast<uint32_t*>(p); });

    if (++sent % 10 == 0) {
      std::cout << "sent=" << sent
                << " hits=" << generator.hits_generated()
                << " tr32=" << generator.tr32_generated()
                << " " << data_sender.Summary() << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kSendPeriodMs));
  }

  if (control_thread.joinable()) control_thread.join();
  if (mss_thread.joinable()) mss_thread.join();

  std::cout << "Bye! (sent " << sent << " blocks, "
            << generator.hits_generated() << " hits, "
            << generator.tr32_generated() << " Tr32 events)" << std::endl;

  return 0;
}
