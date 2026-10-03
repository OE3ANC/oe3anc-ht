// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/m17.hpp>

namespace ht {
namespace m17 {
struct TxSamples {
    int16_t samples[1920] = {}; // One 40 ms frame at 48 kHz.
};

// Pure sample generation. The caller owns RF/audio and the output buffer.
// Keep one instance per TX stream, reset before its preamble, and preserve
// history between frames. No allocations, callbacks, or hardware polarity.
class Modulator {
  public:
    void reset();
    void render(const Frame &frame, TxSamples &output);

  private:
    float filter(float sample);
    float history_[162] = {};
    unsigned position_ = 0;
};

// Feed consecutive mono samples at 24 kHz from one processing context.
// A true return copies one complete wire frame into output; false leaves it
// untouched. No sample buffers are retained. Reset at each new RX session.
class Demodulator {
  public:
    void reset();
    bool sample(int16_t input, Frame &output, bool invert = false);

    bool locked() const {
        return receiving_;
    }

  private:
    int16_t filter(int16_t input, bool invert);
    float envelope(int16_t input);
    bool synchronize(float threshold);
    void acquire();
    void quantize(int16_t input);
    void track_clock(int16_t input);
    void update_clock();
    void update_deviation();
    void lose_lock();
    bool sync_matches(uint16_t word, unsigned tolerance) const;

    float filter_history_[82] = {};
    unsigned filter_position_ = 0;
    int64_t dc_accumulator_ = 0;
    int32_t dc_previous_input_ = 0;
    int32_t dc_previous_output_ = 0;
    float envelope_history_[3] = {};
    unsigned envelope_position_ = 0;
    int16_t correlation_[40] = {};
    unsigned correlation_position_ = 0;
    int32_t sync_peaks_[5] = {};
    bool sync_triggered_ = false;
    unsigned sync_phase_ = 0;
    int32_t clock_energy_[5] = {};
    int16_t clock_previous_ = 0;
    bool clock_update_ = false;
    int32_t positive_deviation_ = 0;
    int32_t negative_deviation_ = 0;
    int32_t positive_sum_ = 0;
    int32_t negative_sum_ = 0;
    unsigned positive_count_ = 0;
    unsigned negative_count_ = 0;
    unsigned warmup_ = 480; // Reference's 24,000 / 50 samples = 20 ms.
    unsigned phase_ = 0;
    unsigned sampling_phase_ = 0;
    unsigned symbols_ = 0;
    unsigned missed_syncs_ = 0;
    bool receiving_ = false;
    bool valid_sync_ = false;
    bool ending_ = false;
    Frame frame_;
};
} // namespace m17
} // namespace ht
