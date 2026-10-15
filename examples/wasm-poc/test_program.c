// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>

// The scalar Thinker query must observe main's completed self-test, not a constant export.
static uint32_t result;
static volatile int16_t samples[] = {-32768, -10000, -3, 0, 3, 10000, 32767};

int main(void) {
#if HT_TEST_ID == 1
    int32_t sum = 0;
    for (unsigned i = 0; i < 7; ++i) {
        sum += samples[i];
    }
    result = sum == -1 ? 11 : 0;
#elif HT_TEST_ID == 2
    static const int16_t expected[] = {-16384, -5000, -1, 0, 1, 5000, 16383};
    result = 22;
    for (unsigned i = 0; i < 7; ++i) {
        samples[i] = (int16_t)((int32_t)samples[i] * 16384 / 32768);
        if (samples[i] != expected[i]) {
            result = 0;
        }
    }
#elif HT_TEST_ID == 3
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 7; ++i) {
        const uint16_t sample = (uint16_t)samples[i];
        hash = (hash ^ (sample & 255)) * 16777619u;
        hash = (hash ^ (sample >> 8)) * 16777619u;
    }
    result = hash == 0xad9c949du ? 33 : 0;
#else
#error Select HT_TEST_ID 1, 2 or 3
#endif
    return result ? 0 : 1;
}

int get_output_count(void) {
    return (int)result;
}
