// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

static inline uint16_t sys_get_le16(const uint8_t *p) {
    return p[0] | uint16_t(p[1]) << 8;
}

static inline uint32_t sys_get_le32(const uint8_t *p) {
    return sys_get_le16(p) | uint32_t(sys_get_le16(p + 2)) << 16;
}

static inline void sys_put_le16(uint16_t v, uint8_t *p) {
    p[0] = v;
    p[1] = v >> 8;
}

static inline void sys_put_le32(uint32_t v, uint8_t *p) {
    sys_put_le16(v, p);
    sys_put_le16(v >> 16, p + 2);
}
