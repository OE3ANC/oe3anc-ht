// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// The C bridge keeps vendor C-only declarations out of application headers.
// Every state operation requires the VoiceCodec owner and codec_lock.
int ht_codec2_initialize(void);
size_t ht_codec2_state_bytes(void);
void ht_codec2_encode(uint8_t *bits, const int16_t *speech);
void ht_codec2_decode(int16_t *speech, const uint8_t *bits);
#ifdef __cplusplus
}
#endif
