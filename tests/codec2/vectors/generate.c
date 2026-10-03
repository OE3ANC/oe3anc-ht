// SPDX-License-Identifier: GPL-3.0-or-later
// Build with unmodified Codec2 a298f3789d7ef9e829049ebe14da8845d97ba8c1.
#include "codec2.h"
#include <stdio.h>

int main(void) {
    struct CODEC2 *codec = codec2_create(CODEC2_MODE_3200);
    if (!codec)
        return 1;
    puts("// SPDX-License-Identifier: GPL-3.0-or-later\n"
         "// Generated with unmodified Codec2 a298f3789d7ef9e829049ebe14da8845d97ba8c1.\n"
         "#pragma once\n#include <stdint.h>\n"
         "static constexpr uint8_t encoded_speech[4][16] = {");
    for (unsigned pair = 0; pair < 4; ++pair) {
        printf("    {");
        for (unsigned frame = 0; frame < 2; ++frame) {
            short speech[160];
            for (unsigned i = 0; i < 160; ++i) {
                const unsigned sample = pair * 320 + frame * 160 + i;
                const int triangle = (sample % 80 < 40) ? sample % 40 : 40 - sample % 40;
                speech[i] = (triangle - 20) * 300;
            }
            unsigned char bits[8];
            codec2_encode(codec, bits, speech);
            for (unsigned i = 0; i < 8; ++i)
                printf("0x%02x%s", bits[i], frame == 1 && i == 7 ? "" : ", ");
        }
        printf("}%s\n", pair == 3 ? "" : ",");
    }
    puts("};");
    codec2_destroy(codec);
    return 0;
}
