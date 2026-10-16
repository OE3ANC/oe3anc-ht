// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ht {
namespace modem {
constexpr size_t max_frame_size = 330;
constexpr unsigned sample_rate = 48000;

// AX.25 bytes without flags or FCS. No AX.25 address/control interpretation.
struct Frame {
    uint8_t bytes[max_frame_size] = {};
    size_t size = 0;
};

enum class KissCommand : uint8_t {
    Data = 0,
    TxDelay = 1,
    Persistence = 2,
    SlotTime = 3,
    TxTail = 4,
    FullDuplex = 5,
    Return = 0xff
};
struct KissMessage {
    KissCommand command = KissCommand::Data;
    Frame frame;
};
enum class KissResult { None, Message, Rejected };

// One owner per object. Invalid/oversize input is discarded through the next
// FEND. Command payload lengths and port zero are checked before delivery.
class KissParser {
  public:
    void reset();
    KissResult feed(uint8_t byte, KissMessage &output);

  private:
    uint8_t bytes_[max_frame_size + 1] = {};
    size_t size_ = 0;
    bool started_ = false;
    bool escaped_ = false;
    bool invalid_ = false;
};

// Port-zero data encoding. Returns zero without writing if capacity is too
// small or data invalid. Worst-case capacity is 2 * max_frame_size + 3.
size_t kiss_encode(const uint8_t *data, size_t size, uint8_t *output, size_t capacity);
uint16_t fcs(const uint8_t *data, size_t size);

// Single-context, allocation-free 48 kHz Bell 202. start copies the packet;
// delay/tail are KISS 10 ms units, filled with complete HDLC flags. At least
// two opening flags (13.3 ms) are sent, including when TXDELAY is zero. render
// writes only the returned count, and zero means finished. Amplitude is
// 1..32767 PCM counts; caller owns PTT and radio deviation calibration.
class Transmitter {
  public:
    bool start(const Frame &frame, uint8_t txdelay = 30, uint8_t txtail = 0,
               int16_t amplitude = 12000);
    size_t render(int16_t *output, size_t capacity);
    bool active() const {
        return bit_position_ < bit_count_;
    }
    void reset();

  private:
    void append(bool bit);
    void flag();
    // Largest stuffed payload/FCS plus 2 * 2550 ms of flags and closing flags.
    uint8_t bits_[9600] = {};
    size_t bit_count_ = 0;
    size_t bit_position_ = 0;
    unsigned sample_position_ = 0;
    float phase_ = 0;
    bool mark_ = true;
    int16_t amplitude_ = 12000;
};

// Feed consecutive mono 48 kHz samples. Returns only CRC-verified payloads;
// output stays untouched otherwise. Reset on stream discontinuity. Correlation
// and a transition-driven clock track phase and small sound-clock differences.
// carrier() is an energy indication (200 counts RMS), not proof of a valid
// AX.25 transmission. Hardware gain/noise-floor calibration remains external.
class Receiver {
  public:
    void reset();
    bool sample(int16_t input, Frame &output);
    bool carrier() const {
        return energy_ > 40000.0f;
    }

  private:
    bool bit(bool value, Frame &output);
    bool decode(size_t count, Frame &output);
    float history_[40] = {};
    unsigned position_ = 0;
    unsigned filled_ = 0;
    float energy_ = 0;
    float dc_ = 0;
    float clock_ = 0;
    bool tone_ = false;
    bool sampled_tone_ = false;
    bool tone_valid_ = false;
    uint8_t shift_ = 0;
    uint8_t bits_[3200] = {};
    size_t bit_count_ = 0;
    bool collecting_ = false;
};
} // namespace modem
} // namespace ht
