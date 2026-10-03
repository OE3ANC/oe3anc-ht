// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the actual driver against checked MMIO/HAL models. Native devicetree
// has no listenai GPIO nodes, so the driver's device-registration macro is empty.
#include "../../../drivers/gpio/gpio_csk6.c"

ZTEST(c62_gpio, test_atomic_port_updates_preserve_other_pins) {
    for (unsigned port = 0; port < 2; ++port) {
        csk_gpio_config config{};
        config.driver_num = port;
        struct device device{};
        device.config = &config;
        auto &value = registers[port].REG_DATAOUT.all.value;
        value = 0x55aa0000;
        zassert_ok(csk_gpio_port_set_bits_raw(&device, 0x12));
        zassert_equal(value, 0x55aa0012);
        zassert_ok(csk_gpio_port_clear_bits_raw(&device, 0x10));
        zassert_equal(value, 0x55aa0002);
        zassert_ok(csk_gpio_port_toggle_bits(&device, 0x23));
        zassert_equal(value, 0x55aa0021);
        zassert_ok(csk_gpio_port_set_masked_raw(&device, 0xff, 0xdeadbeef));
        zassert_equal(value, 0x55aa00ef);
        zassert_ok(csk_gpio_configure(&device, 2, GPIO_OUTPUT_HIGH));
        zassert_equal(value, 0x55aa00ef);
        zassert_ok(csk_gpio_configure(&device, 2, GPIO_OUTPUT_LOW));
        zassert_equal(value, 0x55aa00eb);
        zassert_ok(csk_gpio_configure(&device, 4, GPIO_INPUT | GPIO_PULL_UP));
        zassert_equal(csk_gpio_configure(&device, 4, GPIO_INPUT | GPIO_OUTPUT), -ENOTSUP);
        const unsigned int key = irq_lock();
        zassert_true(arch_irq_unlocked(key), "driver leaked its IRQ lock");
        irq_unlock(key);
    }
}

ZTEST_SUITE(c62_gpio, nullptr, nullptr, nullptr, nullptr, nullptr);
