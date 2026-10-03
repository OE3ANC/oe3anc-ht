/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static inline void expect_irq_locked() {
    const unsigned int key = irq_lock();
    zassert_false(arch_irq_unlocked(key), "shared-register operation was preemptible");
    irq_unlock(key);
}

// The driver accesses this model through its real port API. Every output
// register read/write checks the critical section, rather than a test hook.
struct OutputRegister {
    uint32_t value = 0;

    operator uint32_t() const {
        expect_irq_locked();
        return value;
    }

    void operator=(uint32_t next) {
        expect_irq_locked();
        value = next;
    }

    void operator|=(uint32_t bits) {
        *this = uint32_t(*this) | bits;
    }

    void operator&=(uint32_t bits) {
        *this = uint32_t(*this) & bits;
    }

    void operator^=(uint32_t bits) {
        *this = uint32_t(*this) ^ bits;
    }
};

struct GPIO_RegDef {
    struct {
        OutputRegister all;
    } REG_DATAOUT;

    struct {
        uint32_t all;
    } REG_DATAIN, REG_INTRSTATUS, REG_INTREN;
};

struct GPIO_RESOURCES {
    GPIO_RegDef *reg;
};

static GPIO_RegDef registers[2];
static GPIO_RESOURCES resources[] = {{&registers[0]}, {&registers[1]}};

static inline void *GPIOA() {
    return &resources[0];
}

static inline void *GPIOB() {
    return &resources[1];
}
