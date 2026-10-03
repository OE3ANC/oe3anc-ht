// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../modules/protocols/codec2/allocator.h"
#include "../../m17/vectors/golden.hpp"
#include "../../m17_demodulation/vectors/golden.hpp"
#include <errno.h>
#include <ht/audio_backend.h>
#include <ht/m17_mode.hpp>
#include <ht/m17_modem.hpp>
#include <ht/voice.hpp>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

using namespace ht;

namespace ht {
namespace m17 {
extern k_mutex codec_lock;
}
} // namespace ht

static K_MUTEX_DEFINE(io_mutex);
static bool capture, playback, feed_rx, drop_capture, drop_playback;
static size_t input_position, radio_count, speaker_nonzero;
static const int16_t *rx_samples = reference_adc;
static size_t rx_count = 4800;
static int16_t generated_rx[20000];
static int16_t stereo_input[960], stereo_output[960], radio_samples[48000];
static int64_t next_poll;
static uint32_t tail_calls;
static const char *scenario;

extern "C" int ht_audio_backend_open() {
    return 0;
}

extern "C" int ht_audio_backend_set_running(bool input, bool output) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    capture = input;
    playback = output;
    next_poll = k_uptime_get() + 10;
    k_mutex_unlock(&io_mutex);
    return 0;
}

extern "C" int ht_audio_backend_poll(uint32_t session) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    if (k_uptime_get() < next_poll) {
        k_mutex_unlock(&io_mutex);
        return 0;
    }
    next_poll += 10;
    int error = 0;
    if (capture && !drop_capture) {
        memset(stereo_input, 0, sizeof(stereo_input));
        for (unsigned i = 0; i < 480; ++i) {
            stereo_input[2 * i] = 8000; // Nonzero microphone DC exercises the reference blocker.
            if (feed_rx && input_position / 2 < rx_count)
                stereo_input[2 * i + 1] = rx_samples[input_position / 2];
            ++input_position;
        }
        error = ht_audio_capture(session, stereo_input, 480, 2, 0, 1);
    }
    if (!error && playback && !drop_playback) {
        error = ht_audio_playback(session, stereo_output, 480);
        if (!error)
            for (unsigned i = 0; i < 480; ++i) {
                speaker_nonzero += stereo_output[2 * i] != 0;
                if (radio_count < 48000)
                    radio_samples[radio_count++] = stereo_output[2 * i + 1];
            }
    }
    k_mutex_unlock(&io_mutex);
    return error;
}

extern "C" int ht_audio_backend_tail_ms(uint32_t *delay) {
    k_mutex_lock(&io_mutex, K_FOREVER);
    ++tail_calls;
    k_mutex_unlock(&io_mutex);
    *delay = 30;
    return 0;
}

static void *setup() {
    scenario = getenv("HT_M17_MODE_FAULT");
    if (!scenario)
        scenario = "capture";
    zassert_ok(audio_start());
    return nullptr;
}

static void before(void *) {
    m17_cancel();
    audio_cancel();
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = drop_capture = drop_playback = false;
    rx_samples = reference_adc;
    rx_count = 4800;
    input_position = radio_count = speaker_nonzero = tail_calls = 0;
    k_mutex_unlock(&io_mutex);
}

static void after(void *) {
    m17_cancel();
    audio_cancel();
    k_sleep(K_MSEC(150));
}

static AudioSession route(bool transmit) {
    AudioRoutes routes;
    routes.input_rate[transmit ? 0 : 1] = transmit ? 8000 : 24000;
    routes.output[transmit ? 1 : 0] = AudioSource::Buffer;
    routes.output_rate[transmit ? 1 : 0] = transmit ? 48000 : 8000;
    AudioSession session;
    zassert_ok(audio_route(routes, session));
    return session;
}

static M17Status wait_phase(M17Phase phase, uint32_t timeout = 1000) {
    const int64_t deadline = k_uptime_get() + timeout;
    M17Status state;
    do {
        state = m17_status();
        if (state.phase == phase)
            return state;
        zassert_ok(state.error, "Unexpected M17 fault: %d", state.error);
        k_sleep(K_MSEC(5));
    } while (k_uptime_get() < deadline);
    zassert_equal(state.phase, phase);
    return state;
}

ZTEST(m17_mode, test_a_prepare_does_not_transmit_and_invalid_calls_are_rejected) {
    const auto session = route(true);
    zassert_equal(m17_prepare_transmit({}, "OE3ANC"), -EINVAL);
    zassert_equal(m17_prepare_transmit(session, ""), -EINVAL);
    zassert_equal(m17_prepare_transmit(session, "lowercase"), -EINVAL);
    zassert_equal(m17_prepare_transmit(session, "1234567890"), -EINVAL);
    zassert_equal(m17_begin_transmit(), -EINVAL);
    zassert_ok(m17_prepare_transmit(session, "OE3ANC"));
    zassert_equal(m17_status().phase, M17Phase::Prepared);
    k_sleep(K_MSEC(150));
    k_mutex_lock(&io_mutex, K_FOREVER);
    for (size_t i = 0; i < radio_count; ++i)
        zassert_equal(radio_samples[i], 0);
    k_mutex_unlock(&io_mutex);
    zassert_ok(m17_status().error);
}

ZTEST(m17_mode, test_b_tx_broadcast_voice_final_frame_eot_and_drain) {
    m17::VoiceCodec reference;
    m17::Speech speech;
    m17::Payload expected;
    // Pinned reference DC arithmetic, bounded positive input so its original
    // int32 accumulator/shift have no overflow or negative-shift ambiguity.
    int32_t accumulator = 0, previous_input = 0, previous_output = 0;
    for (auto &sample : speech.samples) {
        accumulator -= previous_input;
        previous_input = 8000 << 15;
        accumulator += previous_input;
        accumulator -= 164 * previous_output;
        previous_output = accumulator >> 15;
        sample = previous_output;
    }
    zassert_ok(reference.open());
    zassert_ok(reference.encode(speech, expected));
    reference.close();
    const auto session = route(true);
    zassert_ok(m17_prepare_transmit(session, "OE3ANC"));
    zassert_ok(m17_begin_transmit());
    // Allow preamble, LSF and at least one complete 40 ms voice frame.
    k_sleep(K_MSEC(230));
    zassert_ok(m17_finish_transmit());
    wait_phase(M17Phase::Finished);
    zassert_ok(m17_finish_transmit());
    zassert_ok(audio_status().error);
    m17::Demodulator demodulator;
    m17::Decoder decoder;
    m17::Frame frame;
    unsigned links = 0, voices = 0, lasts = 0, endings = 0;
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_equal(tail_calls, 1);
    for (size_t i = 0; i < radio_count; i += 2) {
        // Undo DSP RF polarity and reduce amplitude to the RX fixture level.
        if (!demodulator.sample(-radio_samples[i] / 2, frame))
            continue;
        const auto decoded = decoder.decode(frame);
        if (decoded.link_updated) {
            m17::LinkSetup link;
            zassert_true(m17::make_voice_link("OE3ANC", 6, link));
            zassert_mem_equal(decoded.link.bytes, link.bytes, 30);
            ++links;
        }
        if (decoded.payload_valid) {
            if (!voices)
                zassert_mem_equal(decoded.payload.bytes, expected.bytes, 16);
            ++voices;
            lasts += decoded.last;
        }
        endings += decoded.kind == m17::FrameKind::End;
    }
    k_mutex_unlock(&io_mutex);
    zassert_true(links >= 1);
    zassert_true(voices >= 2);
    zassert_equal(lasts, 1);
    zassert_equal(endings, 1);
}

ZTEST(m17_mode, test_c_independent_rx_fixture_callsign_speaker_and_end) {
    const auto session = route(false);
    zassert_ok(m17_receive(session, ""));
    k_mutex_lock(&io_mutex, K_FOREVER);
    feed_rx = true;
    input_position = 0;
    k_mutex_unlock(&io_mutex);
    bool active = false;
    for (unsigned i = 0; i < 60; ++i) {
        const auto state = m17_status();
        zassert_ok(state.error);
        if (state.rx_active) {
            zassert_equal(strcmp(state.callsign, "OE3ANC"), 0);
            active = true;
        }
        k_sleep(K_MSEC(5));
    }
    zassert_true(active);
    zassert_false(m17_status().rx_active);
    zassert_equal(m17_status().callsign[0], '\0');
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_true(speaker_nonzero > 0);
    k_mutex_unlock(&io_mutex);
}

ZTEST(m17_mode, test_d_cancel_blocked_processing_and_replace_sessions) {
    for (unsigned i = 0; i < 6; ++i) {
        auto session = route(false);
        zassert_ok(m17_receive(session, "OE3ANC"));
        k_mutex_lock(&io_mutex, K_FOREVER);
        drop_capture = true;
        k_mutex_unlock(&io_mutex);
        k_sleep(K_MSEC(20));
        m17_cancel();
        audio_cancel();
        zassert_equal(m17_status().phase, M17Phase::Idle);
        k_mutex_lock(&io_mutex, K_FOREVER);
        drop_capture = false;
        k_mutex_unlock(&io_mutex);
        session = route(true);
        zassert_ok(m17_prepare_transmit(session, "OE3ANC"));
        zassert_ok(m17_begin_transmit());
        k_sleep(K_MSEC(20));
        m17_cancel();
        audio_cancel();
        zassert_equal(m17_status().phase, M17Phase::Idle);
        zassert_ok(m17_status().error);
    }
}

static void update_crc(m17::LinkSetup &link) {
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < 28; ++i) {
        crc ^= uint16_t(link.bytes[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x5935 : 0);
    }
    link.bytes[28] = crc >> 8;
    link.bytes[29] = crc;
}

static void generated_receive(const char *destination, bool late, bool invalid, bool audible,
                              bool restart = false, const M17Settings &settings = {},
                              uint8_t received_can = 7) {
    m17::LinkSetup link;
    zassert_true(m17::make_voice_link("OE3ANC", 6, link));
    if (*destination) {
        m17::Address address;
        zassert_true(m17::encode_callsign(destination, strlen(destination), address));
        memcpy(link.bytes, address.bytes, 6);
    }
    const uint16_t type = 5 | (static_cast<uint16_t>(received_can) << 7);
    link.bytes[12] = type >> 8;
    link.bytes[13] = type;
    update_crc(link);
    zassert_true(m17::valid_link(link));
    m17::Encoder encoder;
    m17::Modulator modulator;
    m17::TxSamples waveform;
    m17::Frame frame;
    size_t count = 0;
    const auto append = [&] {
        modulator.render(frame, waveform);
        zassert_true(count + 960 <= 20000);
        for (unsigned i = 0; i < 1920; i += 2)
            generated_rx[count++] = waveform.samples[i] / 4;
    };
    m17::preamble(frame);
    append();
    append();
    zassert_true(encoder.start(link, frame));
    if (!late)
        append();
    if (invalid) {
        memcpy(frame.bytes, bad_crc_lsf, 48);
        append();
    }
    m17::Payload payload;
    for (unsigned i = 0; i < 16; ++i)
        payload.bytes[i] = i;
    for (unsigned i = 0; i < (late ? 8u : 1u); ++i) {
        zassert_true(encoder.stream(payload, restart, frame));
        append();
    }
    if (restart) {
        // The next identical link omits LSF and starts through LICH, without
        // EOT or lock loss between streams. Final voice must reset predictors.
        zassert_true(encoder.start(link, frame));
        for (unsigned i = 0; i < 8; ++i) {
            zassert_true(encoder.stream(payload, i == 7, frame));
            append();
        }
        k_mutex_lock(&m17::codec_lock, K_FOREVER);
        ht_codec2_test_fail_after(-1);
        k_mutex_unlock(&m17::codec_lock);
    }
    m17::end_marker(frame);
    append();
    const auto session = route(false);
    M17Settings copied = settings;
    zassert_ok(m17_receive(session, "OE3ANC", copied));
    // Changing the producer draft cannot change an already prepared RX job.
    copied.can = (copied.can + 1) % 16;
    copied.rx_can_check = !copied.rx_can_check;
    k_mutex_lock(&m17::codec_lock, K_FOREVER);
    const unsigned constructor_allocations = ht_codec2_test_attempts();
    k_mutex_unlock(&m17::codec_lock);
    k_mutex_lock(&io_mutex, K_FOREVER);
    rx_samples = generated_rx;
    rx_count = count;
    input_position = 0;
    speaker_nonzero = 0;
    feed_rx = true;
    k_mutex_unlock(&io_mutex);
    const int64_t deadline = k_uptime_get() + 1500;
    bool complete = false;
    bool accepted_call = false;
    while (k_uptime_get() < deadline) {
        const auto state = m17_status();
        zassert_ok(state.error);
        if (state.rx_active) {
            zassert_equal(strcmp(state.callsign, "OE3ANC"), 0);
            accepted_call = true;
        }
        k_mutex_lock(&io_mutex, K_FOREVER);
        complete = input_position >= 2 * count + 1920 && !state.rx_active;
        k_mutex_unlock(&io_mutex);
        if (complete)
            break;
        k_sleep(K_MSEC(5));
    }
    zassert_true(complete, "RX case dst=%s late=%u invalid=%u", destination, late, invalid);
    zassert_false(m17_status().rx_active);
    // This invalid-LSF case starts with a valid LSF, so earlier activity is
    // allowed; voice after its invalid replacement must still remain silent.
    if (!invalid) {
        zassert_equal(accepted_call, audible);
    }
    if (!audible) {
        k_mutex_lock(&m17::codec_lock, K_FOREVER);
        zassert_equal(ht_codec2_test_attempts(), constructor_allocations);
        k_mutex_unlock(&m17::codec_lock);
    }
    if (restart) {
        k_mutex_lock(&m17::codec_lock, K_FOREVER);
        // Initial preparation + one fresh predictor for each received stream.
        zassert_equal(ht_codec2_test_attempts(), 3 * constructor_allocations);
        k_mutex_unlock(&m17::codec_lock);
    }
    k_mutex_lock(&io_mutex, K_FOREVER);
    zassert_equal(speaker_nonzero > 0, audible);
    k_mutex_unlock(&io_mutex);
    m17_cancel();
    audio_cancel();
    k_sleep(K_MSEC(20));
}

ZTEST(m17_mode, test_e_rx_address_can_late_entry_and_invalid_lsf_gating) {
    generated_receive("", false, false, true);
    generated_receive("OE3ANC", false, false, true);
    generated_receive("OTHER", false, false, false);
    generated_receive("", true, false, true);
    generated_receive("", false, true, false);
}

ZTEST(m17_mode, test_f_audio_cancel_before_processing_cancel_is_a_normal_stop) {
    const auto session = route(false);
    zassert_ok(m17_receive(session, ""));
    audio_cancel();
    k_sleep(K_MSEC(20));
    zassert_equal(m17_status().phase, M17Phase::Idle);
    zassert_ok(m17_status().error);
    m17_cancel();
}

ZTEST(m17_mode, test_g_final_voice_resets_same_link_predictor_without_eot) {
    generated_receive("", false, false, true, true);
}

ZTEST(m17_mode, test_g_directed_can15_tx_uses_copied_settings) {
    const auto session = route(true);
    M17Settings settings;
    settings.destination = Destination::Station;
    strcpy(settings.callsign, "OE1TEST");
    settings.can = 15;
    zassert_ok(m17_prepare_transmit(session, "OE3ANC", settings));
    settings = {}; // Borrowed data must not reach the processing thread.
    zassert_ok(m17_begin_transmit());
    k_sleep(K_MSEC(230));
    zassert_ok(m17_finish_transmit());
    wait_phase(M17Phase::Finished);
    m17::Demodulator demodulator;
    m17::Decoder decoder;
    m17::Frame frame;
    unsigned links = 0;
    k_mutex_lock(&io_mutex, K_FOREVER);
    for (size_t i = 0; i < radio_count; i += 2) {
        if (!demodulator.sample(-radio_samples[i] / 2, frame)) {
            continue;
        }
        const auto decoded = decoder.decode(frame);
        if (decoded.link_updated) {
            zassert_mem_equal(decoded.link.bytes, directed_can_links + 15 * 30, 30);
            ++links;
        }
    }
    k_mutex_unlock(&io_mutex);
    zassert_true(links > 0);
}

ZTEST(m17_mode, test_h_rx_can_filter_precedes_voice_decode_and_activity) {
    M17Settings settings;
    settings.can = 7;
    settings.rx_can_check = true;
    generated_receive("", false, false, true, false, settings, 7);
    generated_receive("OE3ANC", true, false, true, false, settings, 7);
    generated_receive("", false, false, false, false, settings, 0);
    generated_receive("OE3ANC", true, false, false, false, settings, 15);
    generated_receive("OTHER", false, false, false, false, settings, 7);
    settings.can = 15;
    generated_receive("", false, false, true, false, settings, 15);
    generated_receive("", false, false, false, false, settings, 7);
    settings.can = 0;
    generated_receive("", true, false, true, false, settings, 0);
    settings.rx_can_check = false;
    // TX destination is independent of the local RX address/filter.
    settings.destination = Destination::Station;
    strcpy(settings.callsign, "OTHER");
    generated_receive("OE3ANC", false, false, true, false, settings, 15);
}

ZTEST(m17_mode, test_z_processing_error_is_latched_after_cancel_and_restart) {
    const bool output = !strcmp(scenario, "playback");
    const auto session = route(output);
    if (output) {
        zassert_ok(m17_prepare_transmit(session, "OE3ANC"));
        k_mutex_lock(&io_mutex, K_FOREVER);
        drop_playback = true;
        k_mutex_unlock(&io_mutex);
        zassert_ok(m17_begin_transmit());
    } else {
        zassert_ok(m17_receive(session, ""));
        k_mutex_lock(&io_mutex, K_FOREVER);
        drop_capture = true;
        k_mutex_unlock(&io_mutex);
    }
    const auto state = wait_phase(M17Phase::Fault);
    zassert_equal(state.error, -ETIMEDOUT);
    m17_cancel();
    audio_cancel();
    zassert_equal(m17_status().phase, M17Phase::Fault);
    zassert_equal(m17_receive(session, ""), state.error);
    zassert_equal(m17_prepare_transmit(session, "OE3ANC"), state.error);
    zassert_equal(m17_begin_transmit(), state.error);
    zassert_equal(m17_finish_transmit(), state.error);
}

ZTEST_SUITE(m17_mode, nullptr, setup, before, after, nullptr);
