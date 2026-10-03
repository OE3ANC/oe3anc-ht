// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/power_backend.hpp>
#include "bk4819.h"
#include "rf_io.h"
#include <errno.h>
#include <zephyr/ztest.h>
static int rf_error, bus_error, pa_error;
static unsigned order, rf_calls, bus_calls, writes;
extern "C" void power_io_test_reset();
extern "C" unsigned power_io_test_off_calls();

extern "C" int c62_rf_io_init() {
    ++rf_calls;
    zassert_equal(++order, 1);
    return rf_error;
}

extern "C" int bk4819_bus_init() {
    ++bus_calls;
    zassert_equal(++order, 2);
    return bus_error;
}

extern "C" int bk4819_write(uint8_t address, uint16_t value) {
    zassert_equal(value, 0);
    ++writes;
    zassert_equal(++order, writes + 2);
    zassert_equal(address, writes == 1 ? 0x33 : 0x30);
    return writes == 1 ? pa_error : 0;
}

static void before(void *) {
    power_io_test_reset();
    order = rf_calls = bus_calls = writes = 0;
    rf_error = bus_error = pa_error = 0;
}

ZTEST(c62_power_backend, test_prepare_uses_only_bus_and_inactive_registers) {
    zassert_ok(ht::power_backend_prepare());
    zassert_equal(rf_calls, 1);
    zassert_equal(bus_calls, 1);
    zassert_equal(writes, 2);
    zassert_equal(power_io_test_off_calls(), 1);
    // No full RF or audio/DSP initialization symbols linked in this fixture.
}

ZTEST(c62_power_backend, test_prepare_failure_still_attempts_independent_outputs) {
    rf_error = -EIO;
    pa_error = -EPIPE;
    zassert_equal(ht::power_backend_prepare(), -EIO);
    zassert_equal(writes, 2);
    zassert_equal(power_io_test_off_calls(), 1);
    before(nullptr);
    bus_error = -ETIMEDOUT;
    zassert_equal(ht::power_backend_prepare(), -ETIMEDOUT);
    zassert_equal(writes, 0);
    zassert_equal(power_io_test_off_calls(), 1);
}

ZTEST_SUITE(c62_power_backend, nullptr, nullptr, before, nullptr, nullptr);
