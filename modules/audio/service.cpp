// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/audio.hpp>
#include <ht/audio_backend.h>
#include <zephyr/kernel.h>

namespace ht {
static constexpr size_t queue_samples = 7680; // Reference FM capacity: 160 ms at 48 kHz.
static constexpr size_t max_transfer = 1920;  // One 40 ms M17 modulation frame.
static constexpr uint32_t dsp_rate = 48000;
static constexpr int64_t io_timeout_ms = 1000;
static constexpr int64_t startup_timeout_ms = 3000;

struct SampleQueue {
    int16_t samples[queue_samples];
    size_t head;
    size_t count;

    void clear() {
        head = count = 0;
    }

    void push(int16_t sample) {
        samples[(head + count) % queue_samples] = sample;
        ++count;
    }

    int16_t pop() {
        const int16_t sample = samples[head];
        head = (head + 1) % queue_samples;
        --count;
        return sample;
    }
};

// DSP/application buffers must not compete with the 32 KiB kernel heap.
#ifdef CONFIG_BOARD_C62
#define AUDIO_RAM __attribute__((section(".psram_section")))
#else
#define AUDIO_RAM
#endif
static SampleQueue input_queue[2] AUDIO_RAM;
static SampleQueue output_queue[2] AUDIO_RAM;
static SampleQueue fm_queue[2] AUDIO_RAM;
static unsigned input_phase[2];
static unsigned output_phase[2];
static int16_t output_sample[2];
enum class DrainState : uint8_t { Idle, Queued, Tail, Done };

struct Drain {
    DrainState state = DrainState::Idle;
    int64_t deadline = 0;
};

static Drain drains[2];
static K_MUTEX_DEFINE(mutex);
static K_CONDVAR_DEFINE(changed);
static AudioRoutes routes;
static AudioStatus status;
static bool started;
static bool ready;
static uint32_t revision;
static uint32_t applied;
static int64_t heartbeat;
static struct k_thread worker;
K_THREAD_STACK_DEFINE(worker_stack, 8192);

static bool valid_rate(uint32_t rate) {
    return rate == 8000 || rate == 16000 || rate == 24000 || rate == dsp_rate;
}

static int validate(const AudioRoutes &request) {
    for (unsigned i = 0; i < 2; ++i) {
        if ((request.input_rate[i] && !valid_rate(request.input_rate[i])) ||
            request.output[i] > AudioSource::Buffer ||
            (request.output[i] == AudioSource::Buffer ? !valid_rate(request.output_rate[i])
                                                      : request.output_rate[i] != 0)) {
            return -EINVAL;
        }
    }
    return 0;
}

static void cancel_locked() {
    if (++revision == 0) {
        ++revision; // Zero never identifies an active session.
    }
    routes = {};
    for (unsigned i = 0; i < 2; ++i) {
        input_queue[i].clear();
        output_queue[i].clear();
        fm_queue[i].clear();
        input_phase[i] = output_phase[i] = 0;
        output_sample[i] = 0;
        drains[i] = {};
    }
    k_condvar_broadcast(&changed);
}

static void fail_locked(int error) {
    if (!status.error) {
        status.error = error < 0 ? error : -EIO;
        cancel_locked();
    }
}

static void check_health_locked() {
    if (started && !status.error &&
        k_uptime_get() - heartbeat > (ready ? io_timeout_ms : startup_timeout_ms)) {
        fail_locked(-ETIMEDOUT);
    }
}

static int session_error_locked(uint32_t session) {
    if (status.error) {
        return status.error;
    }
    return !session || session != revision || applied != revision ? -ECANCELED : 0;
}

static bool output_empty(unsigned channel) {
    // The last low-rate sample may already be popped but still need repeats.
    return !output_queue[channel].count && !output_phase[channel];
}

static void update_drains(uint32_t session) {
    unsigned pending = 0;
    k_mutex_lock(&mutex, K_FOREVER);
    if (!session_error_locked(session)) {
        for (unsigned i = 0; i < 2; ++i) {
            if (drains[i].state == DrainState::Queued && output_empty(i))
                pending |= 1u << i;
        }
    }
    k_mutex_unlock(&mutex);
    uint32_t delay_ms = 0;
    const int error = pending ? ht_audio_backend_tail_ms(&delay_ms) : 0;
    k_mutex_lock(&mutex, K_FOREVER);
    if (error) {
        fail_locked(error);
    } else if (!session_error_locked(session)) {
        const int64_t now = k_uptime_get();
        for (unsigned i = 0; i < 2; ++i) {
            auto &drain = drains[i];
            if ((pending & (1u << i)) && drain.state == DrainState::Queued && output_empty(i)) {
                // Recheck after the vendor call: a caller may have timed out,
                // written another block and started a new drain in the meantime.
                drain.state = DrainState::Tail;
                drain.deadline = now + delay_ms;
            }
            if (drain.state == DrainState::Tail && now >= drain.deadline) {
                drain.state = DrainState::Done;
                k_condvar_broadcast(&changed);
            }
        }
    }
    k_mutex_unlock(&mutex);
}

static void audio_worker(void *, void *, void *) {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "audio");
#endif
    const int error = ht_audio_backend_open();
    k_mutex_lock(&mutex, K_FOREVER);
    if (error) {
        fail_locked(error);
    } else {
        ready = true;
        heartbeat = k_uptime_get();
    }
    k_condvar_broadcast(&changed);
    k_mutex_unlock(&mutex);
    uint32_t current = 0;
    while (true) {
        k_mutex_lock(&mutex, K_FOREVER);
        const bool failed = status.error != 0;
        const uint32_t next = revision;
        const AudioRoutes request = routes;
        k_mutex_unlock(&mutex);
        if (failed) {
            // A blocked SDK call may never return. Leave its objects alive
            // until reboot; cancellation and the PA shutdown do not wait here.
            ht_audio_backend_set_running(false, false);
            return;
        }
        if (next != current) {
            const bool capture = request.input_rate[0] || request.input_rate[1] ||
                                 request.output[0] == AudioSource::Microphone ||
                                 request.output[0] == AudioSource::Radio ||
                                 request.output[1] == AudioSource::Microphone ||
                                 request.output[1] == AudioSource::Radio;
            const bool playback = request.output[0] != AudioSource::Silence ||
                                  request.output[1] != AudioSource::Silence;
            const int result = ht_audio_backend_set_running(capture, playback);
            k_mutex_lock(&mutex, K_FOREVER);
            if (result) {
                fail_locked(result);
            } else if (revision == next && !status.error) {
                applied = next;
            }
            heartbeat = k_uptime_get();
            k_condvar_broadcast(&changed);
            k_mutex_unlock(&mutex);
            current = next;
            continue;
        }
        const int result = ht_audio_backend_poll(current);
        if (!result)
            update_drains(current);
        k_mutex_lock(&mutex, K_FOREVER);
        if (result && result != -ECANCELED) {
            fail_locked(result);
        }
        heartbeat = k_uptime_get();
        k_mutex_unlock(&mutex);
        k_sleep(K_MSEC(1));
    }
}

int audio_start() {
    k_mutex_lock(&mutex, K_FOREVER);
    if (!started) {
        started = true;
        heartbeat = k_uptime_get();
        cancel_locked();
        k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), audio_worker,
                        nullptr, nullptr, nullptr, 6, 0, K_NO_WAIT);
    }
    const int64_t deadline = k_uptime_get() + startup_timeout_ms;
    while (!ready && !status.error) {
        const int64_t remaining = deadline - k_uptime_get();
        if (remaining <= 0) {
            fail_locked(-ETIMEDOUT);
            break;
        }
        k_condvar_wait(&changed, &mutex, K_MSEC(remaining));
    }
    check_health_locked();
    const int error = status.error;
    k_mutex_unlock(&mutex);
    return error;
}

int audio_route(const AudioRoutes &request, AudioSession &session) {
    const int validation = validate(request);
    if (validation) {
        return validation;
    }
    k_mutex_lock(&mutex, K_FOREVER);
    check_health_locked();
    int error = status.error;
    if (!error && !ready) {
        error = -ENODEV;
    }
    if (!error) {
        cancel_locked();
        routes = request;
        const uint32_t target = revision;
        const int64_t deadline = k_uptime_get() + io_timeout_ms;
        while (applied != target && revision == target && !status.error) {
            const int64_t remaining = deadline - k_uptime_get();
            if (remaining <= 0) {
                fail_locked(-ETIMEDOUT);
                break;
            }
            k_condvar_wait(&changed, &mutex, K_MSEC(remaining));
        }
        error = status.error ? status.error : revision != target ? -ECANCELED : 0;
        if (!error) {
            session.id = target;
        }
    }
    k_mutex_unlock(&mutex);
    return error;
}

void audio_cancel() {
    k_mutex_lock(&mutex, K_FOREVER);
    cancel_locked();
    k_mutex_unlock(&mutex);
}

void audio_report_error(int error) {
    k_mutex_lock(&mutex, K_FOREVER);
    fail_locked(error);
    k_mutex_unlock(&mutex);
}

AudioStatus audio_status() {
    k_mutex_lock(&mutex, K_FOREVER);
    check_health_locked();
    const auto result = status;
    k_mutex_unlock(&mutex);
    return result;
}

static int transfer(AudioSession session, unsigned channel, int16_t *destination,
                    const int16_t *source, size_t count, uint32_t timeout_ms) {
    if (channel >= 2 || !count || count > max_transfer || (!destination && !source)) {
        return -EINVAL;
    }
    const bool input = destination != nullptr;
    k_mutex_lock(&mutex, K_FOREVER);
    SampleQueue &queue = input ? input_queue[channel] : output_queue[channel];
    const int64_t deadline = k_uptime_get() + timeout_ms;
    int error = 0;
    while (true) {
        check_health_locked();
        error = session_error_locked(session.id);
        if (error) {
            break;
        }
        if (input ? !routes.input_rate[channel] : routes.output[channel] != AudioSource::Buffer) {
            error = -ENODEV;
        } else if (!input && drains[channel].state != DrainState::Idle) {
            error = -EBUSY;
        } else if (input ? queue.count >= count : queue_samples - queue.count >= count) {
            break;
        } else if (!timeout_ms) {
            error = -EAGAIN;
        }
        if (error) {
            break;
        }
        const int64_t remaining = deadline - k_uptime_get();
        if (remaining <= 0) {
            error = -ETIMEDOUT;
            break;
        }
        // Wake periodically to observe a stuck worker even with a long caller timeout.
        k_condvar_wait(&changed, &mutex, K_MSEC(MIN(remaining, 100)));
    }
    if (!error) {
        for (size_t i = 0; i < count; ++i) {
            if (input) {
                destination[i] = queue.pop();
            } else {
                queue.push(source[i]);
            }
        }
        k_condvar_broadcast(&changed);
    }
    k_mutex_unlock(&mutex);
    return error;
}

int audio_read(AudioSession session, AudioInput input, int16_t *samples, size_t count,
               uint32_t timeout_ms) {
    return transfer(session, static_cast<unsigned>(input), samples, nullptr, count, timeout_ms);
}

int audio_write(AudioSession session, AudioOutput output, const int16_t *samples, size_t count,
                uint32_t timeout_ms) {
    return transfer(session, static_cast<unsigned>(output), nullptr, samples, count, timeout_ms);
}

int audio_drain(AudioSession session, AudioOutput output, uint32_t timeout_ms) {
    const unsigned channel = static_cast<unsigned>(output);
    if (channel >= 2)
        return -EINVAL;
    k_mutex_lock(&mutex, K_FOREVER);
    check_health_locked();
    int error = session_error_locked(session.id);
    if (!error && routes.output[channel] != AudioSource::Buffer)
        error = -ENODEV;
    if (!error && drains[channel].state != DrainState::Idle)
        error = -EBUSY;
    if (!error && !timeout_ms)
        error = -EAGAIN;
    if (!error) {
        drains[channel].state = DrainState::Queued;
        const int64_t deadline = k_uptime_get() + timeout_ms;
        k_condvar_broadcast(&changed);
        while (true) {
            check_health_locked();
            error = session_error_locked(session.id);
            if (error || drains[channel].state == DrainState::Done)
                break;
            const int64_t remaining = deadline - k_uptime_get();
            if (remaining <= 0) {
                error = -ETIMEDOUT;
                break;
            }
            k_condvar_wait(&changed, &mutex, K_MSEC(MIN(remaining, 100)));
        }
        if (session.id == revision)
            drains[channel] = {};
    }
    k_mutex_unlock(&mutex);
    return error;
}
} // namespace ht

extern "C" int ht_audio_capture(uint32_t session, const int16_t *samples, size_t frames,
                                size_t channels, size_t microphone_index, size_t radio_index) {
    using namespace ht;
    if (!samples || !frames || frames > max_transfer || channels > 6 ||
        microphone_index >= channels || radio_index >= channels ||
        microphone_index == radio_index) {
        return -EINVAL;
    }
    k_mutex_lock(&mutex, K_FOREVER);
    int error = session_error_locked(session);
    const size_t indices[] = {microphone_index, radio_index};
    for (size_t n = 0; n < frames && !error; ++n) {
        for (unsigned source = 0; source < 2 && !error; ++source) {
            const int16_t sample = samples[n * channels + indices[source]];
            if (routes.input_rate[source]) {
                if (!input_phase[source]) {
                    auto &queue = input_queue[source];
                    if (queue.count == queue_samples) {
                        fail_locked(-EOVERFLOW);
                        error = status.error;
                        break;
                    }
                    queue.push(sample);
                }
                input_phase[source] =
                    (input_phase[source] + 1) % (dsp_rate / routes.input_rate[source]);
            }
            for (unsigned sink = 0; sink < 2; ++sink) {
                const AudioSource selected = source ? AudioSource::Radio : AudioSource::Microphone;
                if (routes.output[sink] == selected) {
                    auto &queue = fm_queue[sink];
                    if (queue.count == queue_samples) {
                        queue.pop();
                        ++status.fm_samples_discarded;
                    }
                    queue.push(sample); // Reference FM passthrough is unchanged.
                }
            }
        }
    }
    k_condvar_broadcast(&changed);
    k_mutex_unlock(&mutex);
    return error;
}

extern "C" int ht_audio_playback(uint32_t session, int16_t *samples, size_t frames) {
    using namespace ht;
    if (!samples || !frames || frames > max_transfer) {
        return -EINVAL;
    }
    k_mutex_lock(&mutex, K_FOREVER);
    const int error = session_error_locked(session);
    for (size_t n = 0; n < frames; ++n) {
        for (unsigned sink = 0; sink < 2; ++sink) {
            int32_t sample = 0;
            if (!error && routes.output[sink] == AudioSource::Buffer) {
                auto &queue = output_queue[sink];
                if (output_phase[sink] || queue.count) {
                    if (!output_phase[sink]) {
                        output_sample[sink] = queue.pop();
                    }
                    sample = output_sample[sink];
                    if (sink == 1) {
                        sample = -(sample / 2); // Tested C62 RF level and polarity.
                    }
                    output_phase[sink] =
                        (output_phase[sink] + 1) % (dsp_rate / routes.output_rate[sink]);
                } else {
                    ++status.silence_samples[sink];
                }
            } else if (!error && routes.output[sink] != AudioSource::Silence) {
                sample = fm_queue[sink].count ? fm_queue[sink].pop() : 0;
            }
            samples[2 * n + sink] = sample;
        }
    }
    k_condvar_broadcast(&changed);
    k_mutex_unlock(&mutex);
    return error;
}
