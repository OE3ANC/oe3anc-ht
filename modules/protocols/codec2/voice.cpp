// SPDX-License-Identifier: GPL-3.0-or-later
#include "allocator.h"
#include <errno.h>
#include <ht/voice.hpp>
#include <zephyr/kernel.h>
extern "C" {
#include "codec2.h"
}

namespace ht {
namespace m17 {
// Pinned Codec2 has a global synthesis PRNG. Serialize library calls across
// instances as well as protecting the dedicated heap and its initialization.
K_MUTEX_DEFINE(codec_lock);

int voice_heap_usage(VoiceHeapUsage &output) {
    output = {};
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
    if (k_mutex_lock(&codec_lock, K_NO_WAIT))
        return -EAGAIN;
    sys_memory_stats stats{};
    const int error = ht_codec2_heap_stats(&stats);
    k_mutex_unlock(&codec_lock);
    if (!error) {
        output.used_bytes = stats.allocated_bytes;
        output.peak_bytes = stats.max_allocated_bytes;
        output.free_bytes = stats.free_bytes;
    }
    return error;
#else
    return -ENOTSUP;
#endif
}

VoiceCodec::~VoiceCodec() {
    close();
}

int VoiceCodec::open() {
    k_mutex_lock(&codec_lock, K_FOREVER);
    if (state_)
        codec2_destroy(static_cast<CODEC2 *>(state_));
    ht_codec2_heap_init();
    state_ = codec2_create(CODEC2_MODE_3200);
    int error = state_ ? 0 : -ENOMEM;
    if (state_ && (codec2_samples_per_frame(static_cast<CODEC2 *>(state_)) != 160 ||
                   codec2_bits_per_frame(static_cast<CODEC2 *>(state_)) != 64)) {
        codec2_destroy(static_cast<CODEC2 *>(state_));
        state_ = nullptr;
        error = -EPROTO;
    }
    k_mutex_unlock(&codec_lock);
    return error;
}

void VoiceCodec::close() {
    k_mutex_lock(&codec_lock, K_FOREVER);
    if (state_) {
        codec2_destroy(static_cast<CODEC2 *>(state_));
        state_ = nullptr;
    }
    k_mutex_unlock(&codec_lock);
}

int VoiceCodec::encode(const Speech &speech, Payload &output) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    output = {};
    if (!state_) {
        k_mutex_unlock(&codec_lock);
        return -ENODEV;
    }
    // The vendor API takes mutable short*. Copy each block to preserve the
    // public const-input contract without casting away const or aliasing.
    short block[160];
    for (unsigned frame = 0; frame < 2; ++frame) {
        for (unsigned i = 0; i < 160; ++i)
            block[i] = speech.samples[frame * 160 + i];
        codec2_encode(static_cast<CODEC2 *>(state_), output.bytes + frame * 8, block);
    }
    k_mutex_unlock(&codec_lock);
    return 0;
}

int VoiceCodec::decode(const Payload &payload, Speech &output) {
    k_mutex_lock(&codec_lock, K_FOREVER);
    output = {};
    if (!state_) {
        k_mutex_unlock(&codec_lock);
        return -ENODEV;
    }
    short block[160];
    for (unsigned frame = 0; frame < 2; ++frame) {
        codec2_decode(static_cast<CODEC2 *>(state_), block, payload.bytes + frame * 8);
        for (unsigned i = 0; i < 160; ++i)
            output.samples[frame * 160 + i] = block[i];
    }
    k_mutex_unlock(&codec_lock);
    return 0;
}
} // namespace m17
} // namespace ht
