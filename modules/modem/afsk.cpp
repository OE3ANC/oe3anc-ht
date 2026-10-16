// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/modem.hpp>
#include <math.h>
#include <string.h>

namespace ht {
namespace modem {
namespace {
static_assert((max_frame_size + 2) * 8 * 6 / 5 + 8 <= 3200, "RX bit storage");
static_assert((max_frame_size + 2) * 8 * 6 / 5 + 2 * 384 * 8 <= 9600, "TX bit storage");
constexpr float tau = 6.2831853071795864769f;
constexpr float mark_i[40] = {
    1.000000000f, 0.987688341f, 0.951056516f, 0.891006524f, 0.809016994f,
    0.707106781f, 0.587785252f, 0.453990500f, 0.309016994f, 0.156434465f,
    0.000000000f, -0.156434465f, -0.309016994f, -0.453990500f, -0.587785252f,
    -0.707106781f, -0.809016994f, -0.891006524f, -0.951056516f, -0.987688341f,
    -1.000000000f, -0.987688341f, -0.951056516f, -0.891006524f, -0.809016994f,
    -0.707106781f, -0.587785252f, -0.453990500f, -0.309016994f, -0.156434465f,
    -0.000000000f, 0.156434465f, 0.309016994f, 0.453990500f, 0.587785252f,
    0.707106781f, 0.809016994f, 0.891006524f, 0.951056516f, 0.987688341f,
};
constexpr float mark_q[40] = {
    0.000000000f, 0.156434465f, 0.309016994f, 0.453990500f, 0.587785252f,
    0.707106781f, 0.809016994f, 0.891006524f, 0.951056516f, 0.987688341f,
    1.000000000f, 0.987688341f, 0.951056516f, 0.891006524f, 0.809016994f,
    0.707106781f, 0.587785252f, 0.453990500f, 0.309016994f, 0.156434465f,
    0.000000000f, -0.156434465f, -0.309016994f, -0.453990500f, -0.587785252f,
    -0.707106781f, -0.809016994f, -0.891006524f, -0.951056516f, -0.987688341f,
    -1.000000000f, -0.987688341f, -0.951056516f, -0.891006524f, -0.809016994f,
    -0.707106781f, -0.587785252f, -0.453990500f, -0.309016994f, -0.156434465f,
};
constexpr float space_i[40] = {
    1.000000000f, 0.958819735f, 0.838670568f, 0.649448048f, 0.406736643f,
    0.130526192f, -0.156434465f, -0.430511097f, -0.669130606f, -0.852640164f,
    -0.965925826f, -0.999657325f, -0.951056516f, -0.824126189f, -0.629320391f,
    -0.382683432f, -0.104528463f, 0.182235525f, 0.453990500f, 0.688354576f,
    0.866025404f, 0.972369920f, 0.998629535f, 0.942641491f, 0.809016994f,
    0.608761429f, 0.358367950f, 0.078459096f, -0.207911691f, -0.477158760f,
    -0.707106781f, -0.878817113f, -0.978147601f, -0.996917334f, -0.933580426f,
    -0.793353340f, -0.587785252f, -0.333806859f, -0.052335956f, 0.233445364f,
};
constexpr float space_q[40] = {
    0.000000000f, 0.284015345f, 0.544639035f, 0.760405966f, 0.913545458f,
    0.991444861f, 0.987688341f, 0.902585284f, 0.743144825f, 0.522498565f,
    0.258819045f, -0.026176948f, -0.309016994f, -0.566406237f, -0.777145961f,
    -0.923879533f, -0.994521895f, -0.983254908f, -0.891006524f, -0.725374371f,
    -0.500000000f, -0.233445364f, 0.052335956f, 0.333806859f, 0.587785252f,
    0.793353340f, 0.933580426f, 0.996917334f, 0.978147601f, 0.878817113f,
    0.707106781f, 0.477158760f, 0.207911691f, -0.078459096f, -0.358367950f,
    -0.608761429f, -0.809016994f, -0.942641491f, -0.998629535f, -0.972369920f,
};
} // namespace

uint16_t fcs(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc ^ 0xffff;
}

void Transmitter::reset() {
    bit_count_ = 0;
    bit_position_ = 0;
    sample_position_ = 0;
    phase_ = 0;
    mark_ = true;
}

void Transmitter::append(bool value) {
    bits_[bit_count_++] = value;
}

void Transmitter::flag() {
    for (unsigned i = 0; i < 8; ++i) {
        append((0x7e >> i) & 1);
    }
}

bool Transmitter::start(const Frame &frame, uint8_t txdelay, uint8_t txtail,
                        int16_t amplitude) {
    if (active() || !frame.size || frame.size > max_frame_size || amplitude <= 0) {
        return false;
    }
    reset();
    amplitude_ = amplitude;
    // Round upwards to whole flags. Two opening flags (13.3 ms) let a cold
    // receiver acquire the tone and clock even when TXDELAY is zero.
    const unsigned delay_flags = (unsigned(txdelay) * 12 + 7) / 8 + 1;
    const unsigned preamble = delay_flags < 2 ? 2 : delay_flags;
    const unsigned tail = (unsigned(txtail) * 12 + 7) / 8 + 1;
    for (unsigned i = 0; i < preamble; ++i) {
        flag();
    }
    const uint16_t crc = fcs(frame.bytes, frame.size);
    unsigned ones = 0;
    for (size_t i = 0; i < frame.size + 2; ++i) {
        const uint8_t byte = i < frame.size
                                 ? frame.bytes[i]
                                 : static_cast<uint8_t>(crc >> (8 * (i - frame.size)));
        for (unsigned j = 0; j < 8; ++j) {
            const bool value = (byte >> j) & 1;
            append(value);
            ones = value ? ones + 1 : 0;
            if (ones == 5) {
                append(false);
                ones = 0;
            }
        }
    }
    for (unsigned i = 0; i < tail; ++i) {
        flag();
    }
    return true;
}

size_t Transmitter::render(int16_t *output, size_t capacity) {
    if (!output) {
        return 0;
    }
    size_t count = 0;
    while (count < capacity && active()) {
        if (sample_position_ == 0 && !bits_[bit_position_]) {
            mark_ = !mark_;
        }
        output[count++] = static_cast<int16_t>(amplitude_ * sinf(phase_));
        phase_ += tau * (mark_ ? 1200.0f : 2200.0f) / sample_rate;
        if (phase_ >= tau) {
            phase_ -= tau;
        }
        if (++sample_position_ == 40) {
            sample_position_ = 0;
            ++bit_position_;
        }
    }
    return count;
}

void Receiver::reset() {
    memset(history_, 0, sizeof(history_));
    position_ = 0;
    filled_ = 0;
    energy_ = 0;
    dc_ = 0;
    clock_ = 0;
    tone_ = false;
    sampled_tone_ = false;
    tone_valid_ = false;
    shift_ = 0;
    bit_count_ = 0;
    collecting_ = false;
}

bool Receiver::decode(size_t count, Frame &output) {
    uint8_t bytes[max_frame_size + 2] = {};
    size_t decoded = 0;
    unsigned ones = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool value = bits_[i];
        if (ones == 5) {
            if (value) {
                return false;
            }
            ones = 0;
            continue;
        }
        if (decoded == sizeof(bytes) * 8) {
            return false;
        }
        bytes[decoded / 8] |= uint8_t(value) << (decoded % 8);
        ++decoded;
        ones = value ? ones + 1 : 0;
    }
    if (decoded % 8 || decoded < 24 || ones == 5) {
        return false;
    }
    const size_t size = decoded / 8 - 2;
    const uint16_t received = bytes[size] | (uint16_t(bytes[size + 1]) << 8);
    if (received != fcs(bytes, size)) {
        return false;
    }
    memcpy(output.bytes, bytes, size);
    output.size = size;
    return true;
}

bool Receiver::bit(bool value, Frame &output) {
    shift_ = (shift_ >> 1) | (uint8_t(value) << 7);
    if (collecting_) {
        if (bit_count_ < sizeof(bits_)) {
            bits_[bit_count_++] = value;
        } else {
            collecting_ = false;
        }
    }
    if (shift_ == 0x7e) {
        const bool valid = collecting_ && bit_count_ >= 8 && decode(bit_count_ - 8, output);
        bit_count_ = 0;
        collecting_ = true;
        return valid;
    }
    return false;
}

bool Receiver::sample(int16_t input, Frame &output) {
    // Slow DC removal prevents a microphone/radio DC offset biasing the tones.
    dc_ += 0.001f * (input - dc_);
    const float value = input - dc_;
    energy_ += 0.002f * (value * value - energy_);
    history_[position_] = value;
    position_ = (position_ + 1) % 40;
    if (filled_ < 40) {
        ++filled_;
        return false;
    }
    float mi = 0, mq = 0, si = 0, sq = 0;
    for (unsigned i = 0; i < 40; ++i) {
        const float sample = history_[(position_ + i) % 40];
        mi += sample * mark_i[i];
        mq += sample * mark_q[i];
        si += sample * space_i[i];
        sq += sample * space_q[i];
    }
    const bool tone = mi * mi + mq * mq > si * si + sq * sq;
    if (!tone_valid_) {
        tone_ = tone;
        sampled_tone_ = tone;
        tone_valid_ = true;
    }
    // The matched-window decision is delayed by half a symbol. A transition
    // marks a symbol boundary in that delayed stream; sample halfway between
    // transitions. Correct a fraction of the phase error to tolerate noise.
    if (tone != tone_) {
        float error = clock_ < 20.0f ? clock_ : clock_ - 40.0f;
        clock_ -= error * 0.5f;
        if (clock_ < 0) {
            clock_ += 40.0f;
        }
        tone_ = tone;
    }
    const float previous = clock_;
    clock_ += 1.0f;
    bool valid = false;
    if (previous < 20.0f && clock_ >= 20.0f) {
        valid = bit(tone == sampled_tone_, output);
        sampled_tone_ = tone;
    }
    if (clock_ >= 40.0f) {
        clock_ -= 40.0f;
    }
    return valid;
}
} // namespace modem
} // namespace ht
