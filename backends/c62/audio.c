/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 * DSP setup, cache handoff, channels and timing adapted from audio_stream_c62.c.
 * Application buffer ownership and routing are implemented in modules/audio.
 */
#include <AudioRecord.h>
#include <AudioSystem.h>
#include <AudioTrack.h>
#include <csk6_cm33/include/cache.h>
#include <csk6_cm33/include/venus_ap.h>
#include <errno.h>
#include <ht/audio_backend.h>
#ifdef CONFIG_HT_COMPANION
#include <ht/companion.h>
#endif
#include <ic_fence.h>
#include <ic_proxy.h>
#include <lsf.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define RATE 48000
#define CAPTURE_SAMPLES 480
#define MAX_FRAME_SAMPLES 1920
#define IO_TIMEOUT_MS 1000

void ht_c62_dsp_boot(void);
static AudioRecord record;
static AudioTrack playback;
static bool opened;
static bool capture_running;
static bool playback_running;
static int64_t last_capture;
static int64_t last_playback;
static int64_t playback_deadline;
static uint32_t playback_fraction;
PINCTRL_DT_DEFINE(DT_CHOSEN(zephyr_console));

static int restore_console(void) {
#ifdef CONFIG_HT_COMPANION
    return ht_companion_restore_console();
#else
    const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(console)) {
        return -ENODEV;
    }
    const struct uart_config config = {
        .baudrate = DT_PROP(DT_CHOSEN(zephyr_console), current_speed),
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    const int error = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(zephyr_console)),
                                          PINCTRL_STATE_DEFAULT);
    return error ? error : uart_configure(console, &config);
#endif
}

static int parameters(const char *text) {
    // The SDK constructor silently substitutes its global empty string when
    // allocation fails. Construct the same owned payload with a checked allocation.
    const size_t bytes = strlen(text) + 1;
    SharedBuffer *buffer = SharedBuffer_alloc(bytes);
    if (!buffer) {
        return -ENOMEM;
    }
    String8 value = {.mString = SharedBuffer_data_mut(buffer)};
    memcpy(value.mString, text, bytes);
    const int error = AudioSystem_setParameters(0, &value);
    String8_dtor(&value);
    return error;
}

static int refresh_layout(ICStream *stream) {
    if (!stream || !stream->share) {
        return -EIO;
    }
    const uintptr_t address = (uintptr_t)stream->share;
    dcache_invalidate_range(address, address + IC_DCACHELINE_ROUNDUP_SIZE(sizeof(*stream->share)));
    __DSB();
    const struct ICStreamShare *share = stream->share;
    if (share->channelCount != 2 || share->cellSize != sizeof(int16_t) ||
        share->sampleCountPerFrame <= 0 || share->sampleCountPerFrame > MAX_FRAME_SAMPLES ||
        share->frameCount_InternalBuf <= 0 || share->frameCount_InternalBuf > 128) {
        return -ENOTSUP;
    }
    return ICStream_reconfig(stream) == IC_OK ? 0 : -EIO;
}

int ht_audio_backend_open(void) {
    ht_c62_dsp_boot();
    int error = lsf_init();
    if (!error) {
        error = lsf_connect();
    }
    if (error) {
        return -EIO;
    }
    ICFenceHandle fence = IC_Proxy_getRemoteFence(0);
    if (!fence || ICFence_syncWithRemote(fence) != IC_OK) {
        return -EIO;
    }
    uint32_t ready_message;
    // The SDK fence/RPC calls have unbounded waits. Only this worker uses them;
    // the application waits at most three seconds and latches a fault on timeout.
    if (ICFence_wait(fence, &ready_message) != IC_OK) {
        return -EIO;
    }
    error = AudioSystem_initialize();
    if (!error) {
        error = parameters("ADC_PDM_GAIN_A_LEFT=6;ADC_PDM_GAIN_D_LEFT=20;"
                           "ADC_PDM_GAIN_A_RIGHT=6;ADC_PDM_GAIN_D_RIGHT=20");
    }
    if (!error) {
        char config[64];
        snprintf(config, sizeof(config), "samplingRate=%u;channels=%u", RATE,
                 (unsigned)(CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT));
        error = parameters(config);
    }
    if (!error) {
        error = AudioRecord_ctor(&record, 0, RATE, PCM_16_BIT, CHANNEL_IN_LEFT | CHANNEL_IN_RIGHT,
                                 CAPTURE_SAMPLES, NULL);
    }
    if (!error) {
        error = refresh_layout(record.mICStream);
    }
    if (!error && (!record.mCblk ||
                   record.mCblk->frameCount != record.mICStream->share->sampleCountPerFrame ||
                   record.mCblk->channels != 2 || record.mChannelOutCnt != 2 ||
                   record.mChannelOutIdx[0] >= 2 || record.mChannelOutIdx[1] >= 2 ||
                   record.mChannelOutIdx[0] == record.mChannelOutIdx[1])) {
        error = -ENOTSUP;
    }
    if (!error) {
        error = AudioTrack_ctor(&playback, RATE, PCM_16_BIT, CHANNEL_OUT_STEREO, 0, NULL);
    }
    if (!error) {
        error = refresh_layout(playback.mICStream);
    }
    if (!error && (!playback.mCblk || playback.mCblk->channels != 2 ||
                   playback.mCblk->frameSize != 2 * sizeof(int16_t))) {
        error = -ENOTSUP;
    }
    const int console_error = restore_console();
    if (!error) {
        error = console_error;
    }
    opened = error == 0;
    // On failure keep SDK objects alive for reboot. Destructors can issue RPCs
    // against a failed DSP and must not delay propagation of the original error.
    return error < 0 ? error : error ? -EIO : 0;
}

static int bypass_adc_hpf2(void) {
    // Reference AON ADC01, not CP ADC23. HPF1 (bit 15) remains unchanged.
    const uintptr_t adc = AON_VAD_BASE + 0x20000;
    if ((sys_read32(adc + 0x14) & (BIT(7) | BIT(8))) != (BIT(7) | BIT(8))) {
        return -EIO;
    }
    const uint32_t before = sys_read32(adc + 0x18);
    sys_write32(before & ~BIT(14), adc + 0x18);
    __DSB();
    return sys_read32(adc + 0x18) == (before & ~BIT(14)) ? 0 : -EIO;
}

static int discard_capture(void) {
    ICStream *stream = record.mICStream;
    const struct ICStreamShare *share = stream->share;
    if (share->fifoUnitCountPerFrame <= 0 || share->fifo.size <= 0) {
        return -EIO;
    }
    const int capacity = share->fifo.size / share->fifoUnitCountPerFrame;
    if (capacity <= 0 || capacity > 128) {
        return -ENOTSUP;
    }
    // Capture is stopped before draining; bound the work even if the DSP
    // violates that handshake. Old samples must not enter a new mode/session.
    for (int i = 0; i <= capacity; ++i) {
        if (ICStream_Consumer_fetchRemote(stream) != IC_OK) {
            return -EIO;
        }
        if (ICStream_Consumer_isEmpty(stream)) {
            return 0;
        }
        if (i == capacity) {
            return -EOVERFLOW;
        }
        void *frame;
        if (ICStream_Consumer_acquireFrame(stream, &frame) != IC_OK ||
            ICStream_Consumer_releaseFrame(stream, frame) != IC_OK ||
            ICStream_Consumer_commitRemote(stream) != IC_OK) {
            return -EIO;
        }
    }
    return -EIO;
}

int ht_audio_backend_set_running(bool capture, bool play) {
    if (!opened) {
        return capture || play ? -ENODEV : 0;
    }
    // No application mutex is held. Stop/flush removes the old route's DSP
    // playback tail before acknowledging a new session to the controller.
    if (capture_running) {
        AudioRecord_stop(&record);
        capture_running = false;
    }
    if (playback_running) {
        IAudioTrack_stop(playback.mAudioTrack);
        playback_running = false;
    }
    IAudioTrack_flush(playback.mAudioTrack);
    int error = discard_capture();
    if (!error && capture) {
        error = IAudioRecord_start(record.mAudioRecord);
        capture_running = error == 0;
        if (!error) {
            error = bypass_adc_hpf2();
        }
        last_capture = k_uptime_get();
    }
    if (!error && play) {
        // Direct checked RPC + ICStream frames: no SDK implicit auto-restart,
        // unbounded AudioTrack_write, or ignored start() result.
        error = IAudioTrack_start(playback.mAudioTrack);
        playback_running = error == 0;
        playback_deadline = last_playback = k_uptime_get();
        playback_fraction = 0;
    }
    const int console_error = restore_console();
    return error < 0 ? error : error ? -EIO : console_error;
}

static int capture_frame(uint32_t session) {
    ICStream *stream = record.mICStream;
    if (ICStream_Consumer_fetchRemote(stream) != IC_OK) {
        return -EIO;
    }
    if (ICStream_Consumer_isEmpty(stream)) {
        return k_uptime_get() - last_capture > IO_TIMEOUT_MS ? -ETIMEDOUT : 0;
    }
    void *frame;
    if (ICStream_Consumer_acquireFrame(stream, &frame) != IC_OK) {
        return -EIO;
    }
    const int error = ht_audio_capture(session, frame, stream->share->sampleCountPerFrame,
                                       stream->share->channelCount, record.mChannelOutIdx[0],
                                       record.mChannelOutIdx[1]);
    const int release = ICStream_Consumer_releaseFrame(stream, frame);
    const int commit = ICStream_Consumer_commitRemote(stream);
    last_capture = k_uptime_get();
    return error ? error : release != IC_OK || commit != IC_OK ? -EIO : 0;
}

static int playback_frame(uint32_t session) {
    if (k_uptime_get() < playback_deadline) {
        return 0;
    }
    ICStream *stream = playback.mICStream;
    if (ICStream_Producer_fetchRemote(stream) != IC_OK) {
        return -EIO;
    }
    if (ICStream_Producer_isFull(stream)) {
        return k_uptime_get() - last_playback > IO_TIMEOUT_MS ? -ETIMEDOUT : 0;
    }
    void *frame;
    if (ICStream_Producer_acquireFrame(stream, &frame) != IC_OK) {
        return -EIO;
    }
    const size_t count = stream->share->sampleCountPerFrame;
    const int error = ht_audio_playback(session, frame, count);
    const int release = ICStream_Producer_releaseFrame(stream, frame);
    const int commit = ICStream_Producer_commitRemote(stream);
    last_playback = k_uptime_get();
    // Carry fractional milliseconds, preserving reference pacing without bursts.
    const uint64_t duration = count * 1000ULL + playback_fraction;
    const int64_t period = duration / RATE;
    playback_fraction = duration % RATE;
    playback_deadline += period;
    if (playback_deadline + period <= last_playback) {
        playback_deadline = last_playback + MAX(1, period);
    }
    return error ? error : release != IC_OK || commit != IC_OK ? -EIO : 0;
}

int ht_audio_backend_poll(uint32_t session) {
    int error = capture_running ? capture_frame(session) : 0;
    if (!error && playback_running) {
        error = playback_frame(session);
    }
    return error;
}

int ht_audio_backend_tail_ms(uint32_t *delay_ms) {
    if (!delay_ms)
        return -EINVAL;
    *delay_ms = 0;
    if (!opened || !playback_running)
        return -ENODEV;
    ICStream *stream = playback.mICStream;
    if (ICStream_Producer_fetchRemote(stream) != IC_OK)
        return -EIO;
    const struct ICStreamShare *share = stream->share;
    const ICStreamFifo *fifo = &share->fifo;
    // The pinned FIFO's units are bytes, including both 16-bit channels.
    const unsigned sample_bytes = 2 * sizeof(int16_t);
    if (share->fifoUnitCountPerSample != sample_bytes || fifo->size <= 0 ||
        fifo->size > 128 * MAX_FRAME_SAMPLES * sample_bytes || fifo->size % sample_bytes) {
        return -EIO;
    }
    // FIFO_NO_DS indexes span twice the byte capacity. Snapshot and validate
    // them before subtracting so malformed shared metadata cannot overflow.
    const int head = fifo->head_index.r;
    const int tail_index = fifo->tail_index.l;
    if (fifo->index_size != 2 * fifo->size || head < 0 || head >= fifo->index_size ||
        tail_index < 0 || tail_index >= fifo->index_size)
        return -EIO;
    int used = tail_index - head;
    if (used < 0)
        used += fifo->index_size;
    if (used > fifo->size || used % sample_bytes)
        return -EIO;
    const uint64_t frames = used / sample_bytes;
    // Preserve the reference's drain estimate; continuing silent frames do
    // not move the deadline. Physical DSP/DAC latency remains a bench check.
    const uint64_t tail = DIV_ROUND_UP(frames * 1000ULL, RATE) + playback.mLatency + 20;
    if (tail > UINT32_MAX)
        return -EOVERFLOW;
    *delay_ms = (uint32_t)tail;
    return 0;
}
