/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int c62_rf_io_init(void);
int c62_apc(uint8_t duty_percent);
int c62_speaker(bool enabled);
int c62_receive_led(bool enabled);
#ifdef __cplusplus
}
#endif
