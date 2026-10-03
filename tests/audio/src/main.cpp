// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/audio_backend.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

using namespace ht;
static const char *fault;
static atomic_t stall_route;
static atomic_t poll_error;
static atomic_t running_channels;
static atomic_t tail_calls;
static atomic_t tail_delay = 50;
static atomic_t tail_error;
static atomic_t stall_tail;
static K_SEM_DEFINE(backend_block, 0, 1);
static K_SEM_DEFINE(backend_entered, 0, 1);
static K_SEM_DEFINE(tail_entered, 0, 1);
static K_SEM_DEFINE(waiter_entered, 0, 1);
static K_SEM_DEFINE(waiter_done, 0, 1);
static struct k_thread waiter;
K_THREAD_STACK_DEFINE(waiter_stack, 2048);
static AudioSession waiting_session;
static bool waiting_write;
static bool waiting_drain;
static int waiting_result;
static int16_t waiting_sample;
static int16_t input[2 * 1920];
static int16_t output[2 * 1920];
static int16_t block[1920];

static bool scenario(const char *name) {
    return strcmp(fault, name) == 0;
}

extern "C" int ht_audio_backend_open(void) {
    if (scenario("start-stall")) {
        k_sem_take(&backend_block, K_FOREVER);
    }
    return scenario("start-error") ? -EIO : 0;
}

extern "C" int ht_audio_backend_set_running(bool capture, bool playback) {
    if (atomic_get(&stall_route)) {
        k_sem_give(&backend_entered);
        k_sem_take(&backend_block, K_FOREVER);
    }
    atomic_set(&running_channels, (capture ? 1 : 0) | (playback ? 2 : 0));
    return 0;
}

extern "C" int ht_audio_backend_poll(uint32_t) {
    return atomic_get(&poll_error);
}

extern "C" int ht_audio_backend_tail_ms(uint32_t *delay_ms) {
    atomic_inc(&tail_calls);
    k_sem_give(&tail_entered);
    if (atomic_get(&stall_tail))
        k_sem_take(&backend_block, K_FOREVER);
    *delay_ms = atomic_get(&tail_delay);
    return atomic_get(&tail_error);
}

static void *setup() {
    fault = getenv("HT_AUDIO_FAULT");
    if (!fault) {
        fault = "overflow";
    }
    const int error = audio_start();
    if (scenario("start-stall")) {
        zassert_equal(error, -ETIMEDOUT);
    } else if (scenario("start-error")) {
        zassert_equal(error, -EIO);
    } else {
        zassert_ok(error);
    }
    return nullptr;
}

static void before(void *) {
    if (scenario("start-stall") || scenario("start-error")) {
        return;
    }
    audio_cancel();
    waiting_drain = false;
}

static void normal_only() {
    if (scenario("start-stall") || scenario("start-error")) {
        ztest_test_skip();
    }
}

static AudioSession route(const AudioRoutes &routes) {
    AudioSession session;
    zassert_ok(audio_route(routes, session));
    zassert_not_equal(session.id, 0);
    return session;
}

ZTEST(audio, test_capture_channel_mapping_and_phase_across_frames) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[0] = 8000;
    routes.input_rate[1] = 24000;
    const auto session = route(routes);
    for (int i = 0; i < 30; ++i) {
        input[2 * i] = 1000 + i; // Physical right is microphone in this test.
        input[2 * i + 1] = 2000 + i;
    }
    zassert_ok(ht_audio_capture(session.id, input, 7, 2, 1, 0));
    zassert_ok(ht_audio_capture(session.id, input + 14, 23, 2, 1, 0));
    zassert_ok(audio_read(session, AudioInput::Microphone, block, 5, 0));
    for (int i = 0; i < 5; ++i) {
        zassert_equal(block[i], 2000 + 6 * i);
    }
    zassert_ok(audio_read(session, AudioInput::Radio, block, 15, 0));
    for (int i = 0; i < 15; ++i) {
        zassert_equal(block[i], 1000 + 2 * i);
    }
    zassert_equal(audio_read(session, AudioInput::Radio, block, 1, 0), -EAGAIN);
}

ZTEST(audio, test_playback_repetition_rf_scaling_and_copy_ownership) {
    normal_only();
    AudioRoutes routes;
    routes.output[0] = routes.output[1] = AudioSource::Buffer;
    routes.output_rate[0] = 8000;
    routes.output_rate[1] = 24000;
    const auto session = route(routes);
    int16_t samples[] = {-32768, 32767, -5};
    zassert_ok(audio_write(session, AudioOutput::Speaker, samples, 3, 0));
    zassert_ok(audio_write(session, AudioOutput::Radio, samples, 3, 0));
    memset(samples, 0, sizeof(samples)); // Transport retained a copy, not this pointer.
    zassert_ok(ht_audio_playback(session.id, output, 5));
    zassert_ok(ht_audio_playback(session.id, output + 10, 13));
    const int16_t expected[] = {-32768, 32767, -5};
    for (int i = 0; i < 18; ++i) {
        zassert_equal(output[2 * i], expected[i / 6]);
        zassert_equal(output[2 * i + 1], i < 6 ? -(expected[i / 2] / 2) : 0);
    }
    zassert_true(audio_status().silence_samples[1] >= 12);
}

ZTEST(audio, test_fm_passthrough_preserves_level_and_discards_oldest_on_overflow) {
    normal_only();
    AudioRoutes routes;
    routes.output[0] = AudioSource::Radio;
    routes.output[1] = AudioSource::Microphone;
    const auto session = route(routes);
    const auto discarded = audio_status().fm_samples_discarded;
    for (int frame = 0; frame < 5; ++frame) {
        for (int i = 0; i < 1920; ++i) {
            input[2 * i] = frame * 1920 + i;
            input[2 * i + 1] = -(frame * 1920 + i);
        }
        zassert_ok(ht_audio_capture(session.id, input, 1920, 2, 0, 1));
    }
    zassert_equal(audio_status().fm_samples_discarded - discarded, 2 * 1920);
    zassert_ok(ht_audio_playback(session.id, output, 20));
    for (int i = 0; i < 20; ++i) {
        zassert_equal(output[2 * i], -(1920 + i));
        zassert_equal(output[2 * i + 1], 1920 + i); // FM RF is not halved/inverted.
    }
}

ZTEST(audio, test_mode_switch_cancels_stale_sessions_and_clears_samples) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[1] = 24000;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    const auto old = route(routes);
    block[0] = 900;
    zassert_ok(audio_write(old, AudioOutput::Radio, block, 1, 0));
    zassert_ok(ht_audio_capture(old.id, input, 10, 2, 0, 1));
    const auto current = route(routes);
    zassert_equal(audio_read(old, AudioInput::Radio, block, 1, 0), -ECANCELED);
    zassert_equal(audio_write(old, AudioOutput::Radio, block, 1, 0), -ECANCELED);
    zassert_equal(ht_audio_capture(old.id, input, 10, 2, 0, 1), -ECANCELED);
    memset(output, 1, 20 * sizeof(int16_t));
    zassert_equal(ht_audio_playback(old.id, output, 10), -ECANCELED);
    for (int i = 0; i < 20; ++i) {
        zassert_equal(output[i], 0);
    }
    zassert_equal(audio_read(current, AudioInput::Radio, block, 1, 0), -EAGAIN);
    zassert_ok(ht_audio_playback(current.id, output, 10));
    zassert_equal(output[1], 0);
}

static void blocking_transfer(void *, void *, void *) {
    int16_t sample = 321;
    k_sem_give(&waiter_entered);
    waiting_result = waiting_drain ? audio_drain(waiting_session, AudioOutput::Radio, 5000)
                     : waiting_write
                         ? audio_write(waiting_session, AudioOutput::Radio, &sample, 1, 5000)
                         : audio_read(waiting_session, AudioInput::Radio, &sample, 1, 5000);
    waiting_sample = sample;
    k_sem_give(&waiter_done);
}

static void wait_for_transfer() {
    k_sem_reset(&waiter_entered);
    k_sem_reset(&waiter_done);
    k_thread_create(&waiter, waiter_stack, K_THREAD_STACK_SIZEOF(waiter_stack), blocking_transfer,
                    nullptr, nullptr, nullptr, 8, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&waiter_entered, K_MSEC(100)));
    k_sleep(K_MSEC(10));
    zassert_equal(k_sem_take(&waiter_done, K_NO_WAIT), -EBUSY);
}

static void finish_waiter(int error) {
    zassert_ok(k_sem_take(&waiter_done, K_MSEC(100)));
    zassert_ok(k_thread_join(&waiter, K_MSEC(100)));
    zassert_equal(waiting_result, error);
}

ZTEST(audio, test_cancel_wakes_blocked_read_and_full_queue_writer) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[1] = 24000;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    waiting_session = route(routes);
    waiting_write = false;
    wait_for_transfer();
    audio_cancel();
    finish_waiter(-ECANCELED);

    waiting_session = route(routes);
    for (int i = 0; i < 4; ++i) {
        zassert_ok(audio_write(waiting_session, AudioOutput::Radio, block, 1920, 0));
    }
    waiting_write = true;
    wait_for_transfer();
    audio_cancel();
    finish_waiter(-ECANCELED);
}

ZTEST(audio, test_capture_and_playback_wake_waiters_when_data_or_space_arrives) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[1] = 48000;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    waiting_session = route(routes);
    waiting_write = false;
    wait_for_transfer();
    input[0] = 111;
    input[1] = 222;
    zassert_ok(ht_audio_capture(waiting_session.id, input, 1, 2, 0, 1));
    finish_waiter(0);
    zassert_equal(waiting_sample, 222);

    for (auto &sample : block) {
        sample = 123;
    }
    for (int i = 0; i < 4; ++i) {
        zassert_ok(audio_write(waiting_session, AudioOutput::Radio, block, 1920, 0));
    }
    waiting_write = true;
    wait_for_transfer();
    zassert_ok(ht_audio_playback(waiting_session.id, output, 1));
    finish_waiter(0);
    for (int i = 0; i < 4; ++i) {
        zassert_ok(ht_audio_playback(waiting_session.id, output, 1920));
    }
    zassert_equal(output[2 * 1919 + 1], -(321 / 2));
}

ZTEST(audio, test_validation_and_timeout_do_not_change_routes) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[1] = 16000;
    const auto session = route(routes);
    AudioSession rejected;
    routes.input_rate[1] = 44100;
    zassert_equal(audio_route(routes, rejected), -EINVAL);
    zassert_equal(audio_read(session, AudioInput::Radio, block, 1, 15), -ETIMEDOUT);
    zassert_ok(audio_status().error);
    zassert_equal(audio_read(session, static_cast<AudioInput>(2), block, 1, 0), -EINVAL);
    zassert_equal(audio_read(session, AudioInput::Radio, nullptr, 1, 0), -EINVAL);
    zassert_equal(audio_read(session, AudioInput::Radio, block, 1921, 0), -EINVAL);
    zassert_equal(ht_audio_capture(session.id, input, 1, 2, 0, 0), -EINVAL);
    zassert_equal(audio_write(session, AudioOutput::Speaker, block, 1, 0), -ENODEV);
    zassert_equal(audio_drain(session, AudioOutput::Speaker, 1), -ENODEV);
    zassert_equal(audio_drain(session, static_cast<AudioOutput>(2), 1), -EINVAL);
    zassert_equal(atomic_get(&running_channels), 1);
}

ZTEST(audio, test_drain_waits_for_queue_repetitions_and_backend_tail) {
    normal_only();
    AudioRoutes routes;
    routes.input_rate[1] = 24000;
    routes.output[0] = routes.output[1] = AudioSource::Buffer;
    routes.output_rate[0] = routes.output_rate[1] = 8000;
    waiting_session = route(routes);
    block[0] = 123;
    zassert_equal(audio_drain(waiting_session, AudioOutput::Radio, 0), -EAGAIN);
    zassert_ok(audio_write(waiting_session, AudioOutput::Radio, block, 1, 0));
    const auto calls = atomic_get(&tail_calls);
    k_sem_reset(&tail_entered);
    waiting_drain = true;
    wait_for_transfer();
    zassert_equal(atomic_get(&tail_calls), calls, "Tail began before queue consumption");
    zassert_equal(audio_write(waiting_session, AudioOutput::Radio, block, 1, 0), -EBUSY);
    zassert_equal(audio_drain(waiting_session, AudioOutput::Radio, 1), -EBUSY);
    zassert_ok(audio_write(waiting_session, AudioOutput::Speaker, block, 1, 0));
    zassert_ok(ht_audio_capture(waiting_session.id, input, 2, 2, 0, 1));
    zassert_ok(audio_read(waiting_session, AudioInput::Radio, block, 1, 0));
    zassert_ok(ht_audio_playback(waiting_session.id, output, 5));
    k_sleep(K_MSEC(5));
    zassert_equal(atomic_get(&tail_calls), calls, "Last sample still needs one repeat");
    zassert_ok(ht_audio_playback(waiting_session.id, output, 1));
    zassert_ok(k_sem_take(&tail_entered, K_MSEC(100)));
    zassert_equal(k_sem_take(&waiter_done, K_NO_WAIT), -EBUSY, "DSP tail was skipped");
    finish_waiter(0);
    zassert_equal(atomic_get(&tail_calls), calls + 1);
    zassert_ok(audio_write(waiting_session, AudioOutput::Radio, block, 1, 0));
}

ZTEST(audio, test_drain_timeout_preserves_queued_samples_and_allows_retry) {
    normal_only();
    AudioRoutes routes;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    const auto session = route(routes);
    block[0] = 987;
    zassert_ok(audio_write(session, AudioOutput::Radio, block, 1, 0));
    const auto calls = atomic_get(&tail_calls);
    zassert_equal(audio_drain(session, AudioOutput::Radio, 15), -ETIMEDOUT);
    zassert_equal(atomic_get(&tail_calls), calls);
    zassert_ok(audio_status().error);
    zassert_ok(ht_audio_playback(session.id, output, 1));
    zassert_equal(output[1], -(987 / 2));
    // A short deadline during the DSP tail is also a caller timeout, not a
    // transport fault. A later drain can finish without a new route.
    zassert_equal(audio_drain(session, AudioOutput::Radio, 15), -ETIMEDOUT);
    zassert_ok(audio_status().error);
    zassert_ok(audio_drain(session, AudioOutput::Radio, 100));
    zassert_ok(audio_write(session, AudioOutput::Radio, block, 1, 0));
}

ZTEST(audio, test_cancel_wakes_drain_before_and_after_backend_submission) {
    normal_only();
    AudioRoutes routes;
    routes.output[1] = AudioSource::Buffer;
    routes.output_rate[1] = 48000;
    waiting_drain = true;
    waiting_session = route(routes);
    zassert_ok(audio_write(waiting_session, AudioOutput::Radio, block, 1, 0));
    wait_for_transfer();
    const auto stale = waiting_session;
    audio_cancel();
    finish_waiter(-ECANCELED);
    zassert_equal(audio_drain(stale, AudioOutput::Radio, 1), -ECANCELED);
    waiting_session = route(routes);
    k_sem_reset(&tail_entered);
    wait_for_transfer();
    zassert_ok(k_sem_take(&tail_entered, K_MSEC(100)));
    audio_cancel();
    finish_waiter(-ECANCELED);
    const auto current = route(routes);
    zassert_ok(audio_drain(current, AudioOutput::Radio, 100));
}

static void blocked_route(void *, void *, void *) {
    AudioSession session;
    AudioRoutes routes;
    routes.input_rate[1] = 24000;
    waiting_result = audio_route(routes, session);
    k_sem_give(&waiter_done);
}

ZTEST(audio, test_z_fault_latches_and_does_not_require_vendor_stop) {
    if (scenario("start-stall") || scenario("start-error")) {
        const int expected = scenario("start-stall") ? -ETIMEDOUT : -EIO;
        zassert_equal(audio_status().error, expected);
        audio_cancel();
        zassert_equal(audio_start(), expected);
        if (scenario("start-stall")) {
            k_sem_give(&backend_block);
            k_sleep(K_MSEC(10));
            zassert_equal(audio_status().error, expected); // Late startup cannot recover.
        }
        return;
    }
    if (scenario("route-stall")) {
        atomic_set(&stall_route, 1);
        k_thread_create(&waiter, waiter_stack, K_THREAD_STACK_SIZEOF(waiter_stack), blocked_route,
                        nullptr, nullptr, nullptr, 8, 0, K_NO_WAIT);
        zassert_ok(k_sem_take(&backend_entered, K_MSEC(100)));
        audio_cancel(); // Returns despite the worker's stuck RPC.
        finish_waiter(-ECANCELED);
        k_sleep(K_MSEC(1010));
        zassert_equal(audio_status().error, -ETIMEDOUT);
        atomic_set(&stall_route, 0);
        k_sem_give(&backend_block);
    } else if (scenario("tail-stall") || scenario("tail-error")) {
        AudioRoutes routes;
        routes.output[1] = AudioSource::Buffer;
        routes.output_rate[1] = 48000;
        waiting_session = route(routes);
        waiting_drain = true;
        atomic_set(&stall_tail, 1);
        k_sem_reset(&tail_entered);
        wait_for_transfer();
        zassert_ok(k_sem_take(&tail_entered, K_MSEC(100)));
        if (scenario("tail-stall")) {
            audio_cancel(); // A blocked tail query holds no application mutex.
            finish_waiter(-ECANCELED);
            k_sleep(K_MSEC(1010));
            zassert_equal(audio_status().error, -ETIMEDOUT);
        } else {
            atomic_set(&tail_error, -EPIPE);
        }
        atomic_clear(&stall_tail);
        k_sem_give(&backend_block);
        if (scenario("tail-error")) {
            finish_waiter(-EPIPE);
            zassert_equal(audio_status().error, -EPIPE);
        }
    } else if (scenario("poll-error")) {
        AudioRoutes routes;
        routes.input_rate[1] = 24000;
        waiting_session = route(routes);
        waiting_write = false;
        wait_for_transfer();
        atomic_set(&poll_error, -EPIPE);
        finish_waiter(-EPIPE);
        zassert_equal(audio_status().error, -EPIPE);
    } else {
        AudioRoutes routes;
        routes.input_rate[1] = 48000;
        const auto session = route(routes);
        for (int i = 0; i < 4; ++i) {
            zassert_ok(ht_audio_capture(session.id, input, 1920, 2, 0, 1));
        }
        zassert_equal(ht_audio_capture(session.id, input, 1, 2, 0, 1), -EOVERFLOW);
        zassert_equal(audio_status().error, -EOVERFLOW);
    }
    const int latched = audio_status().error;
    zassert_not_equal(latched, 0);
    k_sleep(K_MSEC(10));
    AudioSession session;
    zassert_equal(audio_route({}, session), latched);
    zassert_equal(audio_start(), latched);
    zassert_equal(audio_read(session, AudioInput::Radio, block, 1, 0), latched);
    zassert_equal(audio_drain(session, AudioOutput::Radio, 100), latched);
    const int64_t stop_deadline = k_uptime_get() + 100;
    while (atomic_get(&running_channels) && k_uptime_get() < stop_deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_equal(atomic_get(&running_channels), 0);
}

ZTEST_SUITE(audio, nullptr, setup, before, nullptr, nullptr);
