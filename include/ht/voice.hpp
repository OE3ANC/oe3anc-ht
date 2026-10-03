// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/m17.hpp>

namespace ht {
namespace m17 {
struct Speech {
    int16_t samples[320] = {}; // 40 ms mono speech at 8 kHz.
};

struct VoiceTiming {
    uint64_t total_us = 0;
    uint32_t frames = 0;
    uint32_t maximum_us = 0;
    uint32_t over_budget = 0; // Elapsed processing exceeded one 20 ms codec frame.
};

struct VoiceStatistics {
    VoiceTiming encode, decode;
    size_t state_bytes = 0; // Fixed reservation, including encoder/decoder FFT buffers.
    uint32_t initializations = 0;
};

// Boot/reset-scoped elapsed timings include preemption, exclude lock waits,
// sample copies and M17 modem work. Snapshots never wait for codec processing.
VoiceStatistics voice_statistics();
void voice_statistics_reset();

// Codec2-mod 3200: two 160-sample frames form one 16-byte M17 payload.
// One processing context owns an instance and close/reset. Calls are blocking;
// use a dedicated thread with enough stack for the codec.
// One fixed state is reserved in PSRAM on C62. A second active instance returns
// -EBUSY. open() resets both predictors. There are no heap allocations.
// Caller buffers are never retained. Closed calls return -ENODEV and clear
// output. All results use zero or negative errno.
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
};
} // namespace m17
} // namespace ht
