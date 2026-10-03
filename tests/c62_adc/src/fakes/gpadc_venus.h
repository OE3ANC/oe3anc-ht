/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define GPADC_FLAG_POWERED (1U << 1)
#define CSK_ADC_ISR_COMPLETE_Pos 3

typedef struct {
    union {
        volatile uint32_t all;

        struct {
            volatile uint32_t ADC_EN : 1;
            volatile uint32_t reserved : 2;
            volatile uint32_t SOFT_TRIG : 1;
        } bit;
    } REG_ADC_CR;

    union {
        volatile uint32_t all;

        struct {
            volatile uint32_t CHANNEL_ERR_IRSR : 1;
            volatile uint32_t ADC_READY_IRSR : 1;
            volatile uint32_t EOC_ERR_IRSR : 1;
            volatile uint32_t ADC_COMPLETE_IRSR : 1;
        } bit;
    } REG_ADC_IRSR0;

    struct {
        volatile uint32_t all;
    } REG_ADC_IMR0, REG_ADC_IMR1, REG_ADC_ICR0, REG_ADC_ICR1;

    struct {
        volatile uint32_t all;
    } REG_ADC_FIFO_DATA_CNT0;
} GPADC_RegDef;

typedef struct {
    uint32_t flags;
} GPADC_INFO;

typedef struct {
    GPADC_RegDef *reg;
    GPADC_INFO *info;
} GPADC_RESOURCES;
