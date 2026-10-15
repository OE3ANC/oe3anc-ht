/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <ht/companion.h>
#include <zephyr/sys/util.h>
int ht_companion_read(uint8_t *data, size_t size) {
    ARG_UNUSED(data);
    ARG_UNUSED(size);
    return -ENOTSUP;
}

int ht_companion_write(const uint8_t *data, size_t size, uint32_t timeout_ms) {
    ARG_UNUSED(data);
    ARG_UNUSED(size);
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
}

int ht_companion_restore_console(void) {
    return 0;
}
