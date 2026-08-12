// Synthetic mPMT payload: the words the FPGA would deliver over DMA.
//
// producer.cpp never parses this buffer — it hands it straight to
// DataSender, which prepends the DAQHeader. So everything here is payload
// only: the DAQ header is not our business, except that DataSender writes
// its word count from the byte length we return.
//
// Bit layouts below are transcribed from the mPMT data format sheet. Fields
// run MSB first, matching the way the sheet is drawn.
//
//   PMT hit — always 3 words
//
//     head    [31:30] 0b10 tag
//             [29:27] event type      (3)
//             [26:22] channel         (5)   sub-PMT id, 0..18
//             [21:19] channel status  (3)   b0 PLL locked, b1 HV, b2 rate ok
//             [18:4]  timestamp bits 29..15 (15)
//             [3:0]   length          (4)   number of subhits
//
//     subhit  [31:30] 0b00 tag
//             [29:15] timestamp bits 14..0  (15)
//             [14:11] time start TDC  (4)   285 ps steps
//             [10:4]  fine/coarse TDC (7)
//             [3:0]   time stop TDC   (4)   285 ps steps
//
//     tail    [31:30] 0b11 tag
//             [29:16] timestamp bits 43..30 (14)
//             [15:4]  ADC charge      (12)
//             [3:0]   CRC             (4)
//
//   Tr32 event — 4 words, emitted once per second
//
//     head    [31:30] 0b10 tag
//             [29:27] event type      (3)
//             [26:0]  timestamp bits 48..22 (27)
//
//     rate    [31:30] 0b00 tag
//             [29:0]  mPMT total ratemeter (30)
//
//     env     [31:30] 0b01 tag
//             [29:24] mPMT status     (6)
//             [23:12] humidity        (12)
//             [11:0]  temperature     (12)
//
//     tail    [31:30] 0b11 tag
//             [29:14] dead time       (16)
//             [13:4]  reserved        (10)
//             [3:0]   CRC             (4)
//
// The 48-bit timestamp counts in 285 ps steps. Head, subhit and tail
// together carry bits 0..43 of it contiguously; the top four bits are not
// transmitted in a PMT hit.

#ifndef MPMT_SIM_PAYLOAD_H
#define MPMT_SIM_PAYLOAD_H

#include <cstddef>
#include <cstdint>

namespace mpmt_sim {

// Word tags, bits [31:30].
constexpr uint32_t TAG_HEAD = 0b10;
constexpr uint32_t TAG_SUBHIT = 0b00;
constexpr uint32_t TAG_ENV = 0b01;
constexpr uint32_t TAG_TAIL = 0b11;

constexpr size_t PMT_HIT_WORDS = 3;
constexpr size_t TR32_WORDS = 4;

constexpr uint8_t EVENT_TYPE_NORMAL_PMT = 0;
constexpr uint8_t EVENT_TYPE_PEDESTAL = 1;
constexpr uint8_t EVENT_TYPE_LED = 2;
constexpr uint8_t EVENT_TYPE_CALIB = 3;

// The format sheet lists PPS as event type F, which needs four bits, but the
// event type field is three bits wide in both the PMT hit and the Tr32 head.
// Until that is reconciled the Tr32 event goes out with the largest value a
// three-bit field can hold. Change this once the real encoding is known.
constexpr uint8_t EVENT_TYPE_TR32 = 0b111;

constexpr uint8_t MAX_CHANNEL = 18;   // sub-PMT ids run 0..18

// Timestamp counter resolution, from the sheet: 0.285 ns per step.
constexpr double TIMESTAMP_STEP_NS = 0.285;

struct Config {
  uint8_t channels = MAX_CHANNEL + 1;   // how many sub-PMTs generate hits
  uint32_t hit_rate_hz = 10000;         // per module, across all channels
  uint16_t adc_mean = 1200;             // ADC counts of a typical charge
  uint16_t adc_spread = 300;
  uint32_t seed = 1;
};

// Fills buffers with well-formed events, keeping the 48-bit timestamp
// running across calls so successive DMA blocks are continuous, and
// injecting one Tr32 event per simulated second.
class PayloadGenerator {
 public:
  explicit PayloadGenerator(const Config& config = Config());

  // Writes as many whole events as fit in max_words and returns the number
  // of words written. Never writes a partial event: a buffer that cannot
  // hold three more words is returned short.
  size_t Fill(uint32_t* buffer, size_t max_words);

  uint64_t timestamp() const { return timestamp_; }
  uint64_t hits_generated() const { return hits_generated_; }
  uint64_t tr32_generated() const { return tr32_generated_; }

 private:
  size_t WritePmtHit(uint32_t* buffer);
  size_t WriteTr32(uint32_t* buffer);
  uint32_t NextRandom();

  Config config_;
  uint64_t timestamp_ = 0;          // 48-bit counter, 285 ps steps
  uint64_t next_tr32_timestamp_;    // when the next Tr32 event is due
  uint64_t timestamp_step_;         // counter advance between hits
  uint64_t hits_generated_ = 0;
  uint64_t tr32_generated_ = 0;
  uint32_t rng_state_;
  uint8_t next_channel_ = 0;
};

// Decoding helpers, used by the tests and handy when eyeballing a capture.
inline uint32_t WordTag(uint32_t word) { return word >> 30; }

struct PmtHit {
  uint8_t event_type;
  uint8_t channel;
  uint8_t status;
  uint8_t length;
  uint8_t time_start_tdc;
  uint8_t fine_coarse_tdc;
  uint8_t time_stop_tdc;
  uint16_t adc;
  uint8_t crc;
  uint64_t timestamp;   // reassembled bits 0..43
};

struct Tr32Event {
  uint8_t event_type;
  uint32_t timestamp_high;   // bits 48..22 as transmitted
  uint32_t ratemeter;
  uint8_t status;
  uint16_t humidity;
  uint16_t temperature;
  uint16_t dead_time;
  uint8_t crc;
};

PmtHit DecodePmtHit(const uint32_t* words);
Tr32Event DecodeTr32(const uint32_t* words);

}  // namespace mpmt_sim

#endif  // MPMT_SIM_PAYLOAD_H
