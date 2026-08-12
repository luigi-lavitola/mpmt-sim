// Cross-check between the two independent implementations of the mPMT data
// format: the payload generator in this repo encodes it, readouttesting's
// RAWMPMTHit / RAWMPMTTr32 decode it. Both were written from the same format
// sheet without sharing code, so agreement here is real evidence that the
// layout is right — and this test fails loudly if either side later drifts.
//
// Build (paths relative to the repo root):
//   g++ -std=c++17 -I ../readouttesting/DataModel -I src/payload
//       tests/test_rbu_decode.cpp src/payload/mpmt_payload.cpp
//       ../readouttesting/DataModel/RAWMPMTHit.cpp
//       ../readouttesting/DataModel/RAWMPMTTr32.cpp -o test-rbu-decode

#include "../src/payload/mpmt_payload.h"

#include <RAWMPMTHit.h>
#include <RAWMPMTTr32.h>

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

static void Check(const char* label, bool ok, const std::string& detail = "") {
  std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", label,
              detail.empty() ? "" : " — ", detail.c_str());
  if (!ok) ++failures;
}

int main() {
  mpmt_sim::Config config;
  config.hit_rate_hz = 10000;
  mpmt_sim::PayloadGenerator generator(config);

  // 8192 words: the DMA block the FPGA delivers (32768 bytes). One block at
  // 10 kHz covers only 0.27 s, so several are needed before the once-a-second
  // Tr32 event turns up.
  const size_t kWords = 8192;
  const int kBlocks = 20;
  std::vector<uint32_t> buffer(kWords, 0);

  size_t hits = 0, tr32s = 0;
  size_t field_mismatches = 0;
  bool framing_ok = true;
  uint64_t previous_timestamp = 0;
  bool timestamps_increase = true;

  for (int block = 0; block < kBlocks && framing_ok; ++block) {

  const size_t written = generator.Fill(buffer.data(), kWords);
  size_t index = 0;

  while (index < written) {
    const uint8_t event_type = (buffer[index] >> 27) & 0x7;

    if (event_type == mpmt_sim::EVENT_TYPE_TR32) {
      if (index + 4 > written) { framing_ok = false; break; }

      // decode with both implementations and compare field by field
      const mpmt_sim::Tr32Event mine = mpmt_sim::DecodeTr32(&buffer[index]);
      RAWMPMTTr32 theirs(&buffer[index]);

      if (theirs.GetEventType() != mine.event_type) ++field_mismatches;
      if (theirs.GetTimestamp() != mine.timestamp_high) ++field_mismatches;
      if (theirs.GetRatemeter() != mine.ratemeter) ++field_mismatches;
      if (theirs.GetStatus() != mine.status) ++field_mismatches;
      if (theirs.GetHumidity() != mine.humidity) ++field_mismatches;
      if (theirs.GetTemperature() != mine.temperature) ++field_mismatches;
      if (theirs.GetDeadTime() != mine.dead_time) ++field_mismatches;
      if (theirs.GetCRC() != mine.crc) ++field_mismatches;

      ++tr32s;
      index += 4;
      continue;
    }

    if (index + 3 > written) { framing_ok = false; break; }

    const mpmt_sim::PmtHit mine = mpmt_sim::DecodePmtHit(&buffer[index]);
    RAWMPMTHit theirs(&buffer[index]);

    if (theirs.GetEventType() != mine.event_type) ++field_mismatches;
    if (theirs.GetChannel() != mine.channel) ++field_mismatches;
    if (theirs.GetStatus() != mine.status) ++field_mismatches;
    if (theirs.GetLength() != mine.length) ++field_mismatches;
    if (theirs.GetTimeStartTDC() != mine.time_start_tdc) ++field_mismatches;
    if (theirs.GetFineCoarseTDC() != mine.fine_coarse_tdc) ++field_mismatches;
    if (theirs.GetTimeStopTDC() != mine.time_stop_tdc) ++field_mismatches;
    if (theirs.GetCharge() != mine.adc) ++field_mismatches;
    if (theirs.GetCRC() != mine.crc) ++field_mismatches;
    if (theirs.GetTimestamp() != mine.timestamp) ++field_mismatches;

    // the static accessors must agree with the object ones
    if (RAWMPMTHit::GetChannel(&buffer[index]) != theirs.GetChannel()) ++field_mismatches;
    if (RAWMPMTHit::GetTimestamp(&buffer[index]) != theirs.GetTimestamp()) ++field_mismatches;
    if (RAWMPMTHit::GetEventType(&buffer[index]) != theirs.GetEventType()) ++field_mismatches;

    if (hits > 0 && theirs.GetTimestamp() <= previous_timestamp) timestamps_increase = false;
    previous_timestamp = theirs.GetTimestamp();

    ++hits;
    index += 3;
  }

  }

  Check("buffer frames cleanly under the RBU decoder", framing_ok);
  Check("every field agrees between the two implementations",
        field_mismatches == 0,
        std::to_string(field_mismatches) + " mismatches over " +
        std::to_string(hits) + " hits and " + std::to_string(tr32s) + " Tr32");
  Check("RBU reassembles the 48 bit timestamp monotonically", timestamps_increase);
  Check("both event kinds present", hits > 0 && tr32s > 0,
        std::to_string(hits) + " hits, " + std::to_string(tr32s) + " Tr32");

  // The RBU's writer classes must round-trip through their own decoder too.
  MPMTHit built(0x0ABCDEF12345ULL & 0xFFFFFFFFFFFULL, 7);
  built.SetEventType(2);
  built.SetStatus(0b101);
  built.SetCharge(3210);
  built.SetTimeStartTDC(9);
  built.SetFineCoarseTDC(77);
  built.SetTimeStopTDC(4);
  built.SetCRC(0);

  RAWMPMTHit read_back(built.GetData());
  const bool round_trip =
      read_back.GetChannel() == 7 && read_back.GetEventType() == 2 &&
      read_back.GetStatus() == 0b101 && read_back.GetCharge() == 3210 &&
      read_back.GetTimeStartTDC() == 9 && read_back.GetFineCoarseTDC() == 77 &&
      read_back.GetTimeStopTDC() == 4 &&
      read_back.GetTimestamp() == (0x0ABCDEF12345ULL & 0xFFFFFFFFFFFULL) &&
      read_back.GetType() == 0b10;
  Check("MPMTHit writer round-trips through RAWMPMTHit", round_trip);

  MPMTTr32 tr32_built(0x1234567);
  tr32_built.SetEventType(mpmt_sim::EVENT_TYPE_TR32);
  tr32_built.SetRatemeter(999999);
  tr32_built.SetStatus(0b101101);
  tr32_built.SetHumidity(480);
  tr32_built.SetTemperature(250);
  tr32_built.SetDeadTime(1234);
  tr32_built.SetCRC(0);

  RAWMPMTTr32 tr32_read(tr32_built.GetData());
  const bool tr32_round_trip =
      tr32_read.GetTimestamp() == 0x1234567 &&
      tr32_read.GetRatemeter() == 999999 && tr32_read.GetStatus() == 0b101101 &&
      tr32_read.GetHumidity() == 480 && tr32_read.GetTemperature() == 250 &&
      tr32_read.GetDeadTime() == 1234 && tr32_read.GetType() == 0b10;
  Check("MPMTTr32 writer round-trips through RAWMPMTTr32", tr32_round_trip);

  std::printf("\nFAILURES: %d\n", failures);
  return failures ? 1 : 0;
}
