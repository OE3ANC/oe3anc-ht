// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>

static int16_t pcm[480];

int probe(void) {
    return 42;
}

int16_t *buffer(void) {
    return pcm;
}

// One mono block, attenuated in place. Q15 unity is 32768.
// This is a proposed portable ABI, not the closed DSP service's ABI.
int process(int count, int gain_q15) {
    if (count < 0 || count > 480 || gain_q15 < 0 || gain_q15 > 32768) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        pcm[i] = (int16_t)((int32_t)pcm[i] * gain_q15 / 32768);
    }
    return count;
}

int main(void) {
    return probe() == 42 ? 0 : 1;
}
