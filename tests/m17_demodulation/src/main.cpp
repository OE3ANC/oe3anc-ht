// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../m17/vectors/golden.hpp"
#include "../vectors/golden.hpp"
#include <ht/m17_modem.hpp>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht::m17;

static Payload voice_payload(unsigned number) {
    Payload payload;
    for (unsigned i = 0; i < 16; ++i)
        payload.bytes[i] = number * 17 + i * 19;
    return payload;
}

static Payload random_payload(unsigned number) {
    Payload payload;
    uint32_t state = 0x58964ac1u + number;
    for (uint8_t &byte : payload.bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = state;
    }
    return payload;
}

ZTEST(demodulation, test_independent_adc_fixture_and_output_ownership) {
    Demodulator receiver;
    Decoder decoder;

    struct {
        uint32_t before = 0x55648712;
        Frame frame;
        uint32_t after = 0xff104724;
    } output;

    unsigned links = 0, voices = 0, endings = 0;
    auto receive = [&](int16_t input) {
        const Frame previous = output.frame;
        if (!receiver.sample(input, output.frame)) {
            zassert_mem_equal(output.frame.bytes, previous.bytes, 48);
            return;
        }
        zassert_equal(output.before, 0x55648712);
        zassert_equal(output.after, 0xff104724);
        const DecodedFrame decoded = decoder.decode(output.frame);
        if (decoded.kind == FrameKind::LinkSetup) {
            ++links;
            zassert_mem_equal(output.frame.bytes, lsf_frame, 48);
            zassert_true(decoded.link_updated);
        } else if (decoded.kind == FrameKind::Stream) {
            ++voices;
            zassert_mem_equal(output.frame.bytes, stream_0, 48);
            zassert_true(decoded.payload_valid);
            zassert_equal(decoded.number, 0);
            for (unsigned i = 0; i < 16; ++i)
                zassert_equal(decoded.payload.bytes[i], i);
        } else if (decoded.kind == FrameKind::End)
            ++endings;
        else
            zassert_unreachable("Unexpected frame %u", unsigned(decoded.kind));
    };
    for (int16_t sample : reference_adc)
        receive(sample);
    // Flush the filters' latency, without inventing another transmitted frame.
    for (unsigned i = 0; i < 960; ++i)
        receive(0);
    zassert_equal(links, 1);
    zassert_equal(voices, 1);
    zassert_equal(endings, 1);
    zassert_false(receiver.locked());
}

struct RadioLink {
    Demodulator receiver;
    Decoder decoder;
    Modulator transmitter;
    TxSamples waveform;
    unsigned voices = 0;
    unsigned links = 0;
    unsigned endings = 0;
    unsigned resample_phase = 0;
    int previous_sample = 0;
    uint32_t noise = 0x59355f7;
    int drift = 0;
    bool invert = false;
    bool random_voice = false;

    void receive(int16_t sample) {
        Frame output;
        if (!receiver.sample(sample, output, invert))
            return;
        const DecodedFrame decoded = decoder.decode(output);
        if (decoded.link_updated) {
            zassert_true(voice_stream(decoded.link));
            ++links;
        }
        if (decoded.payload_valid) {
            const Payload expected =
                random_voice ? random_payload(decoded.number) : voice_payload(decoded.number);
            zassert_mem_equal(decoded.payload.bytes, expected.bytes, sizeof(expected.bytes),
                              "Drift %d, frame %u, errors %u", drift, decoded.number,
                              decoded.errors);
            ++voices;
        }
        if (decoded.kind == FrameKind::End)
            ++endings;
    }

    void send(const Frame &frame) {
        transmitter.render(frame, waveform);
        for (unsigned i = 0; i < 1920; i += 2) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            int value = waveform.samples[i] / 6 + 1000 + int(noise % 201) - 100;
            if (invert)
                value = -value;
            // Resample continuously at +/-83 ppm, interpolating each sample
            // instead of creating discontinuities by dropping/repeating PCM.
            while (resample_phase < 12000) {
                receive(previous_sample + (value - previous_sample) * int(resample_phase) / 12000);
                resample_phase += 12000 + drift;
            }
            resample_phase -= 12000;
            previous_sample = value;
        }
    }
};

ZTEST(demodulation, test_random_voice_with_matching_clocks) {
    RadioLink link;
    link.random_voice = true;
    Encoder encoder;
    Frame frame;
    LinkSetup setup;
    zassert_true(make_voice_link("OE3ANC", 6, setup));
    preamble(frame);
    link.send(frame);
    link.send(frame);
    zassert_true(encoder.start(setup, frame));
    link.send(frame);
    for (unsigned number = 0; number < 1000; ++number) {
        zassert_true(encoder.stream(random_payload(number), number == 999, frame));
        link.send(frame);
    }
    end_marker(frame);
    link.send(frame);
    for (unsigned i = 0; i < 960; ++i)
        link.receive(0);
    zassert_equal(link.voices, 1000);
    zassert_equal(link.endings, 1);
    zassert_false(link.receiver.locked());
}

ZTEST(demodulation, test_sustained_voice_with_noise_dc_polarity_and_clock_drift) {
    for (int drift = -1; drift <= 1; ++drift) {
        RadioLink link;
        link.drift = drift;
        link.invert = drift != 0;
        Encoder encoder;
        Frame frame;
        LinkSetup setup;
        zassert_true(make_voice_link("OE3ANC", 6, setup));
        preamble(frame);
        link.send(frame);
        link.send(frame);
        zassert_true(encoder.start(setup, frame));
        link.send(frame);
        for (unsigned number = 0; number < 400; ++number) {
            const Payload payload = voice_payload(number);
            zassert_true(encoder.stream(payload, number == 399, frame));
            link.send(frame);
        }
        end_marker(frame);
        link.send(frame);
        for (unsigned i = 0; i < 960; ++i)
            link.receive(0);
        zassert_equal(link.voices, 400, "Drift %d", drift);
        zassert_true(link.links > 1, "Direct LSF and LICH late-entry updates");
        zassert_equal(link.endings, 1);
        zassert_false(link.receiver.locked());
    }
}

ZTEST(demodulation, test_signal_loss_late_entry_reset_and_independent_instances) {
    RadioLink link;
    RadioLink other;
    Encoder encoder;
    LinkSetup setup;
    Frame frame;
    zassert_true(make_voice_link("OE3ANC", 6, setup));
    zassert_true(encoder.start(setup, frame));
    // Listen after LSF: recover the sender through the six LICH chunks.
    for (unsigned number = 0; number < 16; ++number) {
        zassert_true(encoder.stream(voice_payload(number), false, frame));
        link.send(frame);
    }
    zassert_true(link.receiver.locked());
    zassert_true(link.voices >= 12);
    zassert_true(link.links > 0);
    for (unsigned i = 0; i < 6 * 960; ++i)
        link.receive(0);
    zassert_false(link.receiver.locked());
    zassert_false(other.receiver.locked());
    // A fresh preamble and LSF must reacquire without a software restart.
    const unsigned previous_links = link.links;
    preamble(frame);
    link.send(frame);
    link.send(frame);
    zassert_true(encoder.start(setup, frame));
    link.send(frame);
    for (unsigned i = 0; i < 4; ++i) {
        zassert_true(encoder.stream(voice_payload(i), false, frame));
        link.send(frame);
    }
    zassert_true(link.links > previous_links);
    link.receiver.reset();
    zassert_false(link.receiver.locked());
    // Reset removes every filter, correlation, timing and partial-frame state.
    for (int16_t input : reference_adc) {
        Frame first, second;
        const bool a = link.receiver.sample(input, first);
        const bool b = other.receiver.sample(input, second);
        zassert_equal(a, b);
        zassert_equal(link.receiver.locked(), other.receiver.locked());
        if (a)
            zassert_mem_equal(first.bytes, second.bytes, 48);
    }
}

ZTEST(demodulation, test_full_scale_noise_and_constant_input_are_bounded) {
    Demodulator receiver;
    Decoder decoder;
    Frame output;
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        receiver.reset();
        decoder.reset();
        uint32_t noise = 0x595612;
        for (unsigned i = 0; i < 240000; ++i) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            const int16_t value = pattern == 0   ? 32767
                                  : pattern == 1 ? -32768
                                  : pattern == 2 ? (i % 2 ? 32767 : -32768)
                                                 : static_cast<int16_t>(noise);
            if (receiver.sample(value, output, i % 2 != 0))
                zassert_false(decoder.decode(output).link_updated, "Noise published a valid LSF");
        }
    }
}

ZTEST_SUITE(demodulation, nullptr, nullptr, nullptr, nullptr, nullptr);
