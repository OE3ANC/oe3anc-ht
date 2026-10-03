// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern "C" uint8_t *ht_ui_buffer();
extern "C" uint16_t *ht_ui_pixels();
extern "C" int ht_ui_start();
extern "C" int ht_ui_apply(unsigned);
extern "C" void ht_ui_tick(unsigned);

int main(int argc, char **argv) {
    if (argc != 3 || ht_ui_start()) {
        return 1;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        return 1;
    }
    const auto size = fread(ht_ui_buffer(), 1, 512, input);
    fclose(input);
    if (ht_ui_apply(size)) {
        return 2;
    }
    for (unsigned i = 0; i < 10; ++i) {
        ht_ui_tick(100);
    }
    FILE *output = fopen(argv[2], "wb");
    if (!output) {
        return 1;
    }
    const bool ok = fwrite(ht_ui_pixels(), 2, 160 * 128, output) == 160 * 128;
    return fclose(output) || !ok;
}
