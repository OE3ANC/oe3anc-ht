// SPDX-License-Identifier: GPL-3.0-or-later
#include "state.h"
#include <errno.h>
#include <ht/voice.hpp>
#include <zephyr/kernel.h>

namespace ht {
namespace m17 {
K_MUTEX_DEFINE(codec_lock);
static VoiceCodec *owner;
static k_spinlock statistics_lock;
static VoiceStatistics statistics;

VoiceStatistics voice_statistics() {
    const auto key = k_spin_lock(&statistics_lock);
    VoiceStatistics result = statistics;
    k_spin_unlock(&statistics_lock, key);
    result.state_bytes = ht_codec2_state_bytes();
    return result;
}

void voice_statistics_reset() {
    const auto key = k_spin_lock(&statistics_lock);
    statistics = {};
    k_spin_unlock(&statistics_lock, key);
}

static void record(VoiceTiming &timing, uint32_t start) {
    const uint32_t elapsed = k_cyc_to_us_ceil64(uint32_t(k_cycle_get_32() - start));
    const auto key = k_spin_lock(&statistics_lock);
    // Freeze a full counter rather than wrapping it into a misleading average.
    if (timing.frames != UINT32_MAX) {
        ++timing.frames;
        timing.total_us += elapsed;
        timing.maximum_us = MAX(timing.maximum_us, elapsed);
        timing.over_budget += elapsed > 20000;
    }
    k_spin_unlock(&statistics_lock, key);
}

VoiceCodec::~VoiceCodec() {
    close();
}

int VoiceCodec::open() {
    k_mutex_lock(&codec_lock, K_FOREVER);
    if (owner && owner != this) {
        k_mutex_unlock(&codec_lock);
        return -EBUSY;
    }
    const int error = ht_codec2_initialize();
    owner = error ? nullptr : this;
    if (!error) {
        const auto key = k_spin_lock(&statistics_lock);
        if (statistics.initializations != UINT32_MAX) {
            ++statistics.initializations;
        }
        k_spin_unlock(&statistics_lock, key);
    }
    k_mutex_unlock(&codec_lock);
    return error;
}

void VoiceCodec::close() {
    k_mutex_lock(&codec_lock, K_FOREVER);
    if (owner == this) {
        owner = nullptr;
    }
    k_mutex_unlock(&codec_lock);
}

int VoiceCodec::encode(const Speech &speech, Payload &output) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    output = {};
    if (owner != this) {
        k_mutex_unlock(&codec_lock);
        return -ENODEV;
    }
    for (unsigned frame = 0; frame < 2; ++frame) {
        const uint32_t start = k_cycle_get_32();
        ht_codec2_encode(output.bytes + frame * 8, speech.samples + frame * 160);
        record(statistics.encode, start);
    }
    k_mutex_unlock(&codec_lock);
    return 0;
}

int VoiceCodec::decode(const Payload &payload, Speech &output) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    output = {};
    if (owner != this) {
        k_mutex_unlock(&codec_lock);
        return -ENODEV;
    }
    for (unsigned frame = 0; frame < 2; ++frame) {
        const uint32_t start = k_cycle_get_32();
        ht_codec2_decode(output.samples + frame * 160, payload.bytes + frame * 8);
        record(statistics.decode, start);
    }
    k_mutex_unlock(&codec_lock);
    return 0;
}
} // namespace m17
} // namespace ht
