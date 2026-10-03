/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

/* SRAM code/data aliases differ by bit 29. DSP owns bank 4. */
#define RAM_START (CONFIG_SRAM_BASE_ADDRESS & ~0x20000000UL)
#define RAM_END (RAM_START + CONFIG_SRAM_SIZE * 1024UL)
BUILD_ASSERT(RAM_END <= 0xc0000 || RAM_START >= 0xd0000,
             "Application SRAM overlaps DSP shared audio bank 4");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(slot0_partition)) +
                     DT_REG_SIZE(DT_NODELABEL(slot0_partition)) <=
                 DT_REG_ADDR(DT_NODELABEL(dsp_firmware)),
             "Application flash overlaps DSP firmware");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(dsp_firmware)) + DT_REG_SIZE(DT_NODELABEL(dsp_firmware)) <=
                 DT_REG_ADDR(DT_NODELABEL(storage_partition)),
             "DSP firmware overlaps settings storage");
BUILD_ASSERT(DT_SAME_NODE(DT_CHOSEN(zephyr_settings_partition), DT_NODELABEL(storage_partition)),
             "Settings must use the reserved storage partition");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(storage_partition)) +
                     DT_REG_SIZE(DT_NODELABEL(storage_partition)) <=
                 DT_REG_SIZE(DT_NODELABEL(flash0)),
             "Settings storage exceeds physical flash");
