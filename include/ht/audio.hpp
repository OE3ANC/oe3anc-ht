// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ht {
enum class AudioInput : uint8_t { Microphone, Radio };
enum class AudioOutput : uint8_t { Speaker, Radio };
enum class AudioSource : uint8_t { Silence, Microphone, Radio, Buffer };

struct AudioRoutes {
    // Zero disables capture. Supported rates: 8, 16, 24 and 48 kHz.
    uint32_t input_rate[2] = {};
    AudioSource output[2] = {AudioSource::Silence, AudioSource::Silence};
    uint32_t output_rate[2] = {};
};

struct AudioSession {
    uint32_t id = 0;
};

struct AudioStatus {
    int error = 0;
    uint32_t fm_samples_discarded = 0;
    uint32_t silence_samples[2] = {};
};

// The controller owns start/route/cancel. DSP objects belong to one worker.
// No vendor call holds the buffer mutex, including construction and stop.
int audio_start();
int audio_route(const AudioRoutes &routes, AudioSession &session);
void audio_cancel();        // Invalidates sessions and wakes waiters without RPC.
AudioStatus audio_status(); // Latches a stalled worker as a reboot-only fault.
void audio_report_error(int error);

// Copy exactly count samples (1..1920), or return a negative errno. Buffers
// remain caller-owned and are never retained. A successful write means the
// samples were queued, not that they have reached the DAC. Each endpoint has
// one reader/writer. Cancellation returns -ECANCELED; full/empty queues wait
// for at most timeout_ms. A zero timeout returns -EAGAIN immediately.
int audio_read(AudioSession session, AudioInput input, int16_t *samples, size_t count,
               uint32_t timeout_ms);
int audio_write(AudioSession session, AudioOutput output, const int16_t *samples, size_t count,
                uint32_t timeout_ms);

// Wait for previous buffered writes, their final rate-conversion repetition,
// and the backend's playback-tail policy. C62 uses the reference FIFO/latency
// estimate, not a physical DAC acknowledgement. Call from the endpoint's
// writer context; writes/another drain to it return -EBUSY until this returns.
// Cancellation/fault wakes the caller. Timeout returns -ETIMEDOUT without
// cancelling samples or latching a transport fault; the owner decides to stop.
// Zero timeout returns -EAGAIN without starting a drain. Other endpoints keep
// running, and the same session may accept new writes after a successful drain.
int audio_drain(AudioSession session, AudioOutput output, uint32_t timeout_ms);
} // namespace ht
