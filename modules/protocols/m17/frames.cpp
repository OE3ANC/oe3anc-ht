/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from OpenRTX M17 Callsign, LinkSetupFrame, Golay, FrameEncoder,
 * FrameDecoder, ConvolutionalEncoder, Viterbi, Interleaver and Decorrelator.
 * Reference commit: 9d800e69f5f3c7275857c232c32786e6526ead80.
 */
#include <ht/m17.hpp>
#include <string.h>

namespace ht {
namespace m17 {
namespace {
constexpr char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
constexpr uint64_t address_limit = 262144000000000ULL; // 40^9
constexpr uint8_t lsf_puncture[] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
                                    1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
                                    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1};
constexpr uint8_t stream_puncture[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
constexpr uint8_t randomizer[46] = {
    0xd6, 0xb5, 0xe2, 0x30, 0x82, 0xff, 0x84, 0x62, 0xba, 0x4e, 0x96, 0x90, 0xd8, 0x98, 0xdd, 0x5d,
    0x0c, 0xc8, 0x52, 0x43, 0x91, 0x1d, 0xf8, 0x6e, 0x68, 0x2f, 0x35, 0xda, 0x14, 0xea, 0xcd, 0x76,
    0x19, 0x8d, 0xd5, 0x80, 0xd1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2d, 0x29, 0x78, 0xc3};
constexpr uint16_t golay_matrix[] = {0x8eb, 0x93e, 0xa97, 0xdc6, 0x367, 0x6cd,
                                     0xd99, 0x3da, 0x7b4, 0xf68, 0x63b, 0xc75};
constexpr uint16_t golay_inverse[] = {0xc75, 0x49f, 0x93e, 0x6e3, 0xdc6, 0xf13,
                                      0xab9, 0x1ed, 0x3da, 0x7b4, 0xf68, 0xa4f};

bool bit(const uint8_t *data, size_t position) {
    return (data[position / 8] >> (7 - position % 8)) & 1;
}

void set_bit(uint8_t *data, size_t position, bool value) {
    const uint8_t mask = 0x80 >> (position % 8);
    if (value)
        data[position / 8] |= mask;
    else
        data[position / 8] &= ~mask;
}

uint16_t crc(const uint8_t *data, size_t length) {
    constexpr uint16_t table[] = {0x0000, 0x5935, 0xb26a, 0xeb5f, 0x3de1, 0x64d4, 0x8f8b, 0xd6be,
                                  0x7bc2, 0x22f7, 0xc9a8, 0x909d, 0x4623, 0x1f16, 0xf449, 0xad7c};
    uint16_t value = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        value = (value << 4) ^ table[((value >> 12) ^ (data[i] >> 4)) & 15];
        value = (value << 4) ^ table[((value >> 12) ^ data[i]) & 15];
    }
    return value;
}

uint16_t matrix_product(uint16_t data, const uint16_t (&matrix)[12]) {
    uint16_t result = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (data & (1u << i))
            result ^= matrix[i];
    return result;
}

uint32_t golay_encode(uint16_t data) {
    return (uint32_t(data) << 12) | matrix_product(data, golay_matrix);
}

uint16_t golay_decode(uint32_t code) {
    const uint16_t data = code >> 12;
    const uint16_t syndrome = (code & 0xfff) ^ matrix_product(data, golay_matrix);
    if (__builtin_popcount(syndrome) <= 3)
        return data;
    for (unsigned i = 0; i < 12; ++i)
        if (__builtin_popcount(syndrome ^ golay_matrix[i]) <= 2)
            return data ^ (1u << i);
    const uint16_t inverse = matrix_product(syndrome, golay_inverse);
    if (__builtin_popcount(inverse) <= 3)
        return data ^ inverse;
    for (unsigned i = 0; i < 12; ++i)
        if (__builtin_popcount(inverse ^ golay_inverse[i]) <= 2)
            return data ^ inverse ^ golay_inverse[i];
    return 0xffff;
}

// Private coding helpers have only the two fixed M17 frame geometries as callers.
// Four zero tail bits return the K=5 encoder to state zero.
template <size_t N, size_t P, size_t O>
void convolution_encode(const uint8_t (&input)[N], const uint8_t (&pattern)[P],
                        uint8_t (&output)[O]) {
    static_assert((N == 30 && P == 61 && O == 46) || (N == 18 && P == 12 && O == 34),
                  "Only LSF and stream channel geometries are supported");
    memset(output, 0, O);
    uint8_t state = 0;
    size_t out_bit = 0;
    for (size_t i = 0; i < N * 8 + 4; ++i) {
        state = ((state << 1) | (i < N * 8 && bit(input, i))) & 31;
        const bool symbols[] = {bool(__builtin_parity(state & 0x19)),
                                bool(__builtin_parity(state & 0x17))};
        for (unsigned j = 0; j < 2; ++j)
            if (pattern[(2 * i + j) % P])
                set_bit(output, out_bit++, symbols[j]);
    }
}

template <size_t N, size_t P, size_t O>
uint16_t convolution_decode(const uint8_t (&input)[N], const uint8_t (&pattern)[P],
                            uint8_t (&output)[O]) {
    static_assert((N == 46 && P == 61 && O == 30) || (N == 34 && P == 12 && O == 18),
                  "Only LSF and stream channel geometries are supported");
    constexpr size_t steps = O * 8 + 4;
    uint16_t history[steps] = {};
    uint16_t metrics[2][16];
    for (unsigned i = 0; i < 16; ++i)
        metrics[0][i] = i == 0 ? 0 : 0x3fff;
    size_t in_bit = 0;
    for (size_t pos = 0; pos < steps; ++pos) {
        int symbols[2] = {-1, -1}; // -1 is a punctured bit, with no branch cost.
        for (unsigned j = 0; j < 2; ++j)
            if (pattern[(2 * pos + j) % P])
                symbols[j] = bit(input, in_bit++);
        uint16_t *previous = metrics[pos & 1];
        uint16_t *next = metrics[(pos + 1) & 1];
        for (unsigned i = 0; i < 16; ++i)
            next[i] = 0x3fff;
        for (unsigned state = 0; state < 16; ++state) {
            for (unsigned value = 0; value < 2; ++value) {
                const unsigned shift = (state << 1) | value;
                const unsigned target = shift & 15;
                uint16_t cost = previous[state];
                if (symbols[0] >= 0)
                    cost += symbols[0] != __builtin_parity(shift & 0x19);
                if (symbols[1] >= 0)
                    cost += symbols[1] != __builtin_parity(shift & 0x17);
                if (cost < next[target]) {
                    next[target] = cost;
                    if (state & 8)
                        history[pos] |= 1u << target;
                    else
                        history[pos] &= ~(1u << target);
                }
            }
        }
    }
    memset(output, 0, O);
    unsigned state = 0;
    for (size_t pos = steps; pos-- > 0;) {
        if (pos < O * 8)
            set_bit(output, pos, state & 1);
        state = (state >> 1) | (((history[pos] >> state) & 1) << 3);
    }
    return metrics[steps & 1][0];
}

void wrap(uint16_t sync, const uint8_t (&data)[46], Frame &output) {
    output = {};
    output.bytes[0] = sync >> 8;
    output.bytes[1] = sync;
    for (size_t i = 0; i < 368; ++i)
        set_bit(output.bytes + 2, i, bit(data, (45 * i + 92 * i * i) % 368));
    for (size_t i = 0; i < 46; ++i)
        output.bytes[2 + i] ^= randomizer[i];
}

void unwrap(const Frame &frame, uint8_t (&data)[46]) {
    uint8_t randomized[46];
    for (size_t i = 0; i < 46; ++i)
        randomized[i] = frame.bytes[2 + i] ^ randomizer[i];
    memset(data, 0, sizeof(data));
    for (size_t i = 0; i < 368; ++i)
        set_bit(data, (45 * i + 92 * i * i) % 368, bit(randomized, i));
}

void encode_lich(const LinkSetup &link, unsigned chunk, uint8_t *output) {
    uint8_t raw[6];
    memcpy(raw, link.bytes + 5 * chunk, 5);
    raw[5] = chunk << 5;
    for (unsigned i = 0; i < 4; ++i) {
        const size_t offset = (i * 3) / 2;
        const uint16_t value = i & 1 ? ((raw[offset] & 15) << 8) | raw[offset + 1]
                                     : (raw[offset] << 4) | (raw[offset + 1] >> 4);
        const uint32_t code = golay_encode(value);
        output[3 * i] = code >> 16;
        output[3 * i + 1] = code >> 8;
        output[3 * i + 2] = code;
    }
}

bool decode_lich(const uint8_t *input, uint8_t (&raw)[6]) {
    memset(raw, 0, sizeof(raw));
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t code =
            (uint32_t(input[3 * i]) << 16) | (uint32_t(input[3 * i + 1]) << 8) | input[3 * i + 2];
        const uint16_t value = golay_decode(code);
        if (value == 0xffff)
            return false;
        const size_t offset = (i * 3) / 2;
        if (i & 1) {
            raw[offset] |= value >> 8;
            raw[offset + 1] = value;
        } else {
            raw[offset] = value >> 4;
            raw[offset + 1] = value << 4;
        }
    }
    return (raw[5] >> 5) < 6 && (raw[5] & 31) == 0;
}

FrameKind frame_kind(const Frame &frame) {
    constexpr uint16_t syncs[] = {0x7777, 0x55f7, 0xff5d, 0x555d, 0x75ff, 0xdf55};
    constexpr FrameKind kinds[] = {FrameKind::Preamble,    FrameKind::LinkSetup,
                                   FrameKind::Stream,      FrameKind::End,
                                   FrameKind::Unsupported, FrameKind::Unsupported};
    const uint16_t sync = (frame.bytes[0] << 8) | frame.bytes[1];
    unsigned minimum = 17;
    FrameKind kind = FrameKind::Unknown;
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned distance = __builtin_popcount(sync ^ syncs[i]);
        if (distance < minimum) {
            minimum = distance;
            kind = kinds[i];
        } else if (distance == minimum) {
            kind = FrameKind::Unknown;
        }
    }
    return minimum <= 4 ? kind : FrameKind::Unknown;
}
} // namespace

bool encode_callsign(const char *text, size_t length, Address &output) {
    output = {};
    if (!text || length == 0 || length > 9 || (length == 3 && memcmp(text, "ALL", 3) == 0) ||
        (length == 7 && memcmp(text, "INVALID", 7) == 0))
        return false;
    uint64_t value = 0;
    for (size_t i = length; i-- > 0;) {
        const char *letter = strchr(alphabet + 1, text[i]);
        if (!letter || text[i] == '\0')
            return false;
        value = value * 40 + (letter - alphabet);
    }
    for (unsigned i = 0; i < 6; ++i)
        output.bytes[5 - i] = value >> (8 * i);
    return true;
}

AddressKind decode_callsign(const Address &address, char (&output)[10]) {
    memset(output, 0, sizeof(output));
    uint64_t value = 0;
    for (uint8_t byte : address.bytes)
        value = (value << 8) | byte;
    if (value == 0xffffffffffffULL) {
        memcpy(output, "ALL", 4);
        return AddressKind::Broadcast;
    }
    if (value == 0 || value >= address_limit)
        return AddressKind::Invalid;
    size_t i = 0;
    while (value) {
        output[i++] = alphabet[value % 40];
        value /= 40;
    }
    return AddressKind::Station;
}

bool make_voice_link(const char *source, size_t length, LinkSetup &output,
                     const M17Settings &settings) {
    output = {};
    Address address;
    if (!valid_m17_settings(settings) || !encode_callsign(source, length, address))
        return false;
    memcpy(output.bytes + 6, address.bytes, 6);
    if (settings.destination == Destination::Broadcast) {
        memset(output.bytes, 0xff, 6);
    } else {
        if (!encode_callsign(settings.callsign, strlen(settings.callsign), address))
            return false;
        memcpy(output.bytes, address.bytes, 6);
    }
    const uint16_t type = 5 | (static_cast<uint16_t>(settings.can) << 7);
    output.bytes[12] = type >> 8;
    output.bytes[13] = type; // Stream, Codec2 3200 voice, unencrypted.
    const uint16_t checksum = crc(output.bytes, 28);
    output.bytes[28] = checksum >> 8;
    output.bytes[29] = checksum;
    return true;
}

bool valid_link(const LinkSetup &link) {
    return crc(link.bytes, 30) == 0;
}

bool voice_stream(const LinkSetup &link) {
    const uint16_t type = (link.bytes[12] << 8) | link.bytes[13];
    // Metadata (bits 5..6) and CAN (bits 7..10) do not select voice decoding.
    return (type & 0xf81f) == 5;
}

uint8_t channel_access_number(const LinkSetup &link) {
    return ((static_cast<uint16_t>(link.bytes[12]) << 8 | link.bytes[13]) >> 7) & 15;
}

bool accept_voice_link(const LinkSetup &link, const char (&local)[10], const M17Settings &settings,
                       char (&source)[10]) {
    memset(source, 0, sizeof(source));
    if (!valid_m17_settings(settings) || (local[0] && !valid_callsign(local)) ||
        !valid_link(link) || !voice_stream(link) ||
        (settings.rx_can_check && channel_access_number(link) != settings.can)) {
        return false;
    }
    Address address;
    char origin[10];
    memcpy(address.bytes, link.bytes + 6, sizeof(address.bytes));
    if (decode_callsign(address, origin) != AddressKind::Station || !valid_callsign(origin)) {
        return false;
    }
    memcpy(address.bytes, link.bytes, sizeof(address.bytes));
    char destination[10];
    const auto kind = decode_callsign(address, destination);
    if (kind != AddressKind::Broadcast &&
        (kind != AddressKind::Station || !local[0] || strcmp(local, destination))) {
        return false;
    }
    memcpy(source, origin, sizeof(source));
    return true;
}

void preamble(Frame &output) {
    memset(output.bytes, 0x77, sizeof(output.bytes));
}

void end_marker(Frame &output) {
    for (unsigned i = 0; i < sizeof(output.bytes); i += 2) {
        output.bytes[i] = 0x55;
        output.bytes[i + 1] = 0x5d;
    }
}

void Encoder::reset() {
    link_ = {};
    number_ = 0;
    chunk_ = 0;
    ready_ = false;
}

bool Encoder::start(const LinkSetup &link, Frame &output) {
    reset();
    output = {};
    if (!valid_link(link) || !voice_stream(link))
        return false;
    link_ = link;
    uint8_t coded[46];
    convolution_encode(link.bytes, lsf_puncture, coded);
    wrap(0x55f7, coded, output);
    ready_ = true;
    return true;
}

bool Encoder::stream(const Payload &payload, bool last, Frame &output) {
    output = {};
    if (!ready_)
        return false;
    const uint16_t number = number_ | (last ? 0x8000 : 0);
    uint8_t raw[18];
    raw[0] = number >> 8;
    raw[1] = number;
    memcpy(raw + 2, payload.bytes, 16);
    uint8_t coded[34];
    convolution_encode(raw, stream_puncture, coded);
    uint8_t data[46];
    encode_lich(link_, chunk_, data);
    memcpy(data + 12, coded, sizeof(coded));
    wrap(0xff5d, data, output);
    number_ = (number_ + 1) & 0x7fff;
    chunk_ = (chunk_ + 1) % 6;
    ready_ = !last;
    return true;
}

void Decoder::reset() {
    partial_ = {};
    chunks_ = 0;
}

DecodedFrame Decoder::decode(const Frame &frame) {
    DecodedFrame result;
    result.kind = frame_kind(frame);
    if (result.kind == FrameKind::Preamble || result.kind == FrameKind::End)
        reset();
    if (result.kind != FrameKind::LinkSetup && result.kind != FrameKind::Stream)
        return result;
    uint8_t data[46];
    unwrap(frame, data);
    if (result.kind == FrameKind::LinkSetup) {
        reset();
        LinkSetup link;
        result.errors = convolution_decode(data, lsf_puncture, link.bytes);
        if (valid_link(link)) {
            result.link = link;
            result.link_updated = true;
        }
        return result;
    }
    uint8_t chunk[6];
    if (decode_lich(data, chunk)) {
        const unsigned index = chunk[5] >> 5;
        memcpy(partial_.bytes + 5 * index, chunk, 5);
        chunks_ |= 1u << index;
        if (chunks_ == 0x3f) {
            if (valid_link(partial_)) {
                result.link = partial_;
                result.link_updated = true;
            }
            reset();
        }
    }
    uint8_t coded[34];
    memcpy(coded, data + 12, sizeof(coded));
    uint8_t raw[18];
    result.errors = convolution_decode(coded, stream_puncture, raw);
    if (result.errors < 15) { // Preserve reference hard-decision acceptance limit.
        result.number = ((raw[0] & 0x7f) << 8) | raw[1];
        result.last = raw[0] & 0x80;
        memcpy(result.payload.bytes, raw + 2, 16);
        result.payload_valid = true;
    }
    return result;
}

void ReceiveStatistics::observe(const DecodedFrame &frame, int64_t now) {
    if (frame.kind == FrameKind::LinkSetup) {
        const bool was_locked = locked;
        *this = {};
        locked = was_locked;
    } else if (frame.kind == FrameKind::End || frame.kind == FrameKind::Preamble) {
        sampled = false;
        sequence_ = false;
    }
    if (frame.kind != FrameKind::Stream) {
        return;
    }
    sample_ms = now;
    sampled = true;
    errors = frame.errors;
    if (frames != UINT32_MAX) {
        ++frames;
    }
    if (!frame.payload_valid) {
        if (rejected != UINT32_MAX) {
            ++rejected;
        }
        return;
    }
    const uint16_t distance = (frame.number - previous_) & 0x7fff;
    if (!sequence_ || (distance && distance < 0x4000)) {
        if (sequence_ && distance > 1) {
            const uint32_t missing = distance - 1;
            lost = missing > UINT32_MAX - lost ? UINT32_MAX : lost + missing;
        }
        previous_ = frame.number;
        sequence_ = !frame.last;
    }
}
} // namespace m17
} // namespace ht
