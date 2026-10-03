// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/battery.hpp>
#include <ht/emulator.hpp>
#include "../../../backends/c62/battery_decode.hpp"
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static void *setup() {
    auto snapshot = ht::battery_snapshot();
    zassert_equal(snapshot.freshness, ht::BatteryFreshness::Unknown);
    ht::emulator_inject_battery({0, true, false}, -ETIMEDOUT);
    zassert_equal(ht::battery_sample(), -ETIMEDOUT);
    snapshot = ht::battery_snapshot();
    zassert_equal(snapshot.freshness, ht::BatteryFreshness::Unknown);
    zassert_equal(snapshot.error, -ETIMEDOUT);
    return nullptr;
}

ZTEST(battery, test_failed_read_does_not_create_switch_decision) {
    ht::emulator_inject_battery({7200, false, true});
    zassert_ok(ht::battery_sample());
    ht::emulator_inject_battery({0, true, false}, -ETIMEDOUT);
    zassert_equal(ht::battery_sample(), -ETIMEDOUT);
    const auto snapshot = ht::battery_snapshot();
    zassert_equal(snapshot.freshness, ht::BatteryFreshness::Stale);
    zassert_true(snapshot.reading.switch_on);
    zassert_false(snapshot.reading.charger_input);
}

ZTEST(battery, test_failed_read_preserves_last_good_and_recovers) {
    ht::emulator_inject_battery({8400, false, true});
    zassert_ok(ht::battery_sample());
    const auto good = ht::battery_snapshot();
    zassert_equal(good.freshness, ht::BatteryFreshness::Fresh);
    k_sleep(K_MSEC(10));
    ht::emulator_inject_battery({0, true, false}, -EIO);
    zassert_equal(ht::battery_sample(), -EIO);
    const auto failed = ht::battery_snapshot();
    zassert_equal(failed.freshness, ht::BatteryFreshness::Stale);
    zassert_equal(failed.reading.millivolts, 8400);
    zassert_true(failed.reading.switch_on);
    zassert_false(failed.reading.charger_input);
    zassert_equal(failed.sample_ms, good.sample_ms);
    zassert_true(failed.attempt_ms > good.attempt_ms);
    ht::emulator_inject_battery({0, true, false});
    zassert_ok(ht::battery_sample());
    const auto off = ht::battery_snapshot();
    zassert_equal(off.freshness, ht::BatteryFreshness::Fresh);
    zassert_equal(off.error, 0);
    zassert_false(off.reading.switch_on);
}

ZTEST(battery, test_snapshot_ages_without_hardware_reads) {
    ht::emulator_inject_battery({7200, false, true});
    zassert_ok(ht::battery_sample());
    ht::emulator_inject_battery({0, true, false});
    k_sleep(K_MSEC(320));
    const auto stale = ht::battery_snapshot();
    zassert_equal(stale.freshness, ht::BatteryFreshness::Stale);
    zassert_equal(stale.reading.millivolts, 7200);
    zassert_true(stale.reading.switch_on);
    zassert_ok(ht::battery_sample());
    zassert_equal(ht::battery_snapshot().freshness, ht::BatteryFreshness::Fresh);
    zassert_equal(ht::battery_snapshot().reading.millivolts, 0);
}

ZTEST(battery, test_emulator_inputs_are_independent_and_capabilities_explicit) {
    const auto caps = ht::battery_capabilities();
    zassert_true(caps.voltage && caps.charger_input && caps.power_switch);
    ht::emulator_inject_battery({0, false, true});
    zassert_ok(ht::battery_sample());
    zassert_true(ht::battery_snapshot().reading.switch_on);
    ht::emulator_inject_battery({8400, true, false});
    zassert_ok(ht::battery_sample());
    zassert_false(ht::battery_snapshot().reading.switch_on);
    zassert_true(ht::battery_snapshot().reading.charger_input);
    ht::emulator_inject_battery({}, 7);
    zassert_equal(ht::battery_sample(), -EIO);
    zassert_equal(ht::battery_snapshot().freshness, ht::BatteryFreshness::Stale);
}

ZTEST(battery, test_reference_conversion_thresholds_and_signed_domain) {
    ht::BatteryReading reading;
    zassert_ok(ht::c62_decode_battery(2100, 2048, 3300, reading));
    zassert_equal(reading.millivolts, 0);
    zassert_false(reading.switch_on || reading.charger_input);
    zassert_ok(ht::c62_decode_battery(2101, 2669, 3300, reading));
    zassert_equal(reading.millivolts, 3000);
    zassert_false(reading.switch_on);
    zassert_true(reading.charger_input);
    zassert_ok(ht::c62_decode_battery(2101, 2670, 3300, reading));
    zassert_equal(reading.millivolts, 3006);
    zassert_true(reading.switch_on);
    zassert_ok(ht::c62_decode_battery(2048, 3785, 3300, reading));
    zassert_equal(reading.millivolts, 8394);
    zassert_false(reading.charger_input);
    zassert_ok(ht::c62_decode_battery(4095, 4095, 3300, reading));
    zassert_equal(reading.millivolts, 9894);
    const auto saved = reading;
    zassert_equal(ht::c62_decode_battery(2048, 2047, 3300, reading), -ERANGE);
    zassert_equal(ht::c62_decode_battery(2048, 0, 3300, reading), -ERANGE);
    zassert_equal(ht::c62_decode_battery(4096, 3785, 3300, reading), -ERANGE);
    zassert_equal(ht::c62_decode_battery(2048, 4096, 3300, reading), -ERANGE);
    zassert_equal(ht::c62_decode_battery(2048, 3785, 0, reading), -ERANGE);
    zassert_equal(ht::c62_decode_battery(2048, 3785, 65535, reading), -ERANGE);
    zassert_equal(reading.millivolts, saved.millivolts);
    zassert_equal(reading.switch_on, saved.switch_on);
    zassert_equal(reading.charger_input, saved.charger_input);
}

ZTEST_SUITE(battery, NULL, setup, NULL, NULL, NULL);
