// SPDX-License-Identifier: GPL-3.0-or-later
// ADC input uses unmodified pinned OpenRTX TX RRC/FIR and independent wire vectors.
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
    puts("// SPDX-License-Identifier: GPL-3.0-or-later\n"
         "// Generated from OpenRTX 9d800e69f5f3c7275857c232c32786e6526ead80 TX RRC/FIR.\n"
         "#pragma once\n#include <stdint.h>\n"
         "static constexpr int16_t reference_adc[4800] = {");
    const int8_t symbols[] = {1, 3, -1, -3};
    for (const auto &frame : frames) {
        for (unsigned i = 0; i < 1920; ++i) {
            float input = 0;
            if (i % 10 == 0) {
                const unsigned symbol = i / 10;
                const unsigned dibit = (frame[symbol / 4] >> (6 - 2 * (symbol % 4))) & 3;
                input = symbols[dibit] * 23000.0f;
            }
            const int16_t pcm = static_cast<int16_t>(filter(input));
            // The C62 capture path selects every second 48 kHz sample. Gain
            // reduction keeps this synthetic discriminator within ADC range.
            if (i % 2 == 0)
                printf("%d,\n", pcm / 4);
        }
    }
    puts("};");
}
