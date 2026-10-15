/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* UART stays active alongside physical PTT. Transport calls are thread-only.
 * Read is nonblocking; errors discard buffered input and require resynchronization.
 * Write may have emitted a prefix on error. Emulator byte I/O returns -ENOTSUP.
 */
int ht_companion_read(uint8_t *data, size_t size);
/* At most 256 bytes and 1..100 ms. Serialized with initialization. */
int ht_companion_write(const uint8_t *data, size_t size, uint32_t timeout_ms);
/* Idempotently initialize UART reception; also called after DSP boot. */
int ht_companion_restore_console(void);
#ifdef __cplusplus
}
#endif
