// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/m17.hpp>

namespace ht {
namespace m17 {
struct Speech {
    int16_t samples[320] = {}; // 40 ms mono speech at 8 kHz.
};

struct VoiceHeapUsage {
    size_t used_bytes = 0;
    size_t peak_bytes = 0;
    size_t free_bytes = 0;
};

// Optional resource measurement. Does not wait for a busy codec; returns
// -EAGAIN then, or -ENOTSUP without SYS_HEAP_RUNTIME_STATS. Clears output on
// failure. Sampling may initialize the already reserved, empty codec heap.
int voice_heap_usage(VoiceHeapUsage &output);

// Codec2 3200: two 160-sample frames form one 16-byte M17 payload.
// One processing context owns an instance and close/reset. Calls are blocking;
// use a dedicated thread with enough stack for the codec.
// The module's bounded 32 KiB heap supports one active codec. open() replaces
// previous state; allocation failure leaves it closed and returns -ENOMEM.
// Encode/decode allocate no heap and never retain caller buffers. Closed calls
// return -ENODEV and clear output. All results use zero or negative errno.
class VoiceCodec {
  public:
    VoiceCodec() = default;
    ~VoiceCodec();
    VoiceCodec(const VoiceCodec &) = delete;
    VoiceCodec &operator=(const VoiceCodec &) = delete;
    int open();
    void close();
    int encode(const Speech &speech, Payload &output);
    int decode(const Payload &payload, Speech &output);

  private:
    void *state_ = nullptr;
};
} // namespace m17
} // namespace ht
