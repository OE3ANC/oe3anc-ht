// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/ui.hpp>
#include <ht/ui_companion.hpp>
#include <ht/ui_presentation_wire.hpp>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#ifdef CONFIG_HT_RESOURCE_DIAGNOSTICS
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ht_ui_resources);
#endif
#ifdef CONFIG_HT_SETTINGS
#include <ht/settings.hpp>
#endif

namespace ht {
static lv_color_t pixels[160 * 16];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
// UI-thread-owned bounded drafts, not nested stack objects. C62 application
// PSRAM excludes the DSP reservation; no UI allocations use the Codec2 heap.
// Use loaded data: NOLOAD PSRAM would discard UiModel's nonzero defaults.
#ifdef CONFIG_BOARD_C62
static UiModel model __attribute__((section(".psramdata_section")));
static UiPresentation presentation __attribute__((section(".psramdata_section")));
#else
static UiModel model;
static UiPresentation presentation;
#endif
#ifdef CONFIG_HT_COMPANION
#ifdef CONFIG_BOARD_C62
static UiSnapshotStore snapshots __attribute__((section(".psramdata_section")));
#else
static UiSnapshotStore snapshots;
#endif
#endif
static K_MUTEX_DEFINE(model_mutex);
static bool ready;
static uint32_t cps_pending;

#ifdef CONFIG_HT_COMPANION
int ui_copy_presentation(uint8_t *bytes, size_t capacity, size_t &length, uint32_t &revision) {
    if (k_mutex_lock(&model_mutex, K_NO_WAIT)) {
        return -EBUSY;
    }
    const int error =
        ready ? snapshots.copy(k_uptime_get(), bytes, capacity, length, revision) : -EAGAIN;
    k_mutex_unlock(&model_mutex);
    return error;
}
#endif

#ifdef CONFIG_HT_CODEPLUG_STORAGE
int ui_replace_codeplug(const Codeplug &plug, uint32_t id, const RadioState &expected,
                        uint32_t revision) {
    if (k_mutex_lock(&model_mutex, K_NO_WAIT)) {
        return -EBUSY;
    }
    int error = !ready || cps_pending || !model.cps_available()
                    ? -EBUSY
                    : settings_replace_codeplug(plug, id, expected, revision);
    if (!error) {
        cps_pending = id;
    }
    k_mutex_unlock(&model_mutex);
    return error;
}
#endif

void ui_run() {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "ui");
#endif
    lv_init();
    model.sync(radio_snapshot());
    const int error = ui_backend_start(model.backlight_percent());
    if (error) {
        printk("UI startup failed: %d\n", error);
        radio_report_fault(error);
        return;
    }
    lv_disp_draw_buf_init(&draw_buffer, pixels, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = ui_backend_flush;
    lv_disp_t *display = lv_disp_drv_register(&driver);
    if (!display) {
        printk("UI display allocation failed\n");
        radio_report_fault(-ENOMEM);
        return;
    }
    if (ui_view_start(lv_disp_get_scr_act(display))) {
        printk("UI label allocation failed\n");
        radio_report_fault(-ENOMEM);
        return;
    }
    int64_t previous = k_uptime_get();
    k_mutex_lock(&model_mutex, K_FOREVER);
    ready = true;
    k_mutex_unlock(&model_mutex);
#ifdef CONFIG_HT_RESOURCE_DIAGNOSTICS
    int64_t next_measurement = previous + 15000;
#endif
    while (true) {
        const int64_t now = k_uptime_get();
        lv_tick_inc(static_cast<uint32_t>(now - previous));
        previous = now;
        k_mutex_lock(&model_mutex, K_FOREVER);
        model.sync(radio_snapshot());
#ifdef CONFIG_HT_SETTINGS
        const auto storage = settings_status();
        model.storage_status(storage.load_error, storage.save_error, storage.pending);
        if (cps_pending && (storage.operation_id != cps_pending || !storage.operation_pending)) {
            cps_pending = 0;
        }
#endif
        UiInput input;
        while (ui_backend_input(input)) {
            // The model observes cancellation even for loss-independent inputs.
            model.input(input, cps_pending != 0);
            if (input.key == UiKey::Ptt) {
                radio_ptt(input.pressed);
            } else if (input.key == UiKey::Monitor) {
                radio_monitor(input.pressed);
            } else if (input.key == UiKey::Release || input.key == UiKey::Quit) {
                radio_ptt(false);
                radio_monitor(false);
            }
        }
        model.advance(k_uptime_get());
        ui_capture_presentation(model, presentation);
#ifdef CONFIG_HT_COMPANION
        snapshots.publish(presentation, k_uptime_get());
#endif
        k_mutex_unlock(&model_mutex);
        if (ui_backend_has_backlight()) {
            const int light_error = ui_backend_backlight(model.backlight_percent());
            if (light_error) {
                radio_report_fault(light_error);
            }
        }
        ui_view_update(presentation);
        ui_backend_service();
        lv_timer_handler();
#ifdef CONFIG_HT_RESOURCE_DIAGNOSTICS
        if (now >= next_measurement) {
            // LVGL remains owned by this thread, including its heap traversal.
            lv_mem_monitor_t memory;
            lv_mem_monitor(&memory);
            LOG_INF("LVGL heap capacity=%u used=%u free=%u largest=%u fragmentation=%u%%",
                    unsigned(memory.total_size), unsigned(memory.total_size - memory.free_size),
                    unsigned(memory.free_size), unsigned(memory.free_biggest_size),
                    unsigned(memory.frag_pct));
            next_measurement = now + 15000;
        }
#endif
        k_sleep(K_MSEC(10));
    }
}
} // namespace ht
