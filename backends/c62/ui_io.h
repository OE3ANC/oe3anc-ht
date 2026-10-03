/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int c62_controls_init(void);
int c62_ptt_read(void); /* logical pressed=1, released=0, negative errno */
/* UART owner quiesces PTT before muxing A15; restore consumes held presses. */
int c62_ptt_enable(bool enabled);
int c62_monitor_read(void);        /* independent auxiliary side GPIO */
int c62_keys_read(uint32_t *keys); /* reference matrix bits 0..19 */
int c62_display_init(void);
/* Serialized with display init/writes; disabling does not start the display. */
int c62_ui_set_power(bool active);
/* UI-owner brightness request, 0..100. Never opens the power/output gate. */
int c62_backlight_set(uint8_t percent);
/* UI owner consumes after outputs resume; skipped flushes require full redraw. */
bool c62_display_redraw_requested(void);
/* Synchronous write of tightly packed, big-endian RGB565 pixels. */
int c62_display_write(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const void *pixels,
                      size_t size);
#ifdef __cplusplus
}
#endif
