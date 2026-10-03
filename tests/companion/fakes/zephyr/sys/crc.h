// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline uint32_t crc32_ieee_update(uint32_t previous, const uint8_t *bytes, size_t length) {
    uint32_t crc = ~previous;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline uint32_t crc32_ieee(const uint8_t *bytes, size_t length) {
    return crc32_ieee_update(0, bytes, length);
}
