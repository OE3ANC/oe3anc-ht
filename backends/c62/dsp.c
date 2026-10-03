/*
 * Copyright(c) 2023 LISTENAI
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from the reference C62 DSP loader; called by the audio worker.
 */
#include <csk6_cm33/include/ClockManager.h>
#include <csk6_cm33/include/SysManager.h>
#include <csk6_cm33/include/cache.h>
#include <csk6_cm33/include/venus_ap.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

#define SHARED_BANK 4
#define SHARED_SIZE 0x10000
#define SHARED_ADDRESS (SYS_RAM_BASE + SHARED_SIZE * SHARED_BANK)
#define IMAGE DT_CHOSEN(lsf_dsp_firmware)
#define AP_BOOT_ADDRESS 0x30000000
#define CP_BOOT_ADDRESS 0x60000000

BUILD_ASSERT(DT_REG_SIZE(IMAGE) <= DT_REG_SIZE(DT_NODELABEL(psram_cp)),
             "DSP firmware exceeds its reserved PSRAM arena");
BUILD_ASSERT((DT_REG_ADDR(DT_NODELABEL(psram_ap)) & ~0x20000000UL) >=
                 (AP_BOOT_ADDRESS & ~0x20000000UL) + DT_REG_SIZE(IMAGE),
             "DSP firmware copy overlaps application PSRAM");

void ht_c62_dsp_boot(void) {
#if !defined(CONFIG_UART_INTERRUPT_DRIVEN) && !defined(CONFIG_UART_ASYNC_API)
    /* DSP logging asserts AP UART2 IRQ 10; a polling console has no handler. */
    irq_disable(DT_IRQN(DT_NODELABEL(uart2)));
#endif
    __HAL_CRM_CP_CLK_DISABLE();
    __HAL_CRM_NPU_CLK_DISABLE();
    __HAL_SYS_CP_STALL();
    IP_SYSCTRL->REG_AP_CTRL1.bit.AP_RAM_SEL = 1 << SHARED_BANK;
    memset((void *)SHARED_ADDRESS, 0, SHARED_SIZE);
    dcache_invalidate_range(SHARED_ADDRESS, SHARED_ADDRESS + SHARED_SIZE);

    __HAL_CRM_CP_CLK_ENABLE();
    __HAL_SYS_CP_SET_BOOT_ADDR(CP_BOOT_ADDRESS);
    __HAL_SYS_CP_STALL();
    if (__HAL_SYS_IS_CP_RESET()) {
        __HAL_SYS_CP_RELEASE();
    }
    if (__HAL_SYS_NPU_IS_RESET()) {
        __HAL_SYS_NPU_RELEASE();
    }
    __DMB();
    const uintptr_t flash = DT_REG_ADDR(DT_CHOSEN(zephyr_flash)) + DT_REG_ADDR(IMAGE);
    memcpy((void *)AP_BOOT_ADDRESS, (const void *)flash, DT_REG_SIZE(IMAGE));
    dcache_clean_range(AP_BOOT_ADDRESS, AP_BOOT_ADDRESS + DT_REG_SIZE(IMAGE));
    __DSB();
    __HAL_SYS_CP_RESET();
    __HAL_SYS_CP_RELEASE();
    __HAL_SYS_CP_RUN();
}
