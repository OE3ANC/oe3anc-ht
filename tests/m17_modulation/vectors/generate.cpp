// SPDX-License-Identifier: GPL-3.0-or-later
// Run with pinned OpenRTX RRC/FIR headers, not the implementation under test.
#include "../../m17/vectors/golden.hpp"
#include "protocols/M17/DSP.hpp"
#include <cstdio>
#include <cstring>

int main() {
    Fir<81> filter(M17::rrc_taps_48k);
    uint8_t frames[5][48];
    memset(frames[0], 0x77, 48);
    memset(frames[1], 0x77, 48);
    memcpy(frames[2], lsf_frame, 48);
    memcpy(frames[3], stream_0, 48);
    for (unsigned i = 0; i < 48; i += 2) {
        frames[4][i] = 0x55;
        frames[4][i + 1] = 0x5d;
    }
    const unsigned probes[] = {0,  1,  2,  3,  5,   9,    10,   19,   20,
                               39, 40, 79, 80, 960, 1000, 1880, 1900, 1919};
    int16_t samples[5][1920];
    uint32_t hashes[5];
    const int8_t symbols[] = {1, 3, -1, -3};
    for (unsigned frame = 0; frame < 5; ++frame) {
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < 1920; ++i) {
            float input = 0.0f;
            if (i % 10 == 0) {
                unsigned symbol = i / 10;
                unsigned dibit = (frames[frame][symbol / 4] >> (6 - 2 * (symbol % 4))) & 3;
                input = symbols[dibit];
            }
            samples[frame][i] = static_cast<int16_t>(filter(input * 23000.0f));
            uint16_t sample = samples[frame][i];
            hash = (hash ^ uint8_t(sample)) * 16777619u;
            hash = (hash ^ uint8_t(sample >> 8)) * 16777619u;
        }
        hashes[frame] = hash;
    }
    puts("// SPDX-License-Identifier: GPL-3.0-or-later\n"
         "// Generated from OpenRTX 9d800e69f5f3c7275857c232c32786e6526ead80 RRC/FIR.\n"
         "#pragma once\n#include <stdint.h>");
    printf("static constexpr uint32_t waveform_hashes[5] = {");
    for (unsigned frame = 0; frame < 5; ++frame)
        printf("0x%08x%s", hashes[frame], frame == 4 ? "};\n" : ", ");
    printf("static constexpr unsigned probe_positions[18] = {");
    for (unsigned i = 0; i < 18; ++i)
        printf("%u%s", probes[i], i == 17 ? "};\n" : ", ");
    puts("static constexpr int16_t waveform_probes[5][18] = {");
    for (unsigned frame = 0; frame < 5; ++frame) {
        printf("    {");
        for (unsigned i = 0; i < 18; ++i)
            printf("%d%s", samples[frame][probes[i]], i == 17 ? "" : ", ");
        printf("}%s\n", frame == 4 ? "};" : ",");
    }
    uint16_t phase_signs[10] = {};
    int16_t phase_peaks[10] = {};
    for (unsigned phase = 0; phase < 10; ++phase) {
        Fir<81> peak_filter(M17::rrc_taps_48k);
        for (unsigned k = 0; phase + 10 * k < 81; ++k)
            if (M17::rrc_taps_48k[phase + 10 * k] >= 0)
                phase_signs[phase] |= 1u << k;
        for (unsigned sample = 0; sample <= 80 + phase; ++sample) {
            float input = 0;
            if (sample % 10 == 0) {
                const unsigned k = 8 - sample / 10;
                input = phase_signs[phase] & (1u << k) ? 69000.0f : -69000.0f;
            }
            phase_peaks[phase] = static_cast<int16_t>(peak_filter(input));
        }
    }
    printf("static constexpr uint16_t phase_signs[10] = {");
    for (unsigned i = 0; i < 10; ++i)
        printf("%u%s", phase_signs[i], i == 9 ? "};\n" : ", ");
    printf("static constexpr int16_t phase_peaks[10] = {");
    for (unsigned i = 0; i < 10; ++i)
        printf("%d%s", phase_peaks[i], i == 9 ? "};\n" : ", ");
    return 0;
}
