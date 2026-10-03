// SPDX-License-Identifier: GPL-3.0-or-later
#include "../vectors/golden.hpp"
#include "allocator.h"
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

static K_SEM_DEFINE(heap_locked, 0, 1);
static K_SEM_DEFINE(heap_unlock, 0, 1);
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
    ht_codec2_test_fail_after(-1);
}

static void after(void *) {
    zassert_equal(ht_codec2_test_heap_used(), 0, "Codec heap leaked");
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
    const unsigned allocations = ht_codec2_test_attempts();
    for (unsigned pair = 0; pair < 4; ++pair) {
        const Speech input = speech(pair);
        const Speech saved = input;
        Payload encoded;
        zassert_ok(codec.encode(input, encoded));
        zassert_mem_equal(encoded.bytes, encoded_speech[pair], sizeof(encoded.bytes));
        zassert_mem_equal(input.samples, saved.samples, sizeof(input.samples));
    }
    zassert_equal(ht_codec2_test_attempts(), allocations, "Encode allocated heap");
    const size_t used = ht_codec2_test_heap_used();
    zassert_true(used > 0 && used < 32768);
    printk("Codec2 live heap: %zu bytes\n", used);
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
    const unsigned allocations = ht_codec2_test_attempts();
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
    zassert_equal(ht_codec2_test_attempts(), allocations, "Frame processing allocated heap");
    size_t unused = 0;
    zassert_ok(k_thread_stack_space_get(k_current_get(), &unused));
    printk("Codec2 native test stack unused: %zu bytes\n", unused);
    zassert_true(unused >= 1024, "Codec test stack is nearly exhausted");
}

ZTEST(codec2, test_every_constructor_allocation_failure_is_clean) {
    VoiceCodec codec;
    zassert_ok(codec.open());
    const unsigned allocations = ht_codec2_test_attempts();
    codec.close();
    zassert_true(allocations >= 10);
    for (unsigned fail = 0; fail < allocations; ++fail) {
        ht_codec2_test_fail_after(fail);
        zassert_equal(codec.open(), -ENOMEM, "Allocation %u", fail);
        zassert_equal(ht_codec2_test_heap_used(), 0, "Allocation %u leaked", fail);
        Payload output;
        zassert_equal(codec.encode(speech(0), output), -ENODEV);
        ht_codec2_test_fail_after(-1);
        zassert_ok(codec.open());
        zassert_true(ht_codec2_test_heap_used() > 0);
        codec.close();
    }
}

ZTEST(codec2, test_bounded_heap_and_repeated_lifecycle) {
    VoiceCodec codec;
    VoiceCodec second;
    zassert_ok(codec.open());
    const size_t first_used = ht_codec2_test_heap_used();
    zassert_equal(second.open(), -ENOMEM); // The reserved heap fits one full state.
    zassert_equal(ht_codec2_test_heap_used(), first_used);
    Payload first;
    zassert_ok(codec.encode(speech(0), first));
    codec.close();
    zassert_ok(second.open());
    second.close();
    for (unsigned i = 0; i < 16; ++i) {
        zassert_ok(codec.open());
        zassert_equal(ht_codec2_test_heap_used(), first_used);
        codec.close();
        zassert_equal(ht_codec2_test_heap_used(), 0);
    }
}

static void hold_codec_lock(void *, void *, void *) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    k_sem_give(&heap_locked);
    k_sem_take(&heap_unlock, K_FOREVER);
    k_mutex_unlock(&codec_lock);
}

ZTEST(codec2, test_heap_measurement_tracks_peak_and_does_not_wait_for_codec) {
    VoiceHeapUsage empty;
    zassert_ok(voice_heap_usage(empty));
    zassert_equal(empty.used_bytes, 0);
    zassert_true(empty.free_bytes > 30000);
    VoiceCodec codec;
    zassert_ok(codec.open());
    VoiceHeapUsage active;
    zassert_ok(voice_heap_usage(active));
    zassert_equal(active.used_bytes, ht_codec2_test_heap_used());
    zassert_true(active.used_bytes > 30000);
    zassert_true(active.peak_bytes >= active.used_bytes);
    codec.close();
    VoiceHeapUsage closed;
    zassert_ok(voice_heap_usage(closed));
    zassert_equal(closed.used_bytes, 0);
    zassert_equal(closed.free_bytes, empty.free_bytes);
    zassert_equal(closed.peak_bytes, active.peak_bytes);
    k_thread_create(&lock_thread, lock_stack, K_THREAD_STACK_SIZEOF(lock_stack), hold_codec_lock,
                    nullptr, nullptr, nullptr, 8, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&heap_locked, K_MSEC(100)));
    VoiceHeapUsage busy = active;
    zassert_equal(voice_heap_usage(busy), -EAGAIN);
    zassert_equal(busy.used_bytes, 0);
    zassert_equal(busy.peak_bytes, 0);
    zassert_equal(busy.free_bytes, 0);
    k_sem_give(&heap_unlock);
    zassert_ok(k_thread_join(&lock_thread, K_MSEC(100)));
}

ZTEST_SUITE(codec2, nullptr, nullptr, before, after, nullptr);
