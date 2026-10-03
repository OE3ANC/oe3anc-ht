/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Microphone DC blocker adapted from OpenRTX core/dsp.c and audio_codec.c
 * at 9d800e69f5f3c7275857c232c32786e6526ead80; original dspGuru algorithm:
 * https://dspguru.com/dsp/tricks/fixed-point-dc-blocking-filter-with-noise-shaping/
 * Worker ownership and lifecycle are new project code.
 */
#include <errno.h>
#include <ht/m17_mode.hpp>
#include <ht/m17_modem.hpp>
#include <ht/voice.hpp>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
namespace {
enum class Operation : uint8_t { None, Receive, Transmit };

struct Request {
    Operation operation = Operation::None;
    AudioSession session;
    char callsign[10] = {};
    M17Settings settings;
    bool begin = false;
    bool finish = false;
};

static K_MUTEX_DEFINE(mutex);
static K_CONDVAR_DEFINE(changed);
static Request request;
static M17Status status;
static uint32_t revision;
static int64_t heartbeat;

struct Processing {
    m17::VoiceCodec codec;
    m17::Encoder encoder;
    m17::Decoder decoder;
    m17::Modulator modulator;
    m17::Demodulator demodulator;
    m17::Speech speech;
    m17::TxSamples output;
    m17::Frame frame;
    int16_t input[240];
    int64_t dc_accumulator = 0;
    int32_t dc_input = 0;
    int32_t dc_output = 0;
};
#ifdef CONFIG_BOARD_C62
static Processing processing __attribute__((section(".psram_section")));
#else
static Processing processing;
#endif

static void next_revision() {
    if (++revision == 0)
        ++revision;
}

static void fail_locked(int error) {
    if (!status.error) {
        status = {};
        status.phase = M17Phase::Fault;
        status.error = error < 0 ? error : -EIO;
        request = {};
        next_revision();
        k_condvar_broadcast(&changed);
    }
}

static void check_health() {
    if ((status.phase == M17Phase::Starting || status.phase == M17Phase::Receiving ||
         status.phase == M17Phase::Transmitting) &&
        k_uptime_get() - heartbeat > 1000)
        fail_locked(-ETIMEDOUT);
}

static bool current(uint32_t job, bool *finish = nullptr) {
    k_mutex_lock(&mutex, K_FOREVER);
    const bool active = job == revision && !status.error;
    if (active) {
        heartbeat = k_uptime_get();
        if (finish)
            *finish = request.finish;
    }
    k_mutex_unlock(&mutex);
    return active;
}

static void publish_phase(uint32_t job, M17Phase phase) {
    k_mutex_lock(&mutex, K_FOREVER);
    if (job == revision && !status.error) {
        status.phase = phase;
        heartbeat = k_uptime_get();
        k_condvar_broadcast(&changed);
    }
    k_mutex_unlock(&mutex);
}

static void publish_rx(uint32_t job, bool active, const char *callsign) {
    k_mutex_lock(&mutex, K_FOREVER);
    if (job == revision && !status.error) {
        status.rx_active = active;
        memcpy(status.callsign, callsign, sizeof(status.callsign));
    }
    k_mutex_unlock(&mutex);
}

static int send(uint32_t job, AudioSession session, const m17::Frame &frame) {
    if (!current(job))
        return -ECANCELED;
    processing.modulator.render(frame, processing.output);
    if (!current(job))
        return -ECANCELED;
    return audio_write(session, AudioOutput::Radio, processing.output.samples, 1920, 200);
}

static int16_t microphone_sample(int16_t sample) {
    // Reference C62 DC pole 0.995, gain 1, no microphone oversampling.
    // Multiplication and explicit floor rounding avoid signed-shift UB.
    auto &p = processing;
    p.dc_accumulator -= p.dc_input;
    p.dc_input = static_cast<int32_t>(sample) * 32768;
    p.dc_accumulator += p.dc_input;
    p.dc_accumulator -= static_cast<int64_t>(164) * p.dc_output;
    const int64_t output =
        p.dc_accumulator >= 0 ? p.dc_accumulator / 32768 : -((-p.dc_accumulator + 32767) / 32768);
    p.dc_output = static_cast<int32_t>(output);
    return output < INT16_MIN ? INT16_MIN : output > INT16_MAX ? INT16_MAX : output;
}

static int transmit(uint32_t job, const Request &work) {
    auto &p = processing;
    m17::LinkSetup link;
    if (!m17::make_voice_link(work.callsign, strlen(work.callsign), link, work.settings))
        return -EINVAL;
    p.modulator.reset();
    p.dc_accumulator = p.dc_input = p.dc_output = 0;
    publish_phase(job, M17Phase::Prepared);
    k_mutex_lock(&mutex, K_FOREVER);
    while (job == revision && !request.begin && !status.error)
        k_condvar_wait(&changed, &mutex, K_FOREVER);
    const bool active = job == revision && !status.error;
    k_mutex_unlock(&mutex);
    if (!active)
        return -ECANCELED;
    publish_phase(job, M17Phase::Transmitting);
    m17::preamble(p.frame);
    int error = send(job, work.session, p.frame);
    if (!error)
        error = send(job, work.session, p.frame);
    p.encoder.start(link, p.frame);
    if (!error)
        error = send(job, work.session, p.frame);
    while (!error) {
        bool last = false;
        p.speech = {};
        size_t count = 0;
        while (count < 320 && current(job, &last) && !last) {
            error = audio_read(work.session, AudioInput::Microphone, p.input, 80, 100);
            if (error)
                break;
            for (unsigned i = 0; i < 80; ++i)
                p.speech.samples[count++] = microphone_sample(p.input[i]);
        }
        if (!current(job, &last))
            return -ECANCELED;
        if (error)
            break;
        m17::Payload payload;
        error = p.codec.encode(p.speech, payload);
        if (!current(job, &last))
            return -ECANCELED;
        if (!error && !p.encoder.stream(payload, last, p.frame))
            error = -EIO;
        if (!error)
            error = send(job, work.session, p.frame);
        if (last)
            break;
    }
    if (!error) {
        m17::end_marker(p.frame);
        error = send(job, work.session, p.frame);
    }
    if (!error && current(job))
        error = audio_drain(work.session, AudioOutput::Radio, 500);
    if (!error && current(job))
        publish_phase(job, M17Phase::Finished);
    return error;
}

static int receive(uint32_t job, const Request &work) {
    auto &p = processing;
    p.demodulator.reset();
    p.decoder.reset();
    bool accepted = false;
    bool locked = false;
    bool reset_codec = true;
    m17::LinkSetup previous_link;
    char source[10] = {};
    publish_phase(job, M17Phase::Receiving);
    while (current(job)) {
        int error = audio_read(work.session, AudioInput::Radio, p.input, 240, 100);
        if (error)
            return error;
        if (!current(job))
            return -ECANCELED;
        for (const auto sample : p.input) {
            const bool complete = p.demodulator.sample(sample, p.frame);
            if (p.demodulator.locked() != locked) {
                locked = p.demodulator.locked();
                p.decoder.reset();
                accepted = false;
                reset_codec = true;
                memset(source, 0, sizeof(source));
                publish_rx(job, false, source);
            }
            if (!complete)
                continue;
            const auto decoded = p.decoder.decode(p.frame);
            if (decoded.link_updated) {
                if (decoded.kind == m17::FrameKind::LinkSetup ||
                    memcmp(previous_link.bytes, decoded.link.bytes, sizeof(previous_link.bytes)))
                    reset_codec = true;
                previous_link = decoded.link;
                accepted =
                    m17::accept_voice_link(decoded.link, work.callsign, work.settings, source);
                if (!accepted)
                    memset(source, 0, sizeof(source));
                publish_rx(job, accepted, source);
            } else if (decoded.kind == m17::FrameKind::LinkSetup) {
                accepted = false;
                reset_codec = true;
                memset(source, 0, sizeof(source));
                publish_rx(job, false, source);
            }
            if (accepted && decoded.payload_valid) {
                if (reset_codec) {
                    error = p.codec.open();
                    reset_codec = false;
                }
                if (!error)
                    error = p.codec.decode(decoded.payload, p.speech);
                if (!current(job))
                    return -ECANCELED;
                if (!error)
                    error =
                        audio_write(work.session, AudioOutput::Speaker, p.speech.samples, 320, 100);
                if (error)
                    return error;
            }
            if (decoded.last || decoded.kind == m17::FrameKind::End) {
                accepted = false;
                reset_codec = true;
                p.decoder.reset();
                memset(source, 0, sizeof(source));
                publish_rx(job, false, source);
            }
        }
    }
    return -ECANCELED;
}

static void worker(void *, void *, void *) {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "m17");
#endif
    uint32_t previous = 0;
    while (true) {
        k_mutex_lock(&mutex, K_FOREVER);
        while (revision == previous || request.operation == Operation::None)
            k_condvar_wait(&changed, &mutex, K_FOREVER);
        const uint32_t job = revision;
        const Request work = request;
        previous = job;
        k_mutex_unlock(&mutex);
        int error = processing.codec.open();
        if (!error && current(job))
            error = work.operation == Operation::Receive ? receive(job, work) : transmit(job, work);
        processing.codec.close();
        k_mutex_lock(&mutex, K_FOREVER);
        if (job == revision && error) {
            if (error == -ECANCELED) {
                // The controller may invalidate audio just before cancelling
                // processing. That ordinary stop is not a reboot-only fault.
                status = {};
                request = {};
                next_revision();
                k_condvar_broadcast(&changed);
            } else {
                fail_locked(error);
            }
        }
        k_mutex_unlock(&mutex);
    }
}

K_THREAD_DEFINE(worker_thread, 24576, worker, nullptr, nullptr, nullptr, 7, 0, 0);

static int start(Operation operation, AudioSession session, const char *local,
                 const M17Settings &settings) {
    if (!session.id || !local || !valid_m17_settings(settings))
        return -EINVAL;
    size_t length = 0;
    while (length < 10 && local[length])
        ++length;
    if (length == 10)
        return -EINVAL;
    m17::Address address;
    if ((operation == Operation::Transmit || *local) &&
        !m17::encode_callsign(local, length, address))
        return -EINVAL;
    k_mutex_lock(&mutex, K_FOREVER);
    check_health();
    if (status.error) {
        const int error = status.error;
        k_mutex_unlock(&mutex);
        return error;
    }
    request = {};
    request.operation = operation;
    request.session = session;
    request.settings = settings;
    memcpy(request.callsign, local, length);
    status = {};
    status.phase = M17Phase::Starting;
    heartbeat = k_uptime_get();
    next_revision();
    const uint32_t job = revision;
    k_condvar_broadcast(&changed);
    const int64_t deadline = heartbeat + 1000;
    while (job == revision && status.phase == M17Phase::Starting) {
        const int64_t remaining = deadline - k_uptime_get();
        if (remaining <= 0) {
            fail_locked(-ETIMEDOUT);
            break;
        }
        k_condvar_wait(&changed, &mutex, K_MSEC(remaining));
    }
    const int error = status.error ? status.error : job != revision ? -ECANCELED : 0;
    k_mutex_unlock(&mutex);
    return error;
}
} // namespace

int m17_receive(AudioSession session, const char *local_callsign, const M17Settings &settings) {
    return start(Operation::Receive, session, local_callsign, settings);
}

int m17_prepare_transmit(AudioSession session, const char *local_callsign,
                         const M17Settings &settings) {
    return start(Operation::Transmit, session, local_callsign, settings);
}

int m17_begin_transmit() {
    k_mutex_lock(&mutex, K_FOREVER);
    check_health();
    const int error = status.error                         ? status.error
                      : status.phase != M17Phase::Prepared ? -EINVAL
                                                           : 0;
    if (!error) {
        request.begin = true;
        status.phase = M17Phase::Transmitting;
        heartbeat = k_uptime_get();
        k_condvar_broadcast(&changed);
    }
    k_mutex_unlock(&mutex);
    return error;
}

int m17_finish_transmit() {
    k_mutex_lock(&mutex, K_FOREVER);
    check_health();
    const int error = status.error ? status.error
                      : status.phase == M17Phase::Transmitting || status.phase == M17Phase::Finished
                          ? 0
                          : -EINVAL;
    if (!error)
        request.finish = true;
    k_mutex_unlock(&mutex);
    return error;
}

void m17_cancel() {
    k_mutex_lock(&mutex, K_FOREVER);
    request = {};
    next_revision();
    if (!status.error)
        status = {};
    k_condvar_broadcast(&changed);
    k_mutex_unlock(&mutex);
}

M17Status m17_status() {
    k_mutex_lock(&mutex, K_FOREVER);
    check_health();
    const auto result = status;
    k_mutex_unlock(&mutex);
    return result;
}
} // namespace ht
