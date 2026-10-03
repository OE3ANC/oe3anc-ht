// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_storage.hpp>
#include <ht/emulator.hpp>
#include <ht/settings.hpp>
#include <ht/ui.hpp>
#include <stdlib.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
using namespace ht;
static atomic_t observed_lock;
static Codeplug plug;
static char root[] = "/tmp/ht-lock-service-XXXXXX";
K_MSGQ_DEFINE(inputs, sizeof(UiInput), 16, 4);
extern "C" void __real__ZN2ht14ui_view_updateERKNS_14UiPresentationE(const UiPresentation &view);

extern "C" void __wrap__ZN2ht14ui_view_updateERKNS_14UiPresentationE(const UiPresentation &view) {
    atomic_set(&observed_lock, view.home.locked);
    __real__ZN2ht14ui_view_updateERKNS_14UiPresentationE(view);
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
    if (key == UiKey::Star) {
        ui_keypad_star(pressed, k_uptime_get());
    }
    UiInput input{key, 0, pressed};
    if (key <= UiKey::Digit) {
        zassert_ok(ui_queue_input(input));
    } else {
        zassert_ok(k_msgq_put(&inputs, &input, K_NO_WAIT));
    }
    k_sleep(K_MSEC(30));
    radio_service();
}

ZTEST(ui_lock_service, test_real_ui_worker_lock_front_keys_side_inputs_and_unlock) {
    zassert_not_null(mkdtemp(root));
    zassert_ok(setenv("HT_SETTINGS_DIR", root, 1));
    zassert_ok(setenv("HT_PROFILE", "lock", 1));
    zassert_ok(setenv("SDL_VIDEODRIVER", "dummy", 1));
    zassert_ok(settings_storage_init());
    plug.global.ui.idle_s = 0;
    plug.vfo.rx_frequency_hz = plug.vfo.tx_frequency_hz = 430000000;
    uint32_t generation = 0;
    zassert_ok(codeplug_save(plug, generation));
    RadioConfig config;
    Selection selection;
    zassert_ok(settings_start(config, selection));
    zassert_ok(radio_start(config, selection));
    k_thread_create(&ui_thread, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), run, nullptr, nullptr,
                    nullptr, 5, 0, K_NO_WAIT);
    k_sleep(K_MSEC(100));
    zassert_false(atomic_get(&observed_lock));
    inject(UiKey::Star);
    k_sleep(K_MSEC(1100));
    zassert_true(atomic_get(&observed_lock));
    inject(UiKey::Star, false);
    inject(UiKey::Up);
    inject(UiKey::Enter);
    inject(UiKey::Right);
    zassert_equal(emulator_tuned_frequency(), 430000000);
    zassert_true(atomic_get(&observed_lock));
    inject(UiKey::Monitor);
    zassert_true(radio_snapshot().monitor_active);
    inject(UiKey::Monitor, false);
    zassert_false(radio_snapshot().monitor_active);
    inject(UiKey::Ptt);
    zassert_true(emulator_transmitting());
    inject(UiKey::Ptt, false);
    zassert_false(emulator_transmitting());
    inject(UiKey::Star);
    inject(UiKey::Release);
    k_sleep(K_MSEC(1100));
    zassert_true(atomic_get(&observed_lock));
    inject(UiKey::Star, false);
    inject(UiKey::Star);
    k_sleep(K_MSEC(1100));
    zassert_false(atomic_get(&observed_lock));
    inject(UiKey::Star, false);
    inject(UiKey::Up);
    zassert_equal(emulator_tuned_frequency(), 430012500);
    // This single-case fixture leaves the UI owner alive at terminal completion.
}

ZTEST_SUITE(ui_lock_service, nullptr, nullptr, nullptr, nullptr, nullptr);
