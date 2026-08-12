#include "mpmt_payload.h"

namespace mpmt_sim {

namespace {

// One second expressed in timestamp counter steps (285 ps each).
constexpr uint64_t COUNTER_STEPS_PER_SECOND =
    static_cast<uint64_t>(1e9 / TIMESTAMP_STEP_NS);

constexpr uint64_t TIMESTAMP_MASK_48 = (1ULL << 48) - 1;

inline uint32_t Field(uint32_t value, unsigned shift, unsigned width) {
  return (value & ((1u << width) - 1u)) << shift;
}

inline uint32_t Extract(uint32_t word, unsigned shift, unsigned width) {
  return (word >> shift) & ((1u << width) - 1u);
}

}  // namespace

PayloadGenerator::PayloadGenerator(const Config& config)
    : config_(config), rng_state_(config.seed ? config.seed : 1) {
  if (config_.channels == 0) config_.channels = 1;
  if (config_.channels > MAX_CHANNEL + 1) config_.channels = MAX_CHANNEL + 1;

  const uint32_t rate = config_.hit_rate_hz ? config_.hit_rate_hz : 1;
  timestamp_step_ = COUNTER_STEPS_PER_SECOND / rate;
  if (timestamp_step_ == 0) timestamp_step_ = 1;

  next_tr32_timestamp_ = COUNTER_STEPS_PER_SECOND;
}

// xorshift32: no <random> dependency, and reproducible across platforms so a
// captured buffer can be regenerated exactly from its seed.
uint32_t PayloadGenerator::NextRandom() {
  rng_state_ ^= rng_state_ << 13;
  rng_state_ ^= rng_state_ >> 17;
  rng_state_ ^= rng_state_ << 5;
  return rng_state_;
}

size_t PayloadGenerator::WritePmtHit(uint32_t* buffer) {
  const uint64_t ts = timestamp_ & TIMESTAMP_MASK_48;

  const uint8_t channel = next_channel_;
  next_channel_ = static_cast<uint8_t>((next_channel_ + 1) % config_.channels);

  // b0 PLL locked, b1 HV status, b2 rate ok — a healthy channel reports all
  // three, which is what a simulator with no fault injection should say.
  const uint8_t status = 0b111;

  const uint32_t rnd = NextRandom();

  int32_t adc = static_cast<int32_t>(config_.adc_mean) +
                static_cast<int32_t>(rnd % (2u * config_.adc_spread + 1u)) -
                static_cast<int32_t>(config_.adc_spread);
  if (adc < 0) adc = 0;
  if (adc > 0xFFF) adc = 0xFFF;

  const uint8_t start_tdc = (rnd >> 3) & 0xF;
  const uint8_t fine_coarse = (rnd >> 7) & 0x7F;
  const uint8_t stop_tdc = (rnd >> 14) & 0xF;

  buffer[0] = Field(TAG_HEAD, 30, 2) |
              Field(EVENT_TYPE_NORMAL_PMT, 27, 3) |
              Field(channel, 22, 5) |
              Field(status, 19, 3) |
              Field(static_cast<uint32_t>((ts >> 15) & 0x7FFF), 4, 15) |
              Field(1, 0, 4);            // one subhit, always

  buffer[1] = Field(TAG_SUBHIT, 30, 2) |
              Field(static_cast<uint32_t>(ts & 0x7FFF), 15, 15) |
              Field(start_tdc, 11, 4) |
              Field(fine_coarse, 4, 7) |
              Field(stop_tdc, 0, 4);

  buffer[2] = Field(TAG_TAIL, 30, 2) |
              Field(static_cast<uint32_t>((ts >> 30) & 0x3FFF), 16, 14) |
              Field(static_cast<uint32_t>(adc), 4, 12) |
              Field(0, 0, 4);            // CRC: algorithm not yet specified

  ++hits_generated_;
  return PMT_HIT_WORDS;
}

size_t PayloadGenerator::WriteTr32(uint32_t* buffer) {
  const uint64_t ts = timestamp_ & TIMESTAMP_MASK_48;
  const uint32_t rnd = NextRandom();

  // b0 ext CLK0, b1 ext CLK1, b2 PLL locked, b3 power ok, b4 voltage ok,
  // b5 clock source. Everything nominal except the second external clock,
  // which a bench setup would not have.
  const uint8_t status = 0b011101;

  // Ratemeter is the module hit rate over the last second.
  const uint32_t ratemeter = config_.hit_rate_hz + (rnd % 128u);

  // 12-bit fields; scaled the same way the sensors report, so the values
  // land in a believable range rather than filling the field.
  const uint16_t humidity = static_cast<uint16_t>(450 + (rnd >> 8) % 60);
  const uint16_t temperature = static_cast<uint16_t>(240 + (rnd >> 16) % 40);
  const uint16_t dead_time = static_cast<uint16_t>((rnd >> 20) % 1000);

  buffer[0] = Field(TAG_HEAD, 30, 2) |
              Field(EVENT_TYPE_TR32, 27, 3) |
              Field(static_cast<uint32_t>((ts >> 22) & 0x7FFFFFF), 0, 27);

  buffer[1] = Field(TAG_SUBHIT, 30, 2) |
              Field(ratemeter & 0x3FFFFFFF, 0, 30);

  buffer[2] = Field(TAG_ENV, 30, 2) |
              Field(status, 24, 6) |
              Field(humidity, 12, 12) |
              Field(temperature, 0, 12);

  buffer[3] = Field(TAG_TAIL, 30, 2) |
              Field(dead_time, 14, 16) |
              Field(0, 4, 10) |          // reserved
              Field(0, 0, 4);            // CRC: algorithm not yet specified

  ++tr32_generated_;
  return TR32_WORDS;
}

size_t PayloadGenerator::Fill(uint32_t* buffer, size_t max_words) {
  size_t written = 0;

  while (true) {
    const bool tr32_due = timestamp_ >= next_tr32_timestamp_;
    const size_t needed = tr32_due ? TR32_WORDS : PMT_HIT_WORDS;

    if (written + needed > max_words) break;

    if (tr32_due) {
      written += WriteTr32(buffer + written);
      next_tr32_timestamp_ += COUNTER_STEPS_PER_SECOND;
    } else {
      written += WritePmtHit(buffer + written);
      timestamp_ = (timestamp_ + timestamp_step_) & TIMESTAMP_MASK_48;
    }
  }

  return written;
}

PmtHit DecodePmtHit(const uint32_t* words) {
  PmtHit hit{};

  hit.event_type = Extract(words[0], 27, 3);
  hit.channel = Extract(words[0], 22, 5);
  hit.status = Extract(words[0], 19, 3);
  hit.length = Extract(words[0], 0, 4);

  hit.time_start_tdc = Extract(words[1], 11, 4);
  hit.fine_coarse_tdc = Extract(words[1], 4, 7);
  hit.time_stop_tdc = Extract(words[1], 0, 4);

  hit.adc = Extract(words[2], 4, 12);
  hit.crc = Extract(words[2], 0, 4);

  const uint64_t low = Extract(words[1], 15, 15);          // bits 14..0
  const uint64_t mid = Extract(words[0], 4, 15);           // bits 29..15
  const uint64_t high = Extract(words[2], 16, 14);         // bits 43..30

  hit.timestamp = low | (mid << 15) | (high << 30);

  return hit;
}

Tr32Event DecodeTr32(const uint32_t* words) {
  Tr32Event event{};

  event.event_type = Extract(words[0], 27, 3);
  event.timestamp_high = Extract(words[0], 0, 27);

  event.ratemeter = words[1] & 0x3FFFFFFF;

  event.status = Extract(words[2], 24, 6);
  event.humidity = Extract(words[2], 12, 12);
  event.temperature = Extract(words[2], 0, 12);

  event.dead_time = Extract(words[3], 14, 16);
  event.crc = Extract(words[3], 0, 4);

  return event;
}

}  // namespace mpmt_sim
