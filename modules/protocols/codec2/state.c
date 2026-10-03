// SPDX-License-Identifier: GPL-3.0-or-later
#include "state.h"
#include "codec2_mod.h"
#include <errno.h>
#include <string.h>

#ifdef CONFIG_BOARD_C62
static codec2_t state __attribute__((section(".psram_section")));
#else
static codec2_t state;
#endif
_Static_assert(CODEC2_SAMPLES_PER_FRAME == 160 && CODEC2_BYTES_PER_FRAME == 8,
               "M17 requires Codec2 3200 framing");
_Static_assert(offsetof(codec2_encoder_t, fftr_fwd_mem) % _Alignof(void *) == 0,
               "Encoder FFT storage must be pointer-aligned");
_Static_assert(offsetof(codec2_decoder_t, fftr_fwd_mem) % _Alignof(void *) == 0 &&
                   offsetof(codec2_decoder_t, fftr_inv_mem) % _Alignof(void *) == 0,
               "Decoder FFT storage must be pointer-aligned");

int ht_codec2_initialize(void) {
    // Probe before initialization: the vendor init API cannot report an
    // undersized FFT buffer and later assumes each returned pointer is valid.
    size_t required = 0;
    kiss_fftr_alloc(512, 0, NULL, &required);
    if (!required || required > FFTR_MEM_BYTES) {
        return -EPROTO;
    }
    memset(&state, 0, sizeof(state));
    codec2_init(&state);
    return state.encoder.fftr_fwd_cfg && state.decoder.fftr_fwd_cfg && state.decoder.fftr_inv_cfg
               ? 0
               : -EPROTO;
}

size_t ht_codec2_state_bytes(void) {
    return sizeof(state);
}

void ht_codec2_encode(uint8_t *bits, const int16_t *speech) {
    codec2_encode(&state.encoder, bits, speech);
}

void ht_codec2_decode(int16_t *speech, const uint8_t *bits) {
    codec2_decode(&state.decoder, speech, bits);
}
