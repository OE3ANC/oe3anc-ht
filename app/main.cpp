// SPDX-License-Identifier: GPL-3.0-or-later
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#ifdef CONFIG_HT_POWER
#include <ht/power.hpp>
#endif
#ifdef CONFIG_HT_BATTERY
#include <ht/battery.hpp>

static void battery_worker(void *, void *, void *) {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "battery");
#endif
    while (true) {
        k_sleep(K_MSEC(100));
        ht::battery_sample();
#ifdef CONFIG_HT_POWER
        ht::power_sample(ht::battery_snapshot(), k_uptime_get());
#endif
    }
}

K_THREAD_STACK_DEFINE(battery_stack, 1024);
static struct k_thread battery_thread;
#endif
#if defined(CONFIG_HT_LVGL) && !defined(CONFIG_HT_UI)
#include <lvgl.h>
#endif
#ifdef CONFIG_HT_UI
#include <ht/ui.hpp>

static void ui_worker(void *, void *, void *) {
    ht::ui_run();
}

K_THREAD_STACK_DEFINE(ui_stack, 4096);
static struct k_thread ui_thread;
#endif
#ifdef CONFIG_HT_EMULATOR
#include <ht/emulator.hpp>
#endif
#ifdef CONFIG_HT_RADIO
#include <ht/radio.hpp>
#endif
#ifdef CONFIG_HT_SETTINGS
#include <ht/settings.hpp>

static void settings_worker(void *, void *, void *) {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "settings");
#endif
#ifdef CONFIG_HT_POWER
    uint32_t last_shutdown = 0;
    bool last_pending = false;
    int last_error = 0;
#endif
    while (true) {
        const auto state = ht::radio_snapshot();
        ht::settings_service(state, k_uptime_get());
#ifdef CONFIG_HT_POWER
        const auto status = ht::settings_status();
        if (state.shutdown_sequence != last_shutdown || status.shutdown_pending != last_pending ||
            status.shutdown_error != last_error) {
            printk("Shutdown save: pending=%u dirty=%u error=%d generation=%u\n",
                   unsigned(status.shutdown_pending), unsigned(status.pending),
                   status.shutdown_error, status.generation);
            last_shutdown = state.shutdown_sequence;
            last_pending = status.shutdown_pending;
            last_error = status.shutdown_error;
        }
#endif
        k_sleep(K_MSEC(50));
    }
}

K_THREAD_STACK_DEFINE(settings_stack, 2048);
static struct k_thread settings_thread;
#endif

int main() {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "radio");
#endif
#if defined(CONFIG_HT_LVGL) && !defined(CONFIG_HT_UI)
    lv_init();
#endif
    printk("OE3ANC HT: Zephyr startup on %s\n", CONFIG_BOARD);
#ifdef CONFIG_HT_POWER
    const int power_error = ht::power_prepare();
    if (power_error) {
        printk("Inactive output setup failed: %d\n", power_error);
    }
#endif
#ifdef CONFIG_HT_BATTERY
    const int battery_error = ht::battery_sample();
    const auto battery = ht::battery_snapshot();
#ifdef CONFIG_HT_POWER
    ht::power_sample(battery, k_uptime_get());
#endif
    if (battery_error) {
        printk("Battery telemetry unavailable: %d\n", battery_error);
    } else {
        printk("Battery: %u mV charger-input=%u switch-on=%u\n", battery.reading.millivolts,
               unsigned(battery.reading.charger_input), unsigned(battery.reading.switch_on));
    }
    k_thread_create(&battery_thread, battery_stack, K_THREAD_STACK_SIZEOF(battery_stack),
                    battery_worker, nullptr, nullptr, nullptr, 11, 0, K_NO_WAIT);
#endif
#if defined(CONFIG_HT_POWER) && !defined(CONFIG_HT_EMULATOR)
    // Only battery telemetry runs here. No application radio, DSP/audio, UI or
    // settings writes until fresh switch samples confirm on. Emulator keeps its
    // developer panel available while the controller itself starts inactive.
    printk("Waiting for confirmed power switch\n");
    int64_t next_report = k_uptime_get() + 5000;
    while (!ht::power_switch_on()) {
        if (k_uptime_get() >= next_report) {
            const auto status = ht::battery_snapshot();
            printk("Inactive: battery-state=%u error=%d output-error=%d\n",
                   unsigned(status.freshness), status.error, power_error);
            next_report = k_uptime_get() + 5000;
        }
        k_sleep(K_MSEC(20));
    }
#endif
#ifdef CONFIG_HT_RADIO
    ht::RadioConfig config;
    ht::Selection selection;
#ifdef CONFIG_HT_SETTINGS
    const int load_error = ht::settings_start(config, selection);
    if (load_error) {
        printk("Settings defaults: %d\n", load_error);
    }
#endif
    const int error = ht::radio_start(config, selection);
    if (error) {
        printk("Radio startup failed: %d\n", error);
    }
#ifdef CONFIG_HT_POWER
    const int output_error = ht::power_apply_outputs(ht::radio_snapshot());
    if (output_error) {
        ht::radio_report_fault(output_error);
    }
#endif
#ifdef CONFIG_HT_SETTINGS
    k_thread_create(&settings_thread, settings_stack, K_THREAD_STACK_SIZEOF(settings_stack),
                    settings_worker, nullptr, nullptr, nullptr, 10, 0, K_NO_WAIT);
#endif
#endif
#ifdef CONFIG_HT_UI
    k_thread_create(&ui_thread, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), ui_worker, nullptr,
                    nullptr, nullptr, 5, 0, K_NO_WAIT);
#endif
    while (true) {
#ifdef CONFIG_HT_RADIO
#ifdef CONFIG_HT_EMULATOR
        if (ht::emulator_take_reset_request()) {
            const auto state = ht::radio_snapshot();
            ht::radio_start(state.config, state.selection);
        }
#endif
        ht::radio_service();
#ifdef CONFIG_HT_POWER
        const int output_error = ht::power_apply_outputs(ht::radio_snapshot());
        if (output_error) {
            ht::radio_report_fault(output_error);
        }
#endif
#endif
        k_sleep(K_MSEC(10));
    }
    return 0;
}
