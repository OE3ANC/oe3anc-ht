// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
using namespace ht;
static bool fail_start;
static atomic_t fail_light, observed_light = -1;
static Codeplug plug;
static char root[] = "/tmp/ht-light-service-XXXXXX";
K_MSGQ_DEFINE(inputs, sizeof(UiInput), 16, 4);
extern "C" int __real__ZN2ht16ui_backend_startEh(uint8_t percent);

extern "C" int __wrap__ZN2ht16ui_backend_startEh(uint8_t percent) {
    return fail_start ? -EIO : __real__ZN2ht16ui_backend_startEh(percent);
}

extern "C" int __real__ZN2ht20ui_backend_backlightEh(uint8_t percent);

extern "C" int __wrap__ZN2ht20ui_backend_backlightEh(uint8_t percent) {
    const int error = __real__ZN2ht20ui_backend_backlightEh(percent);
    atomic_set(&observed_light, percent);
    return atomic_cas(&fail_light, 1, 0) ? -EIO : error;
}

extern "C" bool __real__ZN2ht16ui_backend_inputERNS_7UiInputE(UiInput &input);

extern "C" bool __wrap__ZN2ht16ui_backend_inputERNS_7UiInputE(UiInput &input) {
    return k_msgq_get(&inputs, &input, K_NO_WAIT) == 0 ||
           __real__ZN2ht16ui_backend_inputERNS_7UiInputE(input);
}

static void run(void *, void *, void *) {
    ui_run();
}

static K_THREAD_STACK_DEFINE(ui_stack, 4096);
static struct k_thread ui_thread;

static void inject(UiKey key, bool pressed = true) {
    const UiInput input{key, 0, pressed};
    if (key <= UiKey::Digit) {
        zassert_ok(ui_queue_input(input));
    } else {
        zassert_ok(k_msgq_put(&inputs, &input, K_NO_WAIT));
    }
    k_sleep(K_MSEC(30));
    radio_service();
}

ZTEST(ui_backlight_service, test_actual_service_start_error_dim_wake_ptt_and_brightness_error) {
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "light", 1));
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    zassert_ok(settings_storage_init());
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000000;
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    fail_start = true;
    ui_run();
    radio_service();
    zassert_equal(radio_snapshot().fault, -EIO);
    fail_start = false;
    zassert_ok(radio_start(config, selection));
    k_thread_create(&ui_thread, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), run, nullptr, nullptr,
                    nullptr, 5, 0, K_NO_WAIT);
    k_sleep(K_MSEC(100));
    zassert_equal(atomic_get(&observed_light), 100);
    k_sleep(K_SECONDS(15));
    zassert_equal(atomic_get(&observed_light), 10);
    inject(UiKey::Up);
    zassert_equal(atomic_get(&observed_light), 100);
    zassert_equal(emulator_tuned_frequency(), 430000000); // First key woke only.
    inject(UiKey::Up);
    zassert_equal(emulator_tuned_frequency(), 430012500);
    k_sleep(K_SECONDS(15));
    zassert_equal(atomic_get(&observed_light), 10);
    inject(UiKey::Ptt);
    zassert_true(emulator_transmitting());
    zassert_equal(atomic_get(&observed_light), 100);
    inject(UiKey::Ptt, false);
    zassert_false(emulator_transmitting());
    k_sleep(K_SECONDS(15));
    zassert_equal(atomic_get(&observed_light), 10);
    atomic_set(&fail_light, 1);
    k_sleep(K_MSEC(30));
    radio_service();
    zassert_equal(radio_snapshot().fault, -EIO);
    k_sleep(K_MSEC(30));
    zassert_equal(atomic_get(&observed_light), 100);
    // The single-case fixture ends with its UI worker alive; no mutex-owning thread is aborted.
}

ZTEST_SUITE(ui_backlight_service, nullptr, nullptr, nullptr, nullptr, nullptr);
