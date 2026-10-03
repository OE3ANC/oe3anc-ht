/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The radio owner alone changes modes. Transport calls are thread-only.
 * Read is nonblocking; negative errors discard buffered input, requiring the
 * future protocol to resynchronize. Write may have emitted a prefix on error.
 * The emulator simulates mode ownership only; byte I/O returns -ENOTSUP.
 */
int ht_companion_set_enabled(bool enabled);
bool ht_companion_enabled(void);
int ht_companion_read(uint8_t *data, size_t size);
/* At most 256 bytes and 1..100 ms. Serialized with mode/audio changes. */
int ht_companion_write(const uint8_t *data, size_t size, uint32_t timeout_ms);
/* Audio worker restores normal console state under the same ownership lock. */
int ht_companion_restore_console(void);
#ifdef __cplusplus
}
#endif
