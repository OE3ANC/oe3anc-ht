// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/modem.hpp>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace ht::modem;

static void kiss_tests() {
    KissParser parser;
    KissMessage output;
    const uint8_t payload[] = {0x11, 0xc0, 0xdb, 0x22};
    const uint8_t encoded[] = {0xc0, 0, 0x11, 0xdb, 0xdc, 0xdb, 0xdd, 0x22, 0xc0};
    uint8_t buffer[2 * max_frame_size + 3] = {};
    assert(kiss_encode(payload, sizeof(payload), buffer, sizeof(buffer)) == sizeof(encoded));
    assert(memcmp(encoded, buffer, sizeof(encoded)) == 0);
    memset(buffer, 0x55, sizeof(buffer));
    assert(!kiss_encode(payload, sizeof(payload), buffer, sizeof(encoded) - 1));
    assert(buffer[0] == 0x55);
    for (size_t i = 0; i < sizeof(encoded); ++i) {
        assert(parser.feed(encoded[i], output) ==
               (i + 1 == sizeof(encoded) ? KissResult::Message : KissResult::None));
    }
    assert(output.frame.size == sizeof(payload));
    assert(!memcmp(output.frame.bytes, payload, sizeof(payload)));
    for (unsigned command = 0; command < 256; ++command) {
        parser.reset();
        parser.feed(0xc0, output);
        // Escape command bytes as a host must.
        if (command == 0xc0 || command == 0xdb) {
            parser.feed(0xdb, output);
            parser.feed(command == 0xc0 ? 0xdc : 0xdd, output);
        } else {
            parser.feed(command, output);
        }
        if (command != 255) {
            parser.feed(0xfe, output);
        }
        assert(parser.feed(0xc0, output) ==
               (command <= 5 || command == 255 ? KissResult::Message : KissResult::Rejected));
    }
    const uint8_t malformed[][5] = {{0xc0, 0, 0xdb, 1, 0xc0},
                                   {0xc0, 1, 2, 3, 0xc0},
                                   {0xc0, 0, 1, 0xdb, 0xc0}};
    for (const auto &bytes : malformed) {
        parser.reset();
        for (unsigned i = 0; i < 5; ++i) {
            assert(parser.feed(bytes[i], output) ==
                   (i == 4 ? KissResult::Rejected : KissResult::None));
        }
    }
    parser.reset();
    parser.feed(0xc0, output);
    for (unsigned i = 0; i < max_frame_size + 2; ++i) {
        parser.feed(0, output);
    }
    assert(parser.feed(0xc0, output) == KissResult::Rejected);
    parser.feed(0, output);
    parser.feed(42, output);
    assert(parser.feed(0xc0, output) == KissResult::Message);
    assert(output.frame.size == 1 && output.frame.bytes[0] == 42);
}

// Separate MSB-first polynomial implementation, reflected only at boundaries.
static uint16_t reference_crc(const std::vector<uint8_t> &bytes) {
    unsigned crc = 0xffff;
    for (uint8_t byte : bytes) {
        for (unsigned i = 0; i < 8; ++i) {
            const bool top = (crc & 0x8000) != 0;
            crc = (crc << 1) & 0xffff;
            if (top != bool((byte >> i) & 1)) {
                crc ^= 0x1021;
            }
        }
    }
    unsigned result = 0;
    for (unsigned i = 0; i < 16; ++i) {
        result = (result << 1) | ((crc >> i) & 1);
    }
    return result ^ 0xffff;
}

static std::vector<bool> wire_bits(std::vector<uint8_t> bytes, bool corrupt = false,
                                   unsigned lead = 40, unsigned tail = 8) {
    const uint16_t crc = reference_crc(bytes);
    bytes.push_back((crc & 0xff) ^ (corrupt ? 1 : 0));
    bytes.push_back(crc >> 8);
    std::vector<bool> bits;
    auto flags = [&bits](unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            for (bool bit : {false, true, true, true, true, true, true, false}) {
                bits.push_back(bit);
            }
        }
    };
    flags(lead);
    unsigned ones = 0;
    for (uint8_t byte : bytes) {
        for (unsigned j = 0; j < 8; ++j) {
            bool bit = (byte >> j) & 1;
            bits.push_back(bit);
            ones = bit ? ones + 1 : 0;
            if (ones == 5) {
                bits.push_back(false);
                ones = 0;
            }
        }
    }
    flags(tail);
    return bits;
}

static unsigned receive(const std::vector<uint8_t> &bytes, double clock_ratio,
                        double frequency_offset, double gain, double noise, double dc,
                        unsigned offset, bool corrupt = false,
                        const std::vector<bool> *raw_bits = nullptr) {
    Receiver rx;
    Frame output;
    const auto bits = raw_bits ? *raw_bits : wire_bits(bytes, corrupt);
    std::vector<bool> tones;
    bool mark = true;
    for (bool bit : bits) {
        mark = bit ? mark : !mark;
        tones.push_back(mark);
    }
    unsigned frames = 0;
    for (unsigned i = 0; i < offset; ++i) {
        assert(!rx.sample(0, output));
    }
    double phase = 1.723;
    uint32_t random = 123456;
    const double samples_per_bit = 40 * clock_ratio;
    for (size_t n = 0; n < tones.size() * samples_per_bit; ++n) {
        const size_t index = size_t(n / samples_per_bit);
        if (index >= tones.size()) {
            break;
        }
        const double freq = (tones[index] ? 1200 : 2200) + frequency_offset;
        random = random * 1664525u + 1013904223u;
        const double interference = (double(random) / 4294967295.0 * 2 - 1) * noise;
        const int16_t sample = int16_t(gain * sin(phase) + dc + interference);
        phase += 6.283185307179586 * freq / 48000;
        if (rx.sample(sample, output)) {
            ++frames;
            assert(output.size == bytes.size());
            assert(!memcmp(output.bytes, bytes.data(), bytes.size()));
        }
    }
    return frames;
}

// Classify each emitted TX symbol independently against the two Bell 202
// frequencies, then compare against the reference HDLC/CRC/NRZI sequence.
static void transmitter_wire_test() {
    Frame input;
    input.size = 9;
    memcpy(input.bytes, "123456789", input.size);
    const auto bits = wire_bits(std::vector<uint8_t>(input.bytes, input.bytes + input.size),
                                false, 2, 1);
    Transmitter tx;
    assert(tx.start(input, 0, 0, 10000));
    bool mark = true;
    for (bool bit : bits) {
        mark = bit ? mark : !mark;
        int16_t samples[40];
        assert(tx.render(samples, 40) == 40);
        double energy[2] = {};
        for (unsigned tone = 0; tone < 2; ++tone) {
            double ci = 0, cq = 0;
            for (unsigned n = 0; n < 40; ++n) {
                const double phase = 6.283185307179586 * (tone ? 2200 : 1200) * n / 48000;
                ci += samples[n] * cos(phase);
                cq += samples[n] * sin(phase);
                assert(samples[n] >= -10000 && samples[n] <= 10000);
            }
            energy[tone] = ci * ci + cq * cq;
        }
        assert((energy[0] > energy[1]) == mark);
    }
    assert(!tx.active());
}

static void cold_receiver_test() {
    Frame input;
    input.size = 3;
    input.bytes[0] = 1;
    input.bytes[1] = 2;
    input.bytes[2] = 3;
    for (unsigned offset : {0u, 100u, 1000u, 1001u, 1017u}) {
        Receiver rx;
        Frame output;
        for (unsigned i = 0; i < offset; ++i) {
            assert(!rx.sample(0, output));
        }
        Transmitter tx;
        assert(tx.start(input, 0, 0));
        unsigned frames = 0;
        int16_t samples[137];
        while (tx.active()) {
            const size_t count = tx.render(samples, 137);
            for (size_t i = 0; i < count; ++i) {
                if (rx.sample(samples[i], output)) {
                    ++frames;
                    assert(output.size == input.size);
                    assert(!memcmp(output.bytes, input.bytes, input.size));
                }
            }
        }
        // A final correlation window may extend beyond the last TX sample.
        for (unsigned i = 0; i < 80; ++i) {
            if (rx.sample(0, output)) {
                ++frames;
                assert(output.size == input.size);
                assert(!memcmp(output.bytes, input.bytes, input.size));
            }
        }
        assert(frames == 1);
    }
}

static void audio_tests() {
    const uint8_t check[] = "123456789";
    assert(fcs(check, 9) == 0x906e);
    std::vector<uint8_t> bytes(check, check + 9);
    assert(reference_crc(bytes) == 0x906e);
    // Addresses + UI control/PID, followed by all byte values: stresses long
    // runs, bit stuffing across octet boundaries and every payload value.
    bytes = {0x82, 0xa0, 0xa4, 0xa6, 0x40, 0x40, 0xe0,
             0x9c, 0x60, 0x86, 0x82, 0x98, 0x98, 0x61, 0x03, 0xf0};
    for (unsigned i = 0; i < 256; ++i) {
        bytes.push_back(i);
    }
    for (double rate : {0.99, 1.0, 1.01}) {
        for (double offset : {-40.0, 0.0, 40.0}) {
            assert(receive(bytes, rate, offset, 9000, 1800, 1500, 17) == 1);
        }
    }
    for (unsigned offset = 0; offset < 40; ++offset) {
        assert(receive(bytes, 1, 0, 2000, 100, -400, offset) == 1);
    }
    assert(receive(bytes, 1, 0, -12000, 0, 0, 0) == 1);
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0, true) == 0);
    auto aborted = wire_bits(bytes);
    aborted.insert(aborted.begin() + 400, 15, true);
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0, false, &aborted) == 0);
    const auto valid = wire_bits(bytes);
    aborted.insert(aborted.end(), valid.begin(), valid.end());
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0, false, &aborted) == 1);
    auto overflow = wire_bits(std::vector<uint8_t>(500, 0x55));
    overflow.insert(overflow.end(), valid.begin(), valid.end());
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0, false, &overflow) == 1);
    bytes.assign(max_frame_size, 0xff);
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0) == 1);
    bytes.push_back(0xff);
    assert(receive(bytes, 1, 0, 9000, 0, 0, 0) == 0);

    Transmitter tx;
    Frame input;
    input.size = max_frame_size;
    memset(input.bytes, 0xff, input.size);
    assert(!tx.start(input, 30, 0, 0));
    assert(tx.start(input, 255, 255));
    assert(!tx.start(input));
    Receiver rx;
    Frame output;
    unsigned frames = 0;
    int16_t samples[137]; // Deliberately not aligned to bits or audio blocks.
    while (tx.active()) {
        const size_t count = tx.render(samples, 137);
        assert(count && count <= 137);
        for (size_t i = 0; i < count; ++i) {
            if (rx.sample(samples[i], output)) {
                ++frames;
                assert(output.size == input.size);
                assert(!memcmp(output.bytes, input.bytes, input.size));
            }
        }
    }
    assert(frames == 1);
    assert(!tx.render(samples, 137));
    tx.reset();
    assert(!tx.active());
    rx.reset();
    for (unsigned i = 0; i < 48000; ++i) {
        assert(!rx.sample(0, output));
    }
    assert(!rx.carrier());
    uint32_t random = 761;
    for (unsigned i = 0; i < 480000; ++i) {
        random = random * 1664525u + 1013904223u;
        assert(!rx.sample(int16_t(random >> 16), output));
    }
    rx.reset();
    assert(!rx.carrier());
    printf("sizes: parser=%zu tx=%zu rx=%zu frame=%zu\n", sizeof(KissParser),
           sizeof(Transmitter), sizeof(Receiver), sizeof(Frame));
}

int main() {
    kiss_tests();
    transmitter_wire_test();
    cold_receiver_test();
    audio_tests();
    puts("KISS/HDLC/Bell202 checks passed");
}
