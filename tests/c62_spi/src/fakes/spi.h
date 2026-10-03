/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define SPI1_BASE 1
#define MAX_TRANCNT 512

typedef struct {
    void (*irq_handler)(void);
} SPI_DEV;

static inline void *SPI1(void) {
    return 0;
}
