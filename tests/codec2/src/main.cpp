// SPDX-License-Identifier: GPL-3.0-or-later
#include "../vectors/golden.hpp"
#include <errno.h>
#include <ht/voice.hpp>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht::m17;

namespace ht {
namespace m17 {
extern k_mutex codec_lock;
}
} // namespace ht

static K_SEM_DEFINE(codec_locked, 0, 1);
static K_SEM_DEFINE(codec_unlock, 0, 1);
static k_thread lock_thread;
K_THREAD_STACK_DEFINE(lock_stack, 1024);

static Speech speech(unsigned pair) {
    Speech result;
    for (unsigned i = 0; i < 320; ++i) {
        const unsigned sample = pair * 320 + i;
        const int triangle = sample % 80 < 40 ? sample % 40 : 40 - sample % 40;
        result.samples[i] = (triangle - 20) * 300;
    }
    return result;
}

static void before(void *) {
    voice_statistics_reset();
}

ZTEST(codec2, test_closed_calls_clear_outputs) {
    VoiceCodec codec;
    Payload encoded;
    memset(encoded.bytes, 0xff, sizeof(encoded.bytes));
    zassert_equal(codec.encode(speech(0), encoded), -ENODEV);
    const Payload zero_payload;
    zassert_mem_equal(encoded.bytes, zero_payload.bytes, sizeof(encoded.bytes));
    Speech decoded;
    memset(decoded.samples, 0xff, sizeof(decoded.samples));
    zassert_equal(codec.decode(zero_payload, decoded), -ENODEV);
    const Speech zero_speech;
    zassert_mem_equal(decoded.samples, zero_speech.samples, sizeof(decoded.samples));
    codec.close();
    codec.close();
}

ZTEST(codec2, test_reference_encode_and_input_ownership) {
    VoiceCodec codec;
    zassert_ok(codec.open());
    for (unsigned pair = 0; pair < 4; ++pair) {
        const Speech input = speech(pair);
        const Speech saved = input;
        Payload encoded;
        zassert_ok(codec.encode(input, encoded));
        zassert_mem_equal(encoded.bytes, encoded_speech[pair], sizeof(encoded.bytes));
        zassert_mem_equal(input.samples, saved.samples, sizeof(input.samples));
    }
    const auto stats = voice_statistics();
    zassert_equal(stats.encode.frames, 8);
    zassert_equal(stats.decode.frames, 0);
    zassert_true(stats.state_bytes > 0 && stats.state_bytes < 40000);
    printk("Codec2-mod fixed state: %zu bytes\n", stats.state_bytes);
    // Reopening discards predictor history and reproduces the first payload.
    zassert_ok(codec.open());
    Payload reset;
    zassert_ok(codec.encode(speech(0), reset));
    zassert_mem_equal(reset.bytes, encoded_speech[0], sizeof(reset.bytes));
}

ZTEST(codec2, test_roundtrip_framing_and_output_bounds) {
    VoiceCodec codec;
    zassert_ok(codec.open());
    Encoder encoder;
    LinkSetup link;
    Frame frame;
    Decoder decoder;
    zassert_true(make_voice_link("OE3ANC", 6, link));
    zassert_true(encoder.start(link, frame));
    zassert_true(decoder.decode(frame).link_updated);
    int64_t energy = 0;
    for (unsigned pair = 0; pair < 32; ++pair) {
        Payload encoded;
        zassert_ok(codec.encode(speech(pair), encoded));
        zassert_true(encoder.stream(encoded, pair == 31, frame));
        const auto result = decoder.decode(frame);
        zassert_true(result.payload_valid);
        zassert_equal(result.number, pair);
        zassert_equal(result.last, pair == 31);

        struct {
            uint32_t before = 0x29b50324;
            Speech speech;
            uint32_t after = 0x38204713;
        } output;

        const Payload saved = result.payload;
        zassert_ok(codec.decode(result.payload, output.speech));
        zassert_equal(output.before, 0x29b50324);
        zassert_equal(output.after, 0x38204713);
        zassert_mem_equal(result.payload.bytes, saved.bytes, sizeof(saved.bytes));
        for (int16_t sample : output.speech.samples)
            energy += int64_t(sample) * sample;
    }
    zassert_true(energy > 0); // Codec2 is lossy; equality with source PCM is not expected.
    const auto stats = voice_statistics();
    zassert_equal(stats.encode.frames, 64);
    zassert_equal(stats.decode.frames, 64);
    zassert_true(stats.encode.maximum_us <= stats.encode.total_us);
    zassert_true(stats.decode.over_budget <= stats.decode.frames);
    size_t unused = 0;
    zassert_ok(k_thread_stack_space_get(k_current_get(), &unused));
    printk("Codec2 native test stack unused: %zu bytes\n", unused);
    zassert_true(unused >= 1024, "Codec test stack is nearly exhausted");
}

ZTEST(codec2, test_single_fixed_state_and_repeated_lifecycle) {
    VoiceCodec codec;
    VoiceCodec second;
    zassert_ok(codec.open());
    zassert_equal(second.open(), -EBUSY);
    second.close(); // A rejected instance cannot release the active owner.
    Payload first;
    zassert_ok(codec.encode(speech(0), first));
    zassert_mem_equal(first.bytes, encoded_speech[0], sizeof(first.bytes));
    codec.close();
    zassert_ok(second.open());
    second.close();
    for (unsigned i = 0; i < 16; ++i) {
        zassert_ok(codec.open());
        zassert_ok(codec.encode(speech(0), first));
        zassert_mem_equal(first.bytes, encoded_speech[0], sizeof(first.bytes));
        codec.close();
    }
    zassert_equal(voice_statistics().initializations, 18);
}

static void hold_codec_lock(void *, void *, void *) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    k_sem_give(&codec_locked);
    k_sem_take(&codec_unlock, K_FOREVER);
    k_mutex_unlock(&codec_lock);
}

ZTEST(codec2, test_statistics_reset_and_snapshot_do_not_wait_for_codec) {
    VoiceCodec codec;
    zassert_ok(codec.open());
    Payload payload;
    zassert_ok(codec.encode(speech(0), payload));
    const auto saved = voice_statistics();
    zassert_equal(saved.encode.frames, 2);
    k_thread_create(&lock_thread, lock_stack, K_THREAD_STACK_SIZEOF(lock_stack), hold_codec_lock,
                    nullptr, nullptr, nullptr, 8, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&codec_locked, K_MSEC(100)));
    zassert_equal(voice_statistics().encode.frames, saved.encode.frames);
    voice_statistics_reset();
    const auto cleared = voice_statistics();
    zassert_equal(cleared.encode.frames, 0);
    zassert_equal(cleared.encode.total_us, 0);
    zassert_equal(cleared.decode.maximum_us, 0);
    zassert_equal(cleared.state_bytes, saved.state_bytes);
    k_sem_give(&codec_unlock);
    zassert_ok(k_thread_join(&lock_thread, K_MSEC(100)));
    zassert_ok(codec.encode(speech(1), payload));
    zassert_equal(voice_statistics().encode.frames, 2);
}

ZTEST_SUITE(codec2, nullptr, nullptr, before, nullptr, nullptr);
