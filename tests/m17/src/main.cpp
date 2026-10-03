// SPDX-License-Identifier: GPL-3.0-or-later
#include "../vectors/golden.hpp"
#include <ht/m17.hpp>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht::m17;
using ht::M17Settings;
using ht::Destination;

static Frame frame(const uint8_t (&bytes)[48]) {
    Frame result;
    memcpy(result.bytes, bytes, sizeof(bytes));
    return result;
}

static Payload payload() {
    Payload result;
    for (unsigned i = 0; i < 16; ++i)
        result.bytes[i] = i;
    return result;
}

static LinkSetup link() {
    LinkSetup result;
    zassert_true(make_voice_link("OE3ANC", 6, result));
    return result;
}

static void flip_coded_bit(Frame &value, size_t position) {
    // The M17 permutation is its own inverse; flip a chosen unwrapped bit.
    const size_t wire = (45 * position + 92 * position * position) % 368;
    value.bytes[2 + wire / 8] ^= 0x80 >> (wire % 8);
}

ZTEST(m17, test_callsign_bounds_and_reference_vector) {
    Address encoded;
    zassert_true(encode_callsign("AB1CD", 5, encoded));
    const uint8_t expected[] = {0, 0, 0, 0x9f, 0xdd, 0x51};
    zassert_mem_equal(encoded.bytes, expected, sizeof(expected));
    char decoded[10];
    zassert_equal(decode_callsign(encoded, decoded), AddressKind::Station);
    zassert_equal(strcmp(decoded, "AB1CD"), 0);
    const char *invalid[] = {"", "aB1CD", "AB CD", "AB@CD", "ALL", "INVALID", "1234567890"};
    const Address zero;
    for (const char *text : invalid) {
        memset(encoded.bytes, 0xff, sizeof(encoded.bytes));
        zassert_false(encode_callsign(text, strlen(text), encoded));
        zassert_mem_equal(encoded.bytes, zero.bytes, sizeof(encoded.bytes));
    }
    zassert_false(encode_callsign(nullptr, 3, encoded));
    const char unterminated[] = {'O', 'E', '3', 'A', 'N', 'C'};
    zassert_true(encode_callsign(unterminated, sizeof(unterminated), encoded));
    zassert_true(encode_callsign("ZZ9-/....", 9, encoded));
    zassert_equal(decode_callsign(encoded, decoded), AddressKind::Station);
    zassert_equal(strcmp(decoded, "ZZ9-/...."), 0);
    const char embedded_nul[] = {'A', '\0', 'B'};
    zassert_false(encode_callsign(embedded_nul, sizeof(embedded_nul), encoded));
}

ZTEST(m17, test_reserved_addresses_are_not_truncated) {
    char decoded[10];
    Address address;
    zassert_equal(decode_callsign(address, decoded), AddressKind::Invalid);
    zassert_equal(decoded[0], 0);
    memset(address.bytes, 0xff, 6);
    zassert_equal(decode_callsign(address, decoded), AddressKind::Broadcast);
    zassert_equal(strcmp(decoded, "ALL"), 0);
    // The first reserved value is exactly 40^9; it cannot fit nine base-40 digits.
    const uint64_t limit = 262144000000000ULL;
    for (unsigned i = 0; i < 6; ++i)
        address.bytes[5 - i] = limit >> (8 * i);
    zassert_equal(decode_callsign(address, decoded), AddressKind::Invalid);
    zassert_equal(decoded[0], 0);
}

ZTEST(m17, test_lsf_and_all_lich_chunks_match_official_library) {
    const LinkSetup setup = link();
    zassert_mem_equal(setup.bytes, voice_link, sizeof(voice_link));
    zassert_true(valid_link(setup));
    zassert_true(voice_stream(setup));
    Encoder encoder;
    Frame actual;
    zassert_true(encoder.start(setup, actual));
    zassert_mem_equal(actual.bytes, lsf_frame, sizeof(lsf_frame));
    const uint8_t *expected[] = {stream_0, stream_1, stream_2, stream_3, stream_4, stream_5};
    for (unsigned i = 0; i < 6; ++i) {
        zassert_true(encoder.stream(payload(), i == 5, actual));
        zassert_mem_equal(actual.bytes, expected[i], sizeof(actual.bytes), "Chunk %u", i);
    }
    zassert_false(encoder.stream(payload(), false, actual));
    const Frame zero;
    zassert_mem_equal(actual.bytes, zero.bytes, sizeof(actual.bytes));
}

ZTEST(m17, test_decode_official_frames_and_late_entry) {
    Decoder decoder;
    auto result = decoder.decode(frame(lsf_frame));
    zassert_equal(result.kind, FrameKind::LinkSetup);
    zassert_true(result.link_updated);
    zassert_equal(result.errors, 0);
    zassert_mem_equal(result.link.bytes, voice_link, sizeof(voice_link));
    decoder.reset(); // Late entry starts without an LSF.
    const uint8_t *expected[] = {stream_0, stream_1, stream_2, stream_3, stream_4, stream_5};
    for (unsigned i = 0; i < 6; ++i) {
        Frame input;
        memcpy(input.bytes, expected[i], sizeof(input.bytes));
        result = decoder.decode(input);
        zassert_equal(result.kind, FrameKind::Stream);
        zassert_true(result.payload_valid);
        zassert_equal(result.errors, 0);
        zassert_equal(result.number, i);
        zassert_equal(result.last, i == 5);
        const Payload source = payload();
        zassert_mem_equal(result.payload.bytes, source.bytes, sizeof(source.bytes));
        zassert_equal(result.link_updated, i == 5);
    }
    zassert_mem_equal(result.link.bytes, voice_link, sizeof(voice_link));
}

ZTEST(m17, test_crc_rejection_and_no_stale_payload) {
    Decoder decoder;
    zassert_true(decoder.decode(frame(lsf_frame)).link_updated);
    auto result = decoder.decode(frame(bad_crc_lsf));
    zassert_equal(result.kind, FrameKind::LinkSetup);
    zassert_false(result.link_updated);
    const LinkSetup zero_link;
    zassert_mem_equal(result.link.bytes, zero_link.bytes, sizeof(zero_link.bytes));
    zassert_true(decoder.decode(frame(stream_0)).payload_valid);
    Frame damaged = frame(stream_1);
    // Invert only the convolutional payload, keeping sync and LICH untouched.
    for (size_t i = 96; i < 368; ++i)
        flip_coded_bit(damaged, i);
    result = decoder.decode(damaged);
    zassert_false(result.payload_valid);
    zassert_true(result.errors >= 15);
    const Payload zero;
    zassert_mem_equal(result.payload.bytes, zero.bytes, sizeof(zero.bytes));
    zassert_equal(result.number, 0);
    zassert_false(result.last);
    LinkSetup invalid = link();
    invalid.bytes[6] ^= 1;
    Encoder encoder;
    Frame output;
    zassert_false(encoder.start(invalid, output));
    zassert_false(encoder.stream(payload(), false, output));
}

ZTEST(m17, test_error_correction_and_repeated_lich_chunks) {
    Decoder decoder;
    const uint8_t *expected[] = {stream_0, stream_1, stream_2, stream_3, stream_4, stream_5};
    for (unsigned chunk = 0; chunk < 6; ++chunk) {
        Frame input;
        memcpy(input.bytes, expected[chunk], sizeof(input.bytes));
        // Three errors per Golay word, spread over data and parity bits.
        const unsigned offsets[] = {0, 7, 19};
        for (unsigned word = 0; word < 4; ++word)
            for (unsigned offset : offsets)
                flip_coded_bit(input, word * 24 + offset);
        flip_coded_bit(input, 150);
        auto result = decoder.decode(input);
        zassert_true(result.payload_valid);
        zassert_equal(result.errors, 1);
        const Payload source = payload();
        zassert_mem_equal(result.payload.bytes, source.bytes, sizeof(source.bytes));
        zassert_equal(result.link_updated, chunk == 5);
        if (chunk != 5) {
            result = decoder.decode(input); // A duplicate must not advance the segment map.
            zassert_false(result.link_updated);
        } else {
            zassert_mem_equal(result.link.bytes, voice_link, sizeof(voice_link));
        }
    }
    Frame damaged = frame(lsf_frame);
    flip_coded_bit(damaged, 210);
    auto result = decoder.decode(damaged);
    zassert_true(result.link_updated);
    zassert_equal(result.errors, 1);
}

ZTEST(m17, test_session_reset_and_unsupported_sync) {
    Decoder decoder;
    decoder.decode(frame(stream_0));
    decoder.decode(frame(stream_1));
    Frame input;
    preamble(input);
    zassert_equal(decoder.decode(input).kind, FrameKind::Preamble);
    const uint8_t *rest[] = {stream_2, stream_3, stream_4, stream_5};
    for (const uint8_t *bytes : rest) {
        memcpy(input.bytes, bytes, sizeof(input.bytes));
        zassert_false(decoder.decode(input).link_updated);
    }
    end_marker(input);
    zassert_equal(decoder.decode(input).kind, FrameKind::End);
    input = {};
    zassert_equal(decoder.decode(input).kind, FrameKind::Unknown);
    input.bytes[0] = 0x55;
    input.bytes[1] = 0x75; // Equidistant from LSF and EOT.
    zassert_equal(decoder.decode(input).kind, FrameKind::Unknown);
    input.bytes[0] = 0x75;
    input.bytes[1] = 0xff;
    zassert_equal(decoder.decode(input).kind, FrameKind::Unsupported);
    input.bytes[0] = 0xdf;
    input.bytes[1] = 0x55;
    zassert_equal(decoder.decode(input).kind, FrameKind::Unsupported);
}

ZTEST(m17, test_voice_type_ignores_can_but_rejects_other_codecs_and_encryption) {
    LinkSetup setup = link();
    for (unsigned can = 0; can < 16; ++can) {
        const uint16_t type = 5 | (can << 7) | (2 << 5);
        setup.bytes[12] = type >> 8;
        setup.bytes[13] = type;
        zassert_true(voice_stream(setup));
    }
    const uint16_t unsupported[] = {0, 1, 3, 7, 5 | 8, 5 | 16, 5 | 0x800, 5 | 0x1000};
    for (uint16_t type : unsupported) {
        setup.bytes[12] = type >> 8;
        setup.bytes[13] = type;
        zassert_false(voice_stream(setup));
    }
}

ZTEST(m17, test_counter_rollover_and_start_reset) {
    Encoder encoder;
    Frame output;
    zassert_false(encoder.stream(payload(), false, output));
    zassert_true(encoder.start(link(), output));
    for (unsigned i = 0; i <= 0x8000; ++i)
        zassert_true(encoder.stream(payload(), false, output));
    Decoder decoder;
    auto result = decoder.decode(output);
    zassert_true(result.payload_valid);
    zassert_equal(result.number, 0);
    zassert_false(result.last);
    zassert_true(encoder.start(link(), output));
    zassert_true(encoder.stream(payload(), false, output));
    zassert_mem_equal(output.bytes, stream_0, sizeof(stream_0));
    encoder.reset();
    zassert_false(encoder.stream(payload(), false, output));
}

ZTEST(m17, test_independent_destination_can_matrix_and_receive_policy) {
    char local[10] = "OE1TEST";
    char source[10];
    for (unsigned can = 0; can < 16; ++can) {
        M17Settings settings;
        settings.can = can;
        settings.rx_can_check = true;
        LinkSetup link;
        zassert_true(make_voice_link("OE3ANC", 6, link, settings));
        zassert_mem_equal(link.bytes, broadcast_can_links + 30 * can, 30);
        zassert_equal(channel_access_number(link), can);
        zassert_true(accept_voice_link(link, local, settings, source));
        zassert_equal(strcmp(source, "OE3ANC"), 0);
        settings.destination = Destination::Station;
        strcpy(settings.callsign, "OE1TEST");
        zassert_true(make_voice_link("OE3ANC", 6, link, settings));
        zassert_mem_equal(link.bytes, directed_can_links + 30 * can, 30);
        zassert_true(accept_voice_link(link, local, settings, source));
        // Directed transmit destination never substitutes for local RX address.
        char other[10] = "OTHER";
        zassert_false(accept_voice_link(link, other, settings, source));
        zassert_equal(source[0], 0);
        char unset[10] = {};
        zassert_false(accept_voice_link(link, unset, settings, source));
        settings.can = (can + 1) % 16;
        zassert_false(accept_voice_link(link, local, settings, source));
        zassert_equal(source[0], 0);
        settings.rx_can_check = false;
        zassert_true(accept_voice_link(link, local, settings, source));
        LinkSetup broadcast;
        memcpy(broadcast.bytes, broadcast_can_links + 30 * can, 30);
        settings.can = can;
        settings.rx_can_check = true;
        zassert_true(accept_voice_link(broadcast, unset, settings, source));
        link.bytes[6] ^= 1; // Invalid CRC must always clear accepted source.
        zassert_false(accept_voice_link(link, local, settings, source));
        zassert_equal(source[0], 0);
    }
    M17Settings invalid;
    invalid.can = 16;
    LinkSetup bad;
    memset(bad.bytes, 0xa5, sizeof(bad.bytes));
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
    const LinkSetup zero;
    zassert_mem_equal(bad.bytes, zero.bytes, 30);
    invalid = {};
    invalid.destination = Destination::Station;
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
    strcpy(invalid.callsign, "lower");
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
    strcpy(invalid.callsign, "ALL");
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
    memset(invalid.callsign, 'A', sizeof(invalid.callsign));
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
    invalid = {};
    invalid.destination = static_cast<Destination>(99);
    zassert_false(make_voice_link("OE3ANC", 6, bad, invalid));
}

ZTEST(m17, test_independent_directed_can15_coded_lsf_and_late_entry) {
    M17Settings settings;
    settings.destination = Destination::Station;
    strcpy(settings.callsign, "OE1TEST");
    settings.can = 15;
    LinkSetup link;
    zassert_true(make_voice_link("OE3ANC", 6, link, settings));
    Encoder encoder;
    Frame output;
    zassert_true(encoder.start(link, output));
    zassert_mem_equal(output.bytes, directed_can15_lsf, sizeof(output.bytes));
    Decoder decoder;
    auto decoded = decoder.decode(frame(directed_can15_lsf));
    zassert_true(decoded.link_updated);
    zassert_mem_equal(decoded.link.bytes, directed_can_links + 15 * 30, 30);
    decoder.reset();
    const uint8_t *streams[] = {directed_can15_stream_0, directed_can15_stream_1,
                                directed_can15_stream_2, directed_can15_stream_3,
                                directed_can15_stream_4, directed_can15_stream_5};
    for (unsigned i = 0; i < 6; ++i) {
        zassert_true(encoder.stream(payload(), i == 5, output));
        zassert_mem_equal(output.bytes, streams[i], sizeof(output.bytes));
        Frame incoming;
        memcpy(incoming.bytes, streams[i], sizeof(incoming.bytes));
        decoded = decoder.decode(incoming);
        zassert_equal(decoded.link_updated, i == 5);
    }
    char local[10] = "OE1TEST", source[10];
    settings.rx_can_check = true;
    zassert_true(accept_voice_link(decoded.link, local, settings, source));
    settings.can = 0;
    zassert_false(accept_voice_link(decoded.link, local, settings, source));
}

ZTEST(m17, test_receive_quality_distance_loss_wrap_and_freshness) {
    Decoder decoder;
    Frame incoming = frame(stream_0);
    flip_coded_bit(incoming, 96 + 10); // Stream FEC, outside the 96-bit LICH.
    const auto decoded = decoder.decode(incoming);
    zassert_true(decoded.payload_valid);
    zassert_equal(decoded.errors, 1);
    ReceiveStatistics quality;
    quality.locked = true;
    quality.observe(decoded, 100);
    zassert_equal(quality.ber_permyriad(), 36); // 1/272 = 0.36%, rounded down.
    zassert_true(quality.fresh(599));
    zassert_false(quality.fresh(600));
    zassert_false(quality.fresh(99));
    DecodedFrame next = decoded;
    next.number = 3;
    quality.observe(next, 140);
    zassert_equal(quality.lost, 2);
    quality.observe(next, 180); // Duplicate cannot add inferred loss.
    zassert_equal(quality.lost, 2);
    next.payload_valid = false;
    next.errors = 15;
    quality.observe(next, 220);
    zassert_equal(quality.rejected, 1);
    next.payload_valid = true;
    next.number = 5;
    quality.observe(next, 260);
    zassert_equal(quality.lost, 3); // Rejected/undetected frames are part of the gap.
    next.kind = FrameKind::LinkSetup;
    quality.observe(next, 300);
    zassert_equal(quality.frames, 0);
    zassert_false(quality.sampled);
    next.kind = FrameKind::Stream;
    next.number = 32767;
    quality.observe(next, 340);
    next.number = 0;
    quality.observe(next, 380);
    zassert_equal(quality.lost, 0);
    next.number = 32766; // Old/out-of-order frame does not move the loss baseline.
    quality.observe(next, 400);
    next.number = 1;
    quality.observe(next, 420);
    zassert_equal(quality.lost, 0);
    next.kind = FrameKind::End;
    quality.observe(next, 460);
    zassert_false(quality.fresh(461));
}

ZTEST_SUITE(m17, nullptr, nullptr, nullptr, nullptr, nullptr);
