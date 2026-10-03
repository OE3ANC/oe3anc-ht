/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from OpenRTX Demodulator, Correlator, Synchronizer, ClockRecovery,
 * DevEstimator, DSP, FIR/IIR and DC blocker at reference commit
 * 9d800e69f5f3c7275857c232c32786e6526ead80.
 * FIR/IIR originally adapted from Rob Riggs, Mobilinkd LLC.
 * DC blocker: dspGuru fixed-point DC-blocking filter with noise shaping,
 * https://dspguru.com/dsp/tricks/fixed-point-dc-blocking-filter-with-noise-shaping/
 */
#include <ht/m17_modem.hpp>
#include <stdlib.h>
#include <string.h>

namespace ht {
namespace m17 {
namespace {
constexpr float taps[41] = {
    -0.002021130037130002, -0.001227380092907312, 0.000978411066065117,  0.003011674298801149,
    0.003051422479929027,  0.000606339011138998,  -0.002680772347838965, -0.003889744583281823,
    -0.001077818873364855, 0.004563508234396922,  0.008488746155946006,  0.005343941074480147,
    -0.006789617306671533, -0.023083267913613266, -0.032241823935683658, -0.021221865389865011,
    0.017516745763008643,  0.080124798723015214,  0.150574288667793071,  0.206204943905808818,
    0.227336876945597260,  0.206204943905808818,  0.150574288667793071,  0.080124798723015214,
    0.017516745763008643,  -0.021221865389865011, -0.032241823935683658, -0.023083267913613266,
    -0.006789617306671533, 0.005343941074480147,  0.008488746155946006,  0.004563508234396922,
    -0.001077818873364855, -0.003889744583281823, -0.002680772347838965, 0.000606339011138998,
    0.003051422479929027,  0.003011674298801149,  0.000978411066065117,  -0.001227380092907312,
    -0.002021130037130002,
};
constexpr int8_t sync_symbols[8] = {3, 3, 3, 3, -3, -3, 3, -3};

int16_t saturate(float value) {
    if (value > 32767.0f)
        return 32767;
    if (value < -32768.0f)
        return -32768;
    return static_cast<int16_t>(value);
}
} // namespace

void Demodulator::reset() {
    *this = Demodulator{};
}

int16_t Demodulator::filter(int16_t input, bool invert) {
    // Preserve the fixed-point pole (0.995) and negative floor rounding,
    // without shifting negative integers or overflowing on full-scale edges.
    const int32_t scaled = int32_t(input) * 32768;
    dc_accumulator_ += int64_t(scaled) - dc_previous_input_ - 164 * int64_t(dc_previous_output_);
    dc_previous_input_ = scaled;
    dc_previous_output_ = static_cast<int32_t>(
        dc_accumulator_ >= 0 ? dc_accumulator_ / 32768 : -((-dc_accumulator_ + 32767) / 32768));
    float value = saturate(static_cast<float>(dc_previous_output_));
    if (invert)
        value = -value;
    filter_position_ = filter_position_ == 0 ? 40 : filter_position_ - 1;
    filter_history_[filter_position_] = value;
    filter_history_[filter_position_ + 41] = value;
    float sum = 0;
    for (unsigned i = 0; i < 40; i += 4) {
        sum += filter_history_[filter_position_ + i] * taps[i];
        sum += filter_history_[filter_position_ + i + 1] * taps[i + 1];
        sum += filter_history_[filter_position_ + i + 2] * taps[i + 2];
        sum += filter_history_[filter_position_ + i + 3] * taps[i + 3];
    }
    return saturate(sum + filter_history_[filter_position_ + 40] * taps[40]);
}

float Demodulator::envelope(int16_t input) {
    constexpr float numerator[] = {4.24433681e-05f, 8.48867363e-05f, 4.24433681e-05f};
    constexpr float denominator[] = {1.0f, -1.98148851f, 0.98165828f};
    unsigned index = envelope_position_;
    float sum = 0, feedback = 0;
    for (unsigned i = 1; i < 3; ++i) {
        index = index == 0 ? 2 : index - 1;
        sum += envelope_history_[index] * numerator[i];
        feedback += envelope_history_[index] * denominator[i];
    }
    const float value = abs(int(input)) - feedback;
    sum += numerator[0] * value;
    envelope_history_[envelope_position_] = value;
    envelope_position_ = (envelope_position_ + 1) % 3;
    return sum;
}

bool Demodulator::synchronize(float threshold) {
    int32_t correlation = 0;
    // Next write position is the oldest sample; current phase is the newest.
    unsigned position = (correlation_position_ + 4) % 40;
    for (int8_t symbol : sync_symbols) {
        correlation += symbol * int32_t(correlation_[position]);
        position = (position + 5) % 40;
    }
    const int32_t limit = static_cast<int32_t>(threshold * 33.0f);
    if (abs(correlation) > limit && abs(correlation) > 0) {
        if (!sync_triggered_)
            memset(sync_peaks_, 0, sizeof(sync_peaks_));
        sync_triggered_ = true;
        sync_peaks_[phase_] = correlation;
        return false;
    }
    if (!sync_triggered_)
        return false;
    sync_triggered_ = false;
    int32_t peak = abs(correlation);
    sync_phase_ = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (abs(sync_peaks_[i]) > peak) {
            peak = abs(sync_peaks_[i]);
            sync_phase_ = i;
        }
    }
    return true;
}

bool Demodulator::sync_matches(uint16_t word, unsigned tolerance) const {
    unsigned bits = (uint16_t(frame_.bytes[0]) << 8 | frame_.bytes[1]) ^ word;
    unsigned errors = 0;
    while (bits) {
        bits &= bits - 1;
        ++errors;
    }
    return errors <= tolerance;
}

void Demodulator::quantize(int16_t input) {
    const unsigned dibit = input > (2 * positive_deviation_) / 3   ? 1
                           : input < (2 * negative_deviation_) / 3 ? 3
                           : input > 0                             ? 0
                                                                   : 2;
    const unsigned shift = 6 - 2 * (symbols_ % 4);
    uint8_t &byte = frame_.bytes[symbols_ / 4];
    byte = (byte & ~(3u << shift)) | dibit << shift;
    ++symbols_;
}

void Demodulator::acquire() {
    positive_sum_ = negative_sum_ = 0;
    positive_count_ = negative_count_ = 0;
    const unsigned newest = (correlation_position_ + 39) % 40;
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned position = (correlation_position_ + i) % 40;
        if (position % 5 != sync_phase_)
            continue;
        const int16_t value = correlation_[position];
        if (value > 0) {
            positive_sum_ += value;
            ++positive_count_;
        }
        if (value < 0) {
            negative_sum_ += value;
            ++negative_count_;
        }
    }
    if (!positive_count_ || !negative_count_)
        return;
    positive_deviation_ = positive_sum_ / int(positive_count_);
    negative_deviation_ = negative_sum_ / int(negative_count_);
    positive_sum_ = negative_sum_ = 0;
    positive_count_ = negative_count_ = 0;
    symbols_ = 0;
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned position = (correlation_position_ + i) % 40;
        if (position % 5 == sync_phase_)
            quantize(correlation_[position]);
    }
    if (!sync_matches(0x55f7, 0) && !sync_matches(0xff5d, 0))
        return;
    sampling_phase_ = sync_phase_;
    receiving_ = valid_sync_ = true;
    ending_ = false;
    missed_syncs_ = 0;
    memset(clock_energy_, 0, sizeof(clock_energy_));
    clock_previous_ = correlation_[newest];
    clock_update_ = false;
}

void Demodulator::track_clock(int16_t input) {
    int32_t delta = int32_t(input) - clock_previous_;
    if (int32_t(input) + clock_previous_ < 0)
        delta = -delta;
    clock_energy_[phase_] += delta;
    clock_previous_ = input;
}

void Demodulator::update_clock() {
    bool positive = false;
    unsigned index = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (!positive && clock_energy_[i] > 0)
            positive = true;
        else if (positive && clock_energy_[i] < 0) {
            index = i;
            break;
        }
    }
    sampling_phase_ = index == 0 ? 4 : index - 1;
    memset(clock_energy_, 0, sizeof(clock_energy_));
    clock_update_ = false;
}

void Demodulator::update_deviation() {
    if (positive_count_ && negative_count_) {
        const int32_t positive = positive_sum_ / int(positive_count_);
        const int32_t negative = negative_sum_ / int(negative_count_);
        const int32_t offset = (positive + negative) / 2;
        positive_deviation_ = positive - offset;
        negative_deviation_ = negative - offset;
    }
    positive_sum_ = negative_sum_ = 0;
    positive_count_ = negative_count_ = 0;
}

void Demodulator::lose_lock() {
    receiving_ = false;
    clock_update_ = false;
    sync_triggered_ = false;
    symbols_ = 0;
}

bool Demodulator::sample(int16_t input, Frame &output, bool invert) {
    const int16_t value = filter(input, invert);
    correlation_[correlation_position_] = value;
    correlation_position_ = (correlation_position_ + 1) % 40;
    const float threshold = envelope(value);
    bool ready = false;
    if (warmup_)
        --warmup_;
    else if (!receiving_) {
        if (synchronize(threshold))
            acquire();
    } else {
        // Update away from the previous sample so a phase wrap cannot sample
        // the same symbol twice. Clock energy is bounded to about one frame.
        if (clock_update_ && abs(int(sampling_phase_) - int(phase_)) == 2)
            update_clock();
        track_clock(value);
        if (phase_ == sampling_phase_) {
            quantize(value);
            if (value > (2 * positive_deviation_) / 3) {
                positive_sum_ += value;
                ++positive_count_;
            }
            if (value < (2 * negative_deviation_) / 3) {
                negative_sum_ += value;
                ++negative_count_;
            }
            if (symbols_ == 8) {
                valid_sync_ = sync_matches(0x55f7, 1) || sync_matches(0xff5d, 1);
                ending_ = sync_matches(0x555d, 1);
                if (valid_sync_ || ending_)
                    missed_syncs_ = 0;
                else if (++missed_syncs_ >= 5)
                    lose_lock();
            }
            if (symbols_ == 192) {
                ready = valid_sync_ || ending_;
                if (ready)
                    output = frame_;
                symbols_ = 0;
                update_deviation();
                clock_update_ = true;
                if (ending_)
                    lose_lock();
            }
        }
    }
    phase_ = (phase_ + 1) % 5;
    return ready;
}
} // namespace m17
} // namespace ht
