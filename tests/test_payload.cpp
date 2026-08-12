// Round-trip checks on the synthetic payload: generate, decode, verify every
// field lands where the format sheet says it should.

#include "../src/payload/mpmt_payload.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace mpmt_sim;

static int failures = 0;

static void Check(const char* label, bool ok, const char* detail = "") {
  std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", label,
              detail[0] ? " — " : "", detail);
  if (!ok) ++failures;
}

int main() {
  Config config;
  config.hit_rate_hz = 10000;
  PayloadGenerator generator(config);

  // ~32k words, the DMA block size the FPGA delivers.
  const size_t kWords = 32768;
  std::vector<uint32_t> buffer(kWords, 0xDEADBEEF);

  const size_t written = generator.Fill(buffer.data(), kWords);

  Check("fills the block without overrunning", written <= kWords,
        (std::to_string(written) + " words").c_str());
  Check("leaves no room for another event", kWords - written < PMT_HIT_WORDS,
        (std::to_string(kWords - written) + " words spare").c_str());

  // Walk the buffer the way a receiver would: tags must frame every event.
  size_t index = 0;
  size_t hits = 0, tr32s = 0;
  bool framing_ok = true;
  std::set<int> channels_seen;
  uint64_t previous_timestamp = 0;
  bool timestamps_increase = true;
  bool adc_in_range = true;
  bool status_healthy = true;

  while (index < written) {
    if (WordTag(buffer[index]) != TAG_HEAD) { framing_ok = false; break; }

    const uint8_t event_type = (buffer[index] >> 27) & 0x7;

    if (event_type == EVENT_TYPE_TR32) {
      if (index + TR32_WORDS > written) { framing_ok = false; break; }
      if (WordTag(buffer[index + 1]) != TAG_SUBHIT ||
          WordTag(buffer[index + 2]) != TAG_ENV ||
          WordTag(buffer[index + 3]) != TAG_TAIL) { framing_ok = false; break; }

      const Tr32Event event = DecodeTr32(&buffer[index]);
      if (event.ratemeter < config.hit_rate_hz) adc_in_range = false;

      ++tr32s;
      index += TR32_WORDS;
      continue;
    }

    if (index + PMT_HIT_WORDS > written) { framing_ok = false; break; }
    if (WordTag(buffer[index + 1]) != TAG_SUBHIT ||
        WordTag(buffer[index + 2]) != TAG_TAIL) { framing_ok = false; break; }

    const PmtHit hit = DecodePmtHit(&buffer[index]);

    channels_seen.insert(hit.channel);
    if (hit.channel > MAX_CHANNEL) framing_ok = false;
    if (hit.adc > 0xFFF) adc_in_range = false;
    if (hit.status != 0b111) status_healthy = false;
    if (hit.length != 1) framing_ok = false;
    if (hits > 0 && hit.timestamp <= previous_timestamp) timestamps_increase = false;
    previous_timestamp = hit.timestamp;

    ++hits;
    index += PMT_HIT_WORDS;
  }

  Check("every event is correctly framed by its tags", framing_ok);
  Check("all 19 sub-PMT channels appear", channels_seen.size() == 19,
        (std::to_string(channels_seen.size()) + " channels").c_str());
  Check("48-bit timestamp reassembles monotonically", timestamps_increase);
  Check("ADC charge stays inside its 12-bit field", adc_in_range);
  Check("channel status reports healthy", status_healthy);

  // One Tr32 per simulated second. 32k words of 3-word hits at 10 kHz is
  // roughly a third of a second, so the first block may hold none.
  std::printf("     %zu PMT hits, %zu Tr32 events in %zu words\n",
              hits, tr32s, written);

  // Run long enough to cross several second boundaries and confirm the
  // Tr32 cadence rather than its mere presence.
  PayloadGenerator cadence(config);
  std::vector<uint32_t> block(kWords);
  const int blocks = 40;
  for (int i = 0; i < blocks; ++i) cadence.Fill(block.data(), kWords);

  const double seconds =
      static_cast<double>(cadence.hits_generated()) / config.hit_rate_hz;
  const double expected = seconds;   // one per second
  const double actual = static_cast<double>(cadence.tr32_generated());

  char detail[128];
  std::snprintf(detail, sizeof(detail), "%.1f s elapsed, %.0f Tr32, expected ~%.0f",
                seconds, actual, expected);
  Check("Tr32 arrives once per simulated second",
        actual > 0 && actual >= expected - 1.0 && actual <= expected + 1.0, detail);

  std::printf("\nFAILURES: %d\n", failures);
  return failures ? 1 : 0;
}
