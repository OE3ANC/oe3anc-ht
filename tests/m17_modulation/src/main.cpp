// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../m17/vectors/golden.hpp"
#include "../vectors/golden.hpp"
#include <ht/m17_modem.hpp>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht::m17;

static uint32_t checksum(const TxSamples &wave) {
    uint32_t hash = 2166136261u;
    for (int16_t sample : wave.samples) {
        const uint16_t value = sample;
        hash = (hash ^ uint8_t(value)) * 16777619u;
        hash = (hash ^ uint8_t(value >> 8)) * 16777619u;
    }
    return hash;
}

ZTEST(modulation, test_reference_waveforms_and_buffer_ownership) {
    Modulator modulator;
    Frame frames[5];
    preamble(frames[0]);
    preamble(frames[1]);
    memcpy(frames[2].bytes, lsf_frame, 48);
    memcpy(frames[3].bytes, stream_0, 48);
    end_marker(frames[4]);
    for (unsigned frame = 0; frame < 5; ++frame) {
        struct {
            uint32_t before = 0x81045673;
            TxSamples wave;
            uint32_t after = 0x92501742;
        } output;

        const Frame saved = frames[frame];
        modulator.render(frames[frame], output.wave);
        zassert_equal(output.before, 0x81045673);
        zassert_equal(output.after, 0x92501742);
        zassert_mem_equal(frames[frame].bytes, saved.bytes, sizeof(saved.bytes));
        zassert_equal(checksum(output.wave), waveform_hashes[frame], "Frame %u", frame);
        for (unsigned probe = 0; probe < 18; ++probe)
            zassert_equal(output.wave.samples[probe_positions[probe]],
                          waveform_probes[frame][probe], "Frame %u, sample %u", frame,
                          probe_positions[probe]);
    }
}

ZTEST(modulation, test_stream_history_reset_and_instance_isolation) {
    Modulator first;
    Modulator second;
    Frame training;
    memset(training.bytes, 0x55, sizeof(training.bytes)); // All +3 symbols.
    TxSamples wave;
    first.render(training, wave);
    first.render(training, wave);
    Frame header;
    preamble(header);
    TxSamples fresh;
    second.render(header, fresh);
    zassert_equal(checksum(fresh), waveform_hashes[0]);
    first.render(header, wave);
    zassert_not_equal(checksum(wave), checksum(fresh), "Previous frame tail was discarded");
    first.reset();
    first.render(header, wave);
    zassert_mem_equal(wave.samples, fresh.samples, sizeof(wave.samples));
    // Modulating the next frame must carry the tail through the 40 ms boundary.
    first.render(header, wave);
    zassert_equal(checksum(wave), waveform_hashes[1]);
}

ZTEST(modulation, test_phase_extrema_and_integer_range) {
    // Bound all possible symbol sequences using the absolute tap sum at each
    // of the ten interpolation phases. The directed frames attain each bound.
    Modulator modulator;
    TxSamples wave;
    for (unsigned phase = 0; phase < 10; ++phase) {
        Frame input;
        memset(input.bytes, 0x55, sizeof(input.bytes));
        for (unsigned symbol = 0; symbol <= 8; ++symbol) {
            const unsigned k = 8 - symbol;
            const unsigned dibit = phase_signs[phase] & (1u << k) ? 1 : 3;
            const unsigned shift = 6 - 2 * (symbol % 4);
            input.bytes[symbol / 4] &= ~(3u << shift);
            input.bytes[symbol / 4] |= dibit << shift;
        }
        modulator.reset();
        modulator.render(input, wave);
        zassert_equal(wave.samples[80 + phase], phase_peaks[phase], "Phase %u", phase);
    }
    // Noise streams exercise both signs and the complete ring-buffer lifetime.
    uint32_t random = 0x59350817;
    for (unsigned frame = 0; frame < 40; ++frame) {
        Frame input;
        for (uint8_t &byte : input.bytes) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            byte = random;
        }
        modulator.render(input, wave);
        for (int16_t sample : wave.samples)
            zassert_true(sample >= -31859 && sample <= 31859);
    }
}

ZTEST_SUITE(modulation, nullptr, nullptr, nullptr, nullptr, nullptr);
