/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define CSK_DRIVER_OK 0
#define CSK_GPADC_SAMPLETIME_8 (3U << 8)
#define CSK_GPADC_CLKFREQ_12M (2U << 12)
#define CSK_GPADC_CHANNEL_SEL_Pos 16
#define CSK_GPADC_INPUT_MODE_Single 1U
#define CSK_GPADC_VREF_SEL_1_25V (1U << 4)
#define CSK_GPADC_DMA_DISABLE 0U
void *GPADC(void);
int32_t HAL_GPADC_Control(void *hal, uint32_t control);
uint32_t HAL_GPADC_SetTriggerNum(void *hal, uint32_t count);
uint32_t HAL_GPADC_Start(void *hal);
uint16_t HAL_GPADC_GetValue(void *hal, uint32_t channel);
