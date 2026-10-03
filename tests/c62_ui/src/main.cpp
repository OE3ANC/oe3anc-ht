// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_io.h"
#include <errno.h>
#include <ht/emulator.hpp>
#include <ht/ui.hpp>
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

using namespace ht;
static atomic_t ptt = 1;           // Held at boot must never transmit.
static atomic_t monitor = 1;       // Held at boot must not open RX audio.
static atomic_t matrix = 1U << 11; // Star held at boot must not start a gesture.
static atomic_t gpio_error;
static int display_error;
static bool redraw_requested;
static unsigned writes;
static uint8_t wire[8];
static uint16_t last_x, last_y, last_width, last_height;
static lv_color_t pixels[160 * 16];
static lv_disp_draw_buf_t buffer;
static lv_disp_drv_t driver;
static lv_disp_t *display;

extern "C" bool c62_display_redraw_requested() {
    const bool pending = redraw_requested;
    redraw_requested = false;
    return pending;
}

extern "C" int c62_controls_init() {
    return 0;
}

extern "C" int c62_display_init() {
    return 0;
}

extern "C" int c62_backlight_set(uint8_t) {
    return 0;
}

extern "C" int c62_ptt_read() {
    return atomic_get(&ptt);
}

extern "C" int c62_monitor_read() {
    return atomic_get(&monitor);
}

extern "C" int c62_keys_read(uint32_t *keys) {
    *keys = atomic_get(&matrix);
    return atomic_get(&gpio_error);
}

extern "C" int c62_display_write(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                 const void *data, size_t size) {
    ++writes;
    last_x = x;
    last_y = y;
    last_width = width;
    last_height = height;
    zassert_equal(size, size_t(width) * height * 2);
    memcpy(wire, data, MIN(size, sizeof(wire)));
    return display_error;
}

static void *setup() {
    lv_init();
    zassert_ok(radio_start(RadioConfig{}));
    zassert_ok(ui_backend_start());
    lv_disp_draw_buf_init(&buffer, pixels, nullptr, 160 * 16);
    lv_disp_drv_init(&driver);
    driver.hor_res = 160;
    driver.ver_res = 128;
    driver.draw_buf = &buffer;
    driver.flush_cb = ui_backend_flush;
    display = lv_disp_drv_register(&driver);
    zassert_not_null(display);
    zassert_ok(ui_view_start(lv_disp_get_scr_act(display)));
    return nullptr;
}

static void settle() {
    k_sleep(K_MSEC(40));
}

static UiInput matrix_key(unsigned bit) {
    atomic_clear(&matrix);
    settle();
    UiInput input;
    while (ui_backend_input(input)) {
    }
    atomic_set(&matrix, 1U << bit);
    settle();
    zassert_true(ui_backend_input(input));
    zassert_true(input.timestamp_ms >= 0 && input.timestamp_ms <= k_uptime_get());
    zassert_false(ui_backend_input(input)); // No repeat for a held key.
    return input;
}

ZTEST(c62_ui, test_controls_debounce_release_and_fault_without_ui_service) {
    settle();
    zassert_false(ui_keypad_gesture().pressed);
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_false(radio_snapshot().monitor_active);
    atomic_clear(&monitor);
    settle();
    atomic_set(&monitor, 1);
    k_sleep(K_MSEC(10));
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    settle();
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    UiInput side_input;
    zassert_false(ui_backend_input(side_input), "Side key must not emit front P1");
    atomic_clear(&monitor);
    k_sleep(K_MSEC(10));
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    atomic_clear(&ptt);
    settle();
    atomic_set(&ptt, 1);
    k_sleep(K_MSEC(10));
    zassert_false(radio_ptt_requested());
    settle();
    radio_service();
    zassert_true(emulator_transmitting());
    atomic_clear(&ptt);
    k_sleep(K_MSEC(10));
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());

    const UiKey navigation[] = {UiKey::Enter, UiKey::Up, UiKey::Down, UiKey::Back};
    for (unsigned bit = 0; bit < 4; ++bit) {
        zassert_equal(matrix_key(bit).key, navigation[bit]);
    }
    const auto digit = matrix_key(4);
    zassert_equal(digit.key, UiKey::Digit);
    zassert_equal(digit.character, '3');
    zassert_equal(matrix_key(11).key, UiKey::Star);
    zassert_equal(matrix_key(12).character, '0');
    zassert_equal(matrix_key(13).key, UiKey::Hash);
    zassert_equal(matrix_key(16).key, UiKey::Left);
    radio_service();
    zassert_false(radio_snapshot().monitor_active, "P1 is not the side key");
    zassert_equal(matrix_key(17).key, UiKey::Right);
    zassert_equal(matrix_key(18).character, '1');
    zassert_equal(matrix_key(19).character, '2');
    atomic_set(&matrix, (1U << 14) | (1U << 15));
    settle();
    UiInput input;
    zassert_false(ui_backend_input(input));

    // Let the bounded UI queue fill without running LVGL or draining input.
    for (unsigned i = 0; i < 20; ++i) {
        atomic_clear(&matrix);
        settle();
        atomic_set(&matrix, 1);
        settle();
    }
    UiModel locked;
    locked.sync(radio_snapshot());
    atomic_clear(&matrix);
    settle();
    // A simultaneous Star+Up cannot arm even if Up is immediately released
    // and both ordinary queue events are dropped.
    const auto previous_press = ui_keypad_gesture().press_sequence;
    atomic_set(&matrix, (1U << 11) | (1U << 1));
    const auto deadline = k_uptime_get() + 100;
    while (ui_keypad_gesture().press_sequence == previous_press && k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    zassert_not_equal(ui_keypad_gesture().press_sequence, previous_press);
    atomic_set(&matrix, 1U << 11); // Drop Up before the next producer scan.
    const auto simultaneous = ui_keypad_gesture();
    zassert_true(simultaneous.pressed);
    zassert_equal(simultaneous.cancelled_press, simultaneous.press_sequence);
    locked.advance(k_uptime_get());
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_false(locked.keypad_locked());
    atomic_clear(&matrix);
    settle();
    atomic_set(&matrix, 1U << 11);
    settle();
    locked.advance(k_uptime_get());
    atomic_clear(&matrix);
    k_sleep(K_MSEC(10));
    zassert_false(ui_keypad_gesture().pressed, "Star release bypasses full queue");
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_false(locked.keypad_locked());
    atomic_set(&matrix, 1U << 11);
    settle();
    locked.advance(k_uptime_get());
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_true(locked.keypad_locked());
    atomic_clear(&matrix);
    settle();
    atomic_set(&matrix, 1U << 11);
    settle();
    locked.advance(k_uptime_get());
    atomic_set(&matrix, (1U << 11) | (1U << 1));
    k_sleep(K_MSEC(10));
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_true(locked.keypad_locked());
    atomic_clear(&matrix);
    settle();
    atomic_set(&matrix, 1U << 11);
    settle();
    locked.advance(k_uptime_get());
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_false(locked.keypad_locked());
    atomic_clear(&matrix);
    settle();
    atomic_set(&matrix, 1U << 11);
    settle();
    locked.advance(k_uptime_get());
    k_sleep(K_MSEC(1010));
    locked.advance(k_uptime_get());
    zassert_true(locked.keypad_locked());
    atomic_clear(&matrix);
    settle();
    atomic_set(&monitor, 1);
    settle();
    radio_service();
    zassert_true(radio_snapshot().monitor_active); // UI queue is full.
    zassert_true(locked.keypad_locked());
    atomic_clear(&monitor);
    k_sleep(K_MSEC(10));
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    atomic_set(&ptt, 1);
    settle();
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_true(locked.keypad_locked());
    RadioCommand command;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    atomic_clear(&ptt);
    k_sleep(K_MSEC(10));
    radio_service();
    zassert_false(emulator_transmitting());
    unsigned count = 0;
    while (ui_backend_input(input)) {
        ++count;
    }
    zassert_equal(count, 16);
    for (unsigned i = 0; i < 8; ++i) {
        radio_service();
    }
    atomic_set(&ptt, 1);
    settle();
    radio_service();
    zassert_true(emulator_transmitting());
    atomic_set(&gpio_error, -EIO);
    k_sleep(K_MSEC(10));
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EIO);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
}

ZTEST(c62_ui, test_display_geometry_byte_order_buffer_restore_and_fault) {
    zassert_ok(radio_start(RadioConfig{}));
    UiModel model;
    model.sync(radio_snapshot());
    ui_view_update(model);
    lv_refr_now(display);
    zassert_true(writes > 0);
    const unsigned before_redraw = writes;
    redraw_requested = true;
    ui_backend_service();
    zassert_false(redraw_requested);
    lv_refr_now(display);
    zassert_true(writes >= before_redraw + 8, "Resume must redraw the entire 160x128 screen");
    lv_mem_monitor_t heap;
    lv_mem_monitor(&heap);
    zassert_equal(heap.total_size, 32768);
    zassert_true(heap.free_size > 4096);
    zassert_equal(lv_mem_test(), LV_RES_OK);
    lv_color_t colors[4];
    const uint16_t values[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (unsigned i = 0; i < 4; ++i) {
        colors[i].full = values[i];
    }
    const lv_area_t area{12, 34, 13, 35};
    ui_backend_flush(&driver, &area, colors);
    const uint8_t expected[] = {0xf8, 0, 0x07, 0xe0, 0, 0x1f, 0xff, 0xff};
    zassert_mem_equal(wire, expected, sizeof(wire));
    for (unsigned i = 0; i < 4; ++i) {
        zassert_equal(colors[i].full, values[i]);
    }
    zassert_equal(last_x, 12);
    zassert_equal(last_y, 34);
    zassert_equal(last_width, 2);
    zassert_equal(last_height, 2);
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    display_error = -EPIPE;
    ui_backend_flush(&driver, &area, colors);
    for (unsigned i = 0; i < 4; ++i) {
        zassert_equal(colors[i].full, values[i]);
    }
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().fault, -EPIPE);
    const unsigned before = writes;
    ui_backend_flush(&driver, &area, colors);
    zassert_equal(writes, before); // No automatic display recovery.
}

ZTEST_SUITE(c62_ui, nullptr, setup, nullptr, nullptr, nullptr);
