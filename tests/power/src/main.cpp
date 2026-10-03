// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../modules/power/switch.hpp"
#include <ht/power.hpp>
#include <ht/power_backend.hpp>
#include <ht/emulator.hpp>
#include <errno.h>
#include <zephyr/ztest.h>

using namespace ht;
static int prepare_error, output_error;
static unsigned prepare_calls, output_calls;
static bool output_active;

namespace ht {
int power_backend_prepare() {
    ++prepare_calls;
    output_active = false;
    return prepare_error;
}

int power_backend_outputs(bool active) {
    ++output_calls;
    output_active = active;
    return output_error;
}
}

static BatterySnapshot sample(bool on, int64_t at) {
    BatterySnapshot s;
    s.reading = {on ? 7200u : 0u, true, on};
    s.freshness = BatteryFreshness::Fresh;
    s.sample_ms = s.attempt_ms = at;
    return s;
}

static void before(void *) {
    prepare_error = output_error = 0;
    prepare_calls = output_calls = 0;
    zassert_ok(power_prepare());
    zassert_ok(radio_start(RadioConfig{}));
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
}

static void confirm(bool on, int64_t at) {
    power_sample(sample(on, at), at);
    power_sample(sample(on, at + 100), at + 100);
    power_sample(sample(on, at + 200), at + 200);
}

ZTEST(power, test_fresh_distinct_samples_debounce_and_charger_does_not_start) {
    PowerSwitchFilter filter;
    auto s = sample(false, 0);
    zassert_false(filter.update(s, 0));
    s.sample_ms = 200;
    zassert_false(filter.update(s, 200)); // charger alone cannot start
    s = sample(true, 300);
    zassert_false(filter.update(s, 300));
    zassert_false(filter.update(s, 500)); // cached reading is not a new sample
    zassert_false(filter.update(sample(true, 499), 499));
    zassert_true(filter.update(sample(true, 500), 500));
    zassert_true(filter.update(sample(false, 600), 600));
    zassert_true(filter.update(sample(true, 700), 700)); // off bounce cancelled
    zassert_true(filter.update(sample(false, 800), 800));
    zassert_true(filter.update(sample(false, 999), 999));
    zassert_false(filter.update(sample(false, 1000), 1000));
}

ZTEST(power, test_unknown_stale_future_failed_and_gapped_samples) {
    PowerSwitchFilter filter;
    auto invalid = sample(true, 0);
    invalid.freshness = BatteryFreshness::Unknown;
    zassert_false(filter.update(invalid, 0));
    zassert_false(filter.update(sample(true, -1), 0));
    zassert_false(filter.update(sample(true, 100), 0));
    zassert_false(filter.update(sample(true, 0), 301));
    zassert_false(filter.update(sample(true, 400), 400));
    invalid = sample(true, 500);
    invalid.error = -EIO;
    zassert_false(filter.update(invalid, 500));
    zassert_false(filter.update(sample(true, 600), 600));
    zassert_false(filter.update(sample(true, 901), 901)); // gap restarts debounce
    zassert_false(filter.update(sample(true, 1000), 1000));
    zassert_true(filter.update(sample(true, 1101), 1101));
    invalid = sample(false, 1200);
    invalid.freshness = BatteryFreshness::Stale;
    zassert_true(filter.update(invalid, 1200)); // failed running read is not off
    zassert_true(filter.update(sample(false, 1300), 1300));
    zassert_true(filter.update(sample(false, 1200), 1300)); // backward time resets
    zassert_true(filter.update(sample(false, 1399), 1399));
    zassert_false(filter.update(sample(false, 1400), 1400));
}

ZTEST(power, test_minimal_inactive_start_and_confirmed_resume) {
    zassert_equal(prepare_calls, 1);
    zassert_false(power_switch_on());
    zassert_false(radio_power_requested());
    zassert_ok(power_apply_outputs(radio_snapshot()));
    zassert_equal(output_calls, 0);
    radio_power(false); // model prepare before the first controller service
    confirm(true, 0);
    zassert_true(power_switch_on()); // initial off edge still pending
    zassert_false(radio_power_requested());
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_ok(power_apply_outputs(radio_snapshot()));
    zassert_true(output_active);
    zassert_equal(output_calls, 1);
    zassert_ok(power_apply_outputs(radio_snapshot()));
    zassert_equal(output_calls, 1);
}

ZTEST(power, test_switch_off_unkeys_retains_fault_and_requires_new_ptt) {
    confirm(true, 0);
    radio_service();
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    confirm(false, 300);
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    confirm(true, 600);
    radio_service();
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    radio_report_fault(-EPIPE);
    radio_service();
    confirm(false, 900);
    radio_service();
    zassert_equal(radio_snapshot().fault, -EPIPE);
    confirm(true, 1200);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EPIPE);
}

ZTEST(power, test_prepare_failure_cannot_start_and_output_failure_is_latched) {
    prepare_error = -EIO;
    zassert_equal(power_prepare(), -EIO);
    confirm(true, 0);
    radio_service();
    zassert_false(power_switch_on());
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_equal(power_apply_outputs(radio_snapshot()), -EIO);
    zassert_equal(output_calls, 0);
    prepare_error = 0;
    zassert_ok(power_prepare());
    confirm(true, 0);
    radio_service();
    output_error = -EPIPE;
    zassert_equal(power_apply_outputs(radio_snapshot()), -EPIPE);
    output_error = 0;
    zassert_equal(power_apply_outputs(radio_snapshot()), -EPIPE);
    zassert_equal(output_calls, 1); // no repeated activation retries
    radio_report_fault(-EPIPE);
    radio_service();
    confirm(false, 300);
    radio_service();
    zassert_equal(power_apply_outputs(radio_snapshot()), -EPIPE);
    zassert_false(output_active); // still attempt off despite latched output error
}

ZTEST_SUITE(power, nullptr, nullptr, before, nullptr, nullptr);
