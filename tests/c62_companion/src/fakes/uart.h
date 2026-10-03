/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#define CSK_UART2_FIFO 32
#define UARTC_LSR_RDR 1
#define UARTC_LSR_OE 2
#define UARTC_LSR_PE 4
#define UARTC_LSR_FE 8
#define UARTC_LSR_BI 16
#define UARTC_LSR_THRE 32
#define UARTC_LSR_TEMT 64
static uint8_t input[32], output[256];
static unsigned input_size, input_pos, output_size, status_reads;
static bool tx_ready = true;
static uint32_t line_error;

struct FakeRegister {
    unsigned kind;

    operator uint32_t() const {
        if (kind == 0) {
            ++status_reads;
            const uint32_t error = line_error;
            line_error = 0; // Line-status reads clear the latched error.
            return (input_pos < input_size ? UARTC_LSR_RDR : 0) |
                   (tx_ready ? UARTC_LSR_THRE | UARTC_LSR_TEMT : 0) | error;
        }
        return input_pos < input_size ? input[input_pos++] : 0;
    }

    void operator=(uint32_t value) {
        output[output_size++] = value;
    }
};

struct DW_UART_RegDef {
    struct {
        FakeRegister all;
    } REG_LSR{{0}}, REG_RBR{{1}}, REG_THR{{2}};
};

static DW_UART_RegDef fake_hw;
