/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gpio.h"
#define CSK_GPIO_DIR_OUTPUT 1
#define CSK_GPIO_DIR_INPUT 0
#define CSK_GPIO_INTR_ENABLE 1
#define CSK_GPIO_INTR_DISABLE 0
#define CSK_GPIO_SET_INTR_NEGATIVE_EDGE 2
#define CSK_GPIO_SET_INTR_POSITIVE_EDGE 4
#define CSK_GPIO_SET_INTR_DUAL_EDGE 6
#define CSK_GPIO_SET_INTR_LOW_LEVEL 8
#define CSK_GPIO_SET_INTR_HIGH_LEVEL 16

static inline void GPIO_SetDir(void *, uint32_t, uint32_t) {
    expect_irq_locked();
}

static inline void GPIO_PinWrite(void *context, uint32_t mask, uint32_t value) {
    GPIO_RegDef *reg = static_cast<GPIO_RESOURCES *>(context)->reg;
    if (value) {
        reg->REG_DATAOUT.all |= mask;
    } else {
        reg->REG_DATAOUT.all &= ~mask;
    }
}

static inline void GPIO_Control(void *, uint32_t, uint32_t) {
    expect_irq_locked();
}
