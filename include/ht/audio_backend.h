/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Worker-only target operations. Returning zero from poll permits idle I/O;
 * negative errors latch the transport off. set_running stops and flushes the
 * previous path before starting the requested one, even on a mode change
 * with the same enabled channels. Vendor calls may block only this worker. */
int ht_audio_backend_open(void);
int ht_audio_backend_set_running(bool capture, bool playback);
int ht_audio_backend_poll(uint32_t session);

/* Worker-only, after poll has published the final software-buffered samples.
 * Return the remaining playback-tail delay in milliseconds, or a negative
 * error. C62 estimates shared FIFO occupancy + reported latency +20 ms.
 * No application mutex is held; no pointer is retained. */
int ht_audio_backend_tail_ms(uint32_t *delay_ms);

/* A backend holds each DSP frame until the copy completes. These functions
 * never retain it. Capture accepts the SDK's channel map. Playback fills a
 * complete 48 kHz stereo frame: speaker left, radio right. Stale sessions
 * return -ECANCELED; playback also fills silence in that case. */
int ht_audio_capture(uint32_t session, const int16_t *samples, size_t frames, size_t channels,
                     size_t microphone_index, size_t radio_index);
int ht_audio_playback(uint32_t session, int16_t *samples, size_t frames);

#ifdef __cplusplus
}
#endif
