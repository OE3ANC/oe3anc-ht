// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_io.h"
#include <errno.h>
#include <ht/ui.hpp>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

namespace ht {
static struct k_thread controls_thread;
K_THREAD_STACK_DEFINE(controls_stack, 2048);
static bool display_failed;

struct HoldInput {
    bool armed = false;
    bool pressed = false;
    int64_t released_at = -1;
    int64_t pressed_at = -1;
};

// Press debounce and startup release arming shared by independent side inputs.
// Release never passes through the bounded front-key queue.
static void hold_input(HoldInput &state, bool down, int64_t now, void (*submit)(bool)) {
    if (!down) {
        if (state.pressed) {
            submit(false);
            state.pressed = false;
        }
        state.pressed_at = -1;
        if (state.released_at < 0) {
            state.released_at = now;
        }
        if (now - state.released_at >= 20) {
            state.armed = true;
        }
    } else {
        state.released_at = -1;
        if (state.pressed_at < 0) {
            state.pressed_at = now;
        }
        if (state.armed && !state.pressed && now - state.pressed_at >= 20) {
            submit(true);
            state.pressed = true;
        }
    }
}

static void controls(void *, void *, void *) {
#ifdef CONFIG_THREAD_NAME
    k_thread_name_set(k_current_get(), "controls");
#endif
    // Matrix order preserved from the working reference. Bits 16/17 are front
    // P1/P2 (left/right); the auxiliary side GPIO is a separate hold input.
    static constexpr UiInput map[] = {
        {UiKey::Enter},      {UiKey::Up},         {UiKey::Down},       {UiKey::Back},
        {UiKey::Digit, '3'}, {UiKey::Digit, '4'}, {UiKey::Digit, '5'}, {UiKey::Digit, '6'},
        {UiKey::Digit, '7'}, {UiKey::Digit, '8'}, {UiKey::Digit, '9'}, {UiKey::Star},
        {UiKey::Digit, '0'}, {UiKey::Hash},       {UiKey::Quit},       {UiKey::Quit},
        {UiKey::Left},       {UiKey::Right},      {UiKey::Digit, '1'}, {UiKey::Digit, '2'}};
    HoldInput ptt, monitor;
    int64_t changed_at = 0;
    uint32_t candidate = 0, stable = 0;
    bool first_keys = true;
    while (true) {
        const int64_t now = k_uptime_get();
        const int value = c62_ptt_read();
        if (value < 0) {
            radio_report_fault(value);
            return;
        }
        hold_input(ptt, value != 0, now, radio_ptt);
        const int side = c62_monitor_read();
        if (side < 0) {
            radio_report_fault(side);
            return;
        }
        hold_input(monitor, side != 0, now, radio_monitor);
        uint32_t current;
        const int error = c62_keys_read(&current);
        if (error) {
            ui_keypad_star(false, now);
            ui_keypad_cancel_gesture();
            radio_report_fault(error);
            return;
        }
        current &= 0xfffffU & ~((1U << 14) | (1U << 15));
        // Release/cancellation cannot be lost when the front queue is full.
        if (!(current & (1U << 11))) {
            ui_keypad_star(false, now);
        }
        if (current & ~(1U << 11)) {
            ui_keypad_cancel_gesture();
        }
        if (first_keys) {
            candidate = stable = current; // Ignore held keys at startup.
            first_keys = false;
        }
        if (current != candidate) {
            candidate = current;
            changed_at = now;
        }
        if (now - changed_at >= 20 && candidate != stable) {
            const uint32_t down = candidate & ~stable;
            stable = candidate;
            if (down & (1U << 11)) {
                ui_keypad_star(true, now, (current & ~(1U << 11)) != 0);
            }
            for (unsigned bit = 0; bit < 20; ++bit) {
                if (down & (1U << bit)) {
                    auto input = map[bit];
                    input.timestamp_ms = now;
                    (void)ui_queue_input(input);
                }
            }
        }
        k_sleep(K_MSEC(5));
    }
}

bool ui_backend_has_backlight() {
    return true;
}

int ui_backend_backlight(uint8_t percent) {
    return c62_backlight_set(percent);
}

int ui_backend_start(uint8_t brightness_percent) {
    int error = c62_backlight_set(brightness_percent);
    if (!error) {
        error = c62_controls_init();
    }
    if (!error) {
        error = c62_display_init();
    }
    if (error) {
        radio_report_fault(error);
        return error;
    }
    k_thread_create(&controls_thread, controls_stack, K_THREAD_STACK_SIZEOF(controls_stack),
                    controls, nullptr, nullptr, nullptr, 4, 0, K_NO_WAIT);
    return 0;
}

void ui_backend_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    if (!display_failed) {
        const int width = area->x2 - area->x1 + 1;
        const int height = area->y2 - area->y1 + 1;
        int error = -EINVAL;
        if (area->x1 >= 0 && area->y1 >= 0 && area->x2 < 160 && area->y2 < 128 && width > 0 &&
            height > 0 && width * height <= 160 * 16) {
            const size_t count = static_cast<size_t>(width * height);
            // The pinned ST7735R driver sends bytes as supplied. Swap in place
            // for the synchronous SPI write, then restore LVGL's draw buffer.
            for (size_t i = 0; i < count; ++i) {
                pixels[i].full = sys_cpu_to_be16(pixels[i].full);
            }
            error = c62_display_write(area->x1, area->y1, width, height, pixels,
                                      count * sizeof(lv_color_t));
            for (size_t i = 0; i < count; ++i) {
                pixels[i].full = sys_be16_to_cpu(pixels[i].full);
            }
        }
        if (error) {
            display_failed = true;
            printk("C62 display failed: %d\n", error);
            radio_report_fault(error);
        }
    }
    lv_disp_flush_ready(driver);
}

bool ui_backend_input(UiInput &input) {
    return ui_take_input(input);
}

void ui_backend_service() {
    if (c62_display_redraw_requested()) {
        // Only the UI owner touches LVGL. The request follows output readiness,
        // including when an earlier UI resume render was discarded while off.
        lv_obj_invalidate(lv_disp_get_scr_act(lv_disp_get_default()));
    }
}
} // namespace ht
