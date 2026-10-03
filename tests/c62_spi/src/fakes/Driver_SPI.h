/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define CSK_SPI_MODE_MASTER 1
#define CSK_SPI_MODE_SLAVE 2
#define CSK_SPI_CPOL0_CPHA0 0
#define CSK_SPI_CPOL0_CPHA1 4
#define CSK_SPI_CPOL1_CPHA0 8
#define CSK_SPI_CPOL1_CPHA1 12
#define CSK_SPI_DATA_BITS(n) ((n) << 8)
#define CSK_SPI_LSB_MSB 16
#define CSK_SPI_MSB_LSB 0
#define CSK_SPI_TXIO_DMA 32
#define CSK_SPI_RXIO_DMA 64
#define CSK_SPI_ABORT_TRANSFER 0x80000000U
#define CSK_SPI_EVENT_TRANSFER_COMPLETE 1
#define CSK_POWER_FULL 1
int32_t SPI_Control(void *, uint32_t, uint32_t);
int32_t SPI_Send(void *, const void *, uint32_t);
int32_t SPI_Receive(void *, void *, uint32_t);
int32_t SPI_Transfer(void *, const void *, void *, uint32_t);

static inline int32_t SPI_Initialize(void *context, void (*callback)(uint32_t, uint32_t),
                                     uint32_t arg) {
    (void)context;
    (void)callback;
    (void)arg;
    return 0;
}

static inline int32_t SPI_PowerControl(void *context, uint32_t state) {
    (void)context;
    (void)state;
    return 0;
}
