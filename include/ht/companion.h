/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The radio owner alone changes modes. RX remains available with mode off for
 * automatic HELLO admission. Transport calls are thread-only.
 * Read is nonblocking; negative errors discard buffered input, requiring the
 * protocol to resynchronize. Write may have emitted a prefix on error.
 * The emulator simulates mode ownership only; byte I/O returns -ENOTSUP.
 */
int ht_companion_set_enabled(bool enabled);
bool ht_companion_enabled(void);
int ht_companion_read(uint8_t *data, size_t size);
/* At most 256 bytes and 1..100 ms. Serialized with mode/audio changes. */
int ht_companion_write(const uint8_t *data, size_t size, uint32_t timeout_ms);
/* Idempotently initialize UART reception; also called after DSP boot. */
int ht_companion_restore_console(void);
#ifdef __cplusplus
}
#endif
