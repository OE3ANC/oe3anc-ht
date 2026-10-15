/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define CSK_IOMUX_PAD_A 0
#define CSK_IOMUX_FUNC_DEFAULT 0U
int32_t IOMuxManager_GetConfig(uint8_t pad, uint8_t pin, uint32_t *function);
int32_t IOMuxManager_PinConfigure(uint8_t pad, uint8_t pin, uint32_t function);
