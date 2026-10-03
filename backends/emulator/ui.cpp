// SPDX-License-Identifier: GPL-3.0-or-later
#include "../linux/sdl.hpp"
#include <errno.h>
#include <ht/emulator.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
static SdlDisplay device_display;
static SdlDisplay developer_display;
static lv_color_t developer_pixels[240 * 16];
static lv_disp_draw_buf_t developer_buffer;
static lv_disp_drv_t developer_driver;
#ifdef CONFIG_HT_BATTERY
static constexpr unsigned row_count = 16, control_count = 10;
static BatteryReading battery_input;
static int battery_error;
#else
static constexpr unsigned row_count = 11, control_count = 6;
#endif
static lv_obj_t *labels[row_count];
static BackendStatus injected;
static unsigned selected;
static bool editing;
static char draft[16];
static size_t length;
static int developer_error;

static void developer_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    if (sdl_flush(developer_display, *area, pixels)) {
        developer_error = -EIO;
    }
    lv_disp_flush_ready(driver);
}

bool ui_backend_has_backlight() {
    return true;
}

int ui_backend_backlight(uint8_t percent) {
    return sdl_backlight(device_display, percent);
}

int ui_backend_start(uint8_t brightness_percent) {
    int scale = 3;
    const char *option = getenv("HT_SCALE");
    if (option) {
        if (strlen(option) != 1 || option[0] < '1' || option[0] > '8') {
            return -EINVAL;
        }
        scale = option[0] - '0';
    }
    int error = sdl_open(device_display, "OE3ANC HT - Space PTT", 160, 128, scale);
    if (error) {
        return error;
    }
    error = sdl_backlight(device_display, brightness_percent);
    if (error) {
        sdl_close(device_display);
        return error;
    }
    error = sdl_open(developer_display, "HT developer controls", 240, 4 + row_count * 17, 2);
    if (error) {
        sdl_close(device_display);
        return error;
    }
    lv_disp_draw_buf_init(&developer_buffer, developer_pixels, nullptr, 240 * 16);
    lv_disp_drv_init(&developer_driver);
    developer_driver.hor_res = 240;
    developer_driver.ver_res = 4 + row_count * 17;
    developer_driver.draw_buf = &developer_buffer;
    developer_driver.flush_cb = developer_flush;
    lv_disp_t *display = lv_disp_drv_register(&developer_driver);
    if (!display) {
        return -ENOMEM;
    }
    lv_obj_t *screen = lv_disp_get_scr_act(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    for (unsigned row = 0; row < row_count; ++row) {
        labels[row] = lv_label_create(screen);
        if (!labels[row]) {
            return -ENOMEM;
        }
        lv_obj_set_pos(labels[row], 4, 4 + row * 17);
        lv_obj_set_width(labels[row], 232);
        lv_label_set_long_mode(labels[row], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(labels[row], lv_color_hex(0xe5e7eb), 0);
        lv_label_set_text(labels[row], "");
    }
#ifdef CONFIG_HT_BATTERY
    battery_input = emulator_battery_input(battery_error);
#endif
    injected.rssi_dbm = -85;
    strcpy(injected.callsign, "OE1TEST");
    return 0;
}

void ui_backend_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    if (sdl_flush(device_display, *area, pixels)) {
        developer_error = -EIO;
    }
    lv_disp_flush_ready(driver);
}

static void developer_input(const UiInput &input) {
    if (!input.pressed) {
        return;
    }
    developer_error = 0;
    if (editing) {
        if (input.key == UiKey::Back) {
            editing = false;
        } else if (input.key == UiKey::Erase) {
            if (length) {
                draft[--length] = 0;
            }
        } else if ((input.key == UiKey::Character || input.key == UiKey::Digit) &&
                   length < sizeof(draft) - 1) {
            char c = input.character;
            if (c >= 'a' && c <= 'z') {
                c -= 'a' - 'A';
            }
            draft[length++] = c;
            draft[length] = 0;
        } else if (input.key == UiKey::Enter) {
#ifdef CONFIG_HT_BATTERY
            if (selected == 6) {
                char *end;
                const long mv = strtol(draft, &end, 10);
                if (!length || *end || mv < 0 || mv > 20000) {
                    developer_error = -EINVAL;
                    return;
                }
                battery_input.millivolts = mv;
                emulator_inject_battery(battery_input, battery_error);
            } else
#endif
                if (selected == 1) {
                char *end;
                const long rssi = strtol(draft, &end, 10);
                if (!length || *end || rssi < -127 || rssi > 0) {
                    developer_error = -EINVAL;
                    return;
                }
                injected.rssi_dbm = rssi;
            } else {
                char call[10] = {};
                if (length > 9) {
                    developer_error = -EINVAL;
                    return;
                }
                memcpy(call, draft, length);
                if (length && !valid_callsign(call)) {
                    developer_error = -EINVAL;
                    return;
                }
                memcpy(injected.callsign, call, sizeof(call));
            }
            emulator_inject(injected);
            editing = false;
        }
        return;
    }
    if (input.key == UiKey::Back) {
        selected = 0;
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        selected = (selected + (input.key == UiKey::Down ? 1 : control_count - 1)) % control_count;
    } else if (input.key == UiKey::Enter) {
        if (selected == 0) {
            injected.rx_active = !injected.rx_active;
            emulator_inject(injected);
        } else if (selected < 3
#ifdef CONFIG_HT_BATTERY
                   || selected == 6
#endif
        ) {
            editing = true;
            length = 0;
            draft[0] = 0;
        } else if (selected < 5) {
            injected.error = selected == 3 ? -ETIMEDOUT : -EPIPE;
            emulator_inject(injected);
        } else if (selected == 5) {
            injected = {};
            injected.rssi_dbm = -85;
            strcpy(injected.callsign, "OE1TEST");
            emulator_inject(injected);
            emulator_request_reset();
#ifdef CONFIG_HT_BATTERY
        } else {
            if (selected == 7) {
                battery_input.charger_input = !battery_input.charger_input;
            }
            if (selected == 8) {
                battery_input.switch_on = !battery_input.switch_on;
            }
            if (selected == 9) {
                battery_error = battery_error ? 0 : -EIO;
            }
            emulator_inject_battery(battery_input, battery_error);
#endif
        }
    }
}

bool ui_backend_input(UiInput &input) {
    uint32_t window;
    while (sdl_input(input, window)) {
        if (input.key == UiKey::Quit) {
            radio_ptt(false);
            radio_monitor(false);
            // There is no live RF/audio in this process. Closing ends simulation.
            sdl_close(developer_display);
            sdl_close(device_display);
            exit(0);
        }
        if (input.key == UiKey::Release ||
            ((input.key == UiKey::Ptt || input.key == UiKey::Monitor) && !input.pressed)) {
            if (input.key == UiKey::Release) {
                ui_keypad_star(false, k_uptime_get());
                ui_keypad_cancel_gesture();
            }
            return true;
        }
        if (input.key == UiKey::Star && !input.pressed) {
            ui_keypad_star(false, k_uptime_get());
            return true;
        }
        if (window == SDL_GetWindowID(developer_display.window)) {
            developer_input(input);
        } else if (window == SDL_GetWindowID(device_display.window)) {
            if (input.key == UiKey::Star) {
                ui_keypad_star(input.pressed, k_uptime_get());
            } else if (input.pressed && input.key != UiKey::Ptt && input.key != UiKey::Monitor) {
                ui_keypad_cancel_gesture();
            }
            if (input.key == UiKey::Ptt || input.key == UiKey::Monitor) {
                return true;
            }
            // Pump one observed local front event before draining the shared
            // FIFO. Other producers already accepted by it keep their order.
            (void)ui_queue_input(input);
            break;
        }
    }
    return ui_take_input(input);
}

void ui_backend_service() {
    char text[row_count][48] = {};
    snprintf(text[0], 48, "EMULATOR CONTROLS");
    snprintf(text[1], 48, "%c RX activity: %s", selected == 0 ? '>' : ' ',
             injected.rx_active ? "on" : "off");
    snprintf(text[2], 48, "%c RSSI: %d dBm", selected == 1 ? '>' : ' ', injected.rssi_dbm);
    snprintf(text[3], 48, "%c Received call: %s", selected == 2 ? '>' : ' ', injected.callsign);
    snprintf(text[4], 48, "%c Inject DSP fault", selected == 3 ? '>' : ' ');
    snprintf(text[5], 48, "%c Inject audio fault", selected == 4 ? '>' : ' ');
    snprintf(text[6], 48, "%c Reset simulation", selected == 5 ? '>' : ' ');
#ifdef CONFIG_HT_BATTERY
    snprintf(text[7], 48, "%c Battery: %u mV", selected == 6 ? '>' : ' ', battery_input.millivolts);
    snprintf(text[8], 48, "%c Charger input: %s", selected == 7 ? '>' : ' ',
             battery_input.charger_input ? "on" : "off");
    snprintf(text[9], 48, "%c Power switch: %s", selected == 8 ? '>' : ' ',
             battery_input.switch_on ? "on" : "off");
    snprintf(text[10], 48, "%c Battery read error: %s", selected == 9 ? '>' : ' ',
             battery_error ? "on" : "off");
    const auto snapshot = battery_snapshot();
    const char *freshness = snapshot.freshness == BatteryFreshness::Fresh   ? "fresh"
                            : snapshot.freshness == BatteryFreshness::Stale ? "stale"
                                                                            : "unknown";
    snprintf(text[14], 48, "Sample: %s (%d)", freshness, snapshot.error);
#endif
    if (editing) {
        snprintf(text[selected + 1], 48, ">%s_", draft);
    }
    const unsigned help = 1 + control_count;
    snprintf(text[help], 48, "Arrows / Enter; type to edit");
    snprintf(text[help + 1], 48, "Backspace delete; Esc cancel");
    if (developer_error) {
        snprintf(text[help + 2], 48, "Error %d", developer_error);
    } else {
        snprintf(text[help + 2], 48, "TX: %s", emulator_transmitting() ? "keyed" : "off");
    }
    snprintf(text[row_count - 1], 48, "No live audio or DSP execution");
    for (unsigned row = 0; row < row_count; ++row) {
        if (strcmp(lv_label_get_text(labels[row]), text[row])) {
            lv_label_set_text(labels[row], text[row]);
        }
    }
}
} // namespace ht
