/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpio.h"
#define CSK_IOMUX_FUNC_DEFAULT 0
#define CSK_IOMUX_FUNC_ALTER1 1
#define CSK_IOMUX_PAD_A 0
#define CSK_IOMUX_PAD_B 1
#define HAL_IOMUX_NONE_MODE 0
#define HAL_IOMUX_PULLUP_MODE 1
#define HAL_IOMUX_PULLDOWN_MODE 2

static inline void IOMuxManager_PinConfigure(uint8_t, uint8_t, uint32_t) {
    expect_irq_locked();
}

static inline void IOMuxManager_ModeConfigure(uint8_t, uint8_t, uint8_t) {
    expect_irq_locked();
}
