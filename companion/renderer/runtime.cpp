// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/ui_presentation_wire.hpp>
#include <ht/ui_view.hpp>
#include <stdint.h>

namespace {
static uint8_t input[ht::companion::UI_MAX_BYTES];
static ht::UiPresentation staging;
static lv_color_t draw_pixels[160 * 16];
static uint16_t pixels[160 * 128];
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t driver;
static bool started;

void flush(lv_disp_drv_t *d, const lv_area_t *area, lv_color_t *colors) {
    unsigned offset = 0;
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            pixels[y * 160 + x] = colors[offset++].full;
        }
    }
    lv_disp_flush_ready(d);
}
} // namespace

extern "C" {
uint8_t *ht_ui_buffer() {
    return input;
}

uint16_t *ht_ui_pixels() {
    return pixels;
}

int ht_ui_start() {
    if (started) {
        return 0;
    }
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &draw_buffer;
    driver.flush_cb = flush;
    auto *display = lv_disp_drv_register(&driver);
    if (!display) {
        return -ENOMEM;
    }
    const int error = ht::ui_view_start(lv_disp_get_scr_act(display));
    if (error) {
        return error;
    }
    started = true;
    return 0;
}

int ht_ui_apply(unsigned size) {
    if (!started || size > sizeof(input)) {
        return -EINVAL;
    }
    const int error = ht::ui_decode_presentation(input, size, staging);
    if (error) {
        return error;
    }
    ht::ui_view_update(staging);
    lv_refr_now(nullptr);
    return 0;
}

void ht_ui_tick(unsigned milliseconds) {
    if (!started) {
        return;
    }
    // A resumed/suspended tab cannot catch up unbounded animation time.
    lv_tick_inc(milliseconds > 100 ? 100 : milliseconds);
    lv_timer_handler();
}
}
