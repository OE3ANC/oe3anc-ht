// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/ui_view.hpp>
#include <ht/ui_theme.hpp>
#include <string.h>

namespace ht {
static lv_obj_t *view_screen;
static lv_obj_t *content_root, *title, *detail, *theme_rows[4], *selection, *stripe;
static lv_obj_t *separators[2], *footer[4], *list_prefix[4], *list_suffix[4];
static lv_obj_t *entry, *entry_line, *entry_help;
static lv_obj_t *lock_body, *lock_shackle;
static lv_obj_t *home_strip, *home_mode, *home_battery, *signal[5];
static bool home_visible, system_visible;
static bool appearance_visible, editor_visible, list_visible, content_visible, styled;
static UiScreen drawn_screen;
static uint16_t drawn_list_cursor;
static uint8_t drawn_form_cursor;
static Theme last_theme;
static Contrast last_contrast;
static uint32_t drawn_interruptions, drawn_ptt_sequence, drawn_monitor_sequence;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height) {
    auto *object = lv_obj_create(parent);
    if (object) {
        lv_obj_remove_style_all(object);
        lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, width, height);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    }
    return object;
}

static lv_obj_t *text(lv_obj_t *parent, int x, int y, int width, const lv_font_t *font) {
    auto *label = lv_label_create(parent);
    if (label) {
        lv_obj_set_pos(label, x, y);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_font(label, font, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, "");
    }
    return label;
}

static void text_color(lv_obj_t *object, uint32_t color) {
    const auto value = lv_color_hex(color);
    if (lv_obj_get_style_text_color(object, LV_PART_MAIN).full != value.full) {
        lv_obj_set_style_text_color(object, value, 0);
    }
}

static void box_color(lv_obj_t *object, uint32_t color) {
    const auto value = lv_color_hex(color);
    if (lv_obj_get_style_bg_color(object, LV_PART_MAIN).full != value.full) {
        lv_obj_set_style_bg_color(object, value, 0);
    }
}

static void changed_text(lv_obj_t *label, const char *value) {
    if (strcmp(lv_label_get_text(label), value)) {
        lv_label_set_text(label, value);
    }
}

static uint32_t status_color(UiStatusColor status, const UiPalette &colors) {
    return status == UiStatusColor::Red      ? colors.red
           : status == UiStatusColor::Amber  ? colors.amber
           : status == UiStatusColor::Accent ? colors.accent
                                             : colors.muted;
}

static void selection_y(void *object, int32_t y) {
    lv_obj_set_y(static_cast<lv_obj_t *>(object), y);
}

static void page_y(void *object, int32_t y) {
    lv_obj_set_y(static_cast<lv_obj_t *>(object), y);
}

int ui_view_start(lv_obj_t *screen) {
    view_screen = screen;
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lock_shackle = box(screen, 148, 3, 6, 6);
    lock_body = box(screen, 147, 7, 8, 6);
    if (!lock_body || !lock_shackle) {
        return -ENOMEM;
    }
    lv_obj_set_style_bg_opa(lock_shackle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lock_shackle, 1, 0);
    lv_obj_set_style_radius(lock_shackle, 3, 0);
    lv_obj_set_style_radius(lock_body, 1, 0);
    lv_obj_add_flag(lock_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lock_shackle, LV_OBJ_FLAG_HIDDEN);
    content_root = box(screen, 0, 0, 160, 128);
    if (!content_root) {
        return -ENOMEM;
    }
    selection = box(content_root, 5, 39, 150, 15);
    if (!selection) {
        return -ENOMEM;
    }
    lv_obj_set_style_radius(selection, 3, 0);
    stripe = box(selection, 0, 3, 2, 11);
    home_strip = box(content_root, 8, 75, 144, 12);
    if (!home_strip) {
        return -ENOMEM;
    }
    lv_obj_set_style_radius(home_strip, 3, 0);
    lv_obj_add_flag(home_strip, LV_OBJ_FLAG_HIDDEN);
    title = text(content_root, 8, 5, 144, &lv_font_montserrat_12);
    detail = text(content_root, 8, 27, 144, &lv_font_montserrat_10);
    separators[0] = box(content_root, 8, 23, 144, 1);
    separators[1] = box(content_root, 8, 99, 144, 1);
    if (!stripe || !title || !detail || !separators[0] || !separators[1]) {
        return -ENOMEM;
    }
    lv_label_set_text(title, "APPEARANCE");
    for (unsigned row = 0; row < 4; ++row) {
        theme_rows[row] = text(content_root, 12, 41 + 15 * row, 140, &lv_font_montserrat_10);
        footer[row] =
            text(content_root, row % 2 ? 82 : 8, row < 2 ? 102 : 115, 70, &lv_font_montserrat_10);
        if (!theme_rows[row] || !footer[row]) {
            return -ENOMEM;
        }
        lv_label_set_text(theme_rows[row],
                          ui_palette(static_cast<Theme>(row), Contrast::Normal).name);
        if (row % 2) {
            lv_obj_set_style_text_align(footer[row], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    home_mode = text(content_root, 103, 5, 25, &lv_font_montserrat_10);
    home_battery = text(content_root, 65, 5, 36, &lv_font_montserrat_10);
    if (!home_mode || !home_battery) {
        return -ENOMEM;
    }
    lv_obj_add_flag(home_mode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(home_battery, LV_OBJ_FLAG_HIDDEN);
    for (unsigned bar = 0; bar < 5; ++bar) {
        const int height = 3 + bar * 2;
        signal[bar] = box(content_root, 133 + bar * 4, 16 - height, 2, height);
        if (!signal[bar]) {
            return -ENOMEM;
        }
        lv_obj_add_flag(signal[bar], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_parent(lock_shackle, content_root);
    lv_obj_set_parent(lock_body, content_root);
    lv_obj_set_pos(lock_shackle, 148, 28);
    lv_obj_set_pos(lock_body, 147, 32);
    for (unsigned row = 0; row < 4; ++row) {
        list_prefix[row] = text(content_root, 11, 43 + 15 * row, 24, &lv_font_montserrat_10);
        list_suffix[row] = text(content_root, 136, 43 + 15 * row, 19, &lv_font_montserrat_10);
        if (!list_prefix[row] || !list_suffix[row]) {
            return -ENOMEM;
        }
        lv_obj_set_style_text_align(list_suffix[row], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(list_prefix[row], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_suffix[row], LV_OBJ_FLAG_HIDDEN);
    }
    entry = lv_textarea_create(content_root);
    entry_line = box(content_root, 8, 77, 144, 1);
    entry_help = text(content_root, 8, 85, 144, &lv_font_montserrat_10);
    if (!entry || !entry_line || !entry_help) {
        return -ENOMEM;
    }
    lv_textarea_set_one_line(entry, true);
    // one_line changes height to LV_SIZE_CONTENT, which clips the underline
    // caret at the font's bottom edge. Reserve explicit caret space afterwards.
    lv_obj_set_pos(entry, 8, 46);
    lv_obj_set_size(entry, 144, 29);
    lv_textarea_set_cursor_click_pos(entry, false);
    lv_obj_clear_flag(entry, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                                 LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(entry, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(entry, 0, 0);
    lv_obj_set_style_border_width(entry, 0, 0);
    lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, 0);
    lv_obj_set_style_anim_time(entry, 0, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_width(entry, 1, LV_PART_CURSOR);
    lv_obj_set_style_border_side(entry, LV_BORDER_SIDE_BOTTOM, LV_PART_CURSOR);
    lv_obj_add_flag(entry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(entry_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(entry_help, LV_OBJ_FLAG_HIDDEN);
    appearance_visible = editor_visible = list_visible = content_visible = home_visible =
        system_visible = styled = false;
    drawn_screen = UiScreen::Home;
    drawn_interruptions = 0;
    drawn_ptt_sequence = 0;
    drawn_monitor_sequence = 0;
    lv_obj_add_flag(content_root, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

void ui_view_update(const UiPresentation &presentation) {
    const auto &preferences = presentation.preferences;
    const auto colors = ui_palette(preferences.theme, preferences.contrast);
    const auto ptt_sequence = presentation.ptt_sequence,
               monitor_sequence = presentation.monitor_sequence;
    const bool interrupted = drawn_interruptions != presentation.interruptions ||
                             drawn_ptt_sequence != ptt_sequence ||
                             drawn_monitor_sequence != monitor_sequence;
    const bool motion = presentation.motion && !interrupted;
    const bool restyle =
        !styled || preferences.theme != last_theme || preferences.contrast != last_contrast;
    if (restyle) {
        lv_obj_set_style_bg_color(view_screen, lv_color_hex(colors.background), 0);
        lv_obj_set_style_bg_color(content_root, lv_color_hex(colors.background), 0);
        lv_obj_set_style_bg_color(selection, lv_color_hex(colors.selection), 0);
        lv_obj_set_style_bg_color(stripe, lv_color_hex(colors.accent), 0);
        lv_obj_set_style_bg_color(lock_body, lv_color_hex(colors.accent), 0);
        lv_obj_set_style_border_color(lock_shackle, lv_color_hex(colors.accent), 0);
        for (auto *separator : separators) {
            lv_obj_set_style_bg_color(separator, lv_color_hex(colors.line), 0);
        }
        text_color(title, colors.white);
        text_color(detail, colors.muted);
        text_color(entry, colors.white);
        text_color(entry_help, colors.muted);
        lv_obj_set_style_border_color(entry, lv_color_hex(colors.accent), LV_PART_CURSOR);
        lv_obj_set_style_bg_color(entry_line, lv_color_hex(colors.accent), 0);
        for (unsigned row = 0; row < 4; ++row) {
            text_color(footer[row], row == 0 ? colors.accent : colors.muted);
        }
    }
    const auto &lines = presentation.lines;
    const auto &home = presentation.home;
    const bool show_home = home.visible;
    const bool show_lock = show_home && home.locked;
    lv_obj_t *lock_objects[] = {lock_body, lock_shackle};
    for (auto *object : lock_objects) {
        if (show_lock == lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
            if (show_lock) {
                lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    const auto &system = presentation.status;
    const bool show_system = presentation.system_visible;
    const bool normal = !show_system;
    const bool show_menu = normal && !show_home && presentation.screen == UiScreen::Menu;
    const bool show_status =
        normal && !show_home &&
        (presentation.screen == UiScreen::Status || presentation.screen == UiScreen::CompanionExit);
    const bool show_diagnostic =
        normal && !show_home && presentation.screen == UiScreen::Diagnostics;
    const bool show_hex = show_diagnostic && presentation.diagnostic_editing;
    const bool show_appearance =
        normal && !show_home && presentation.screen == UiScreen::Appearance;
    const bool show_editor = show_hex || (normal && !show_home &&
                                          (presentation.screen == UiScreen::Frequency ||
                                           presentation.screen == UiScreen::Callsign ||
                                           presentation.screen == UiScreen::ChannelNumber ||
                                           presentation.screen == UiScreen::ChannelField ||
                                           presentation.screen == UiScreen::BankName));
    const bool show_list =
        (show_diagnostic && !show_hex) ||
        (normal && !show_home &&
         (presentation.screen == UiScreen::Channels || presentation.screen == UiScreen::Banks ||
          presentation.screen == UiScreen::ChannelBank ||
          presentation.screen == UiScreen::BankMembers ||
          presentation.screen == UiScreen::BankAdd));
    const bool show_form =
        normal && !show_home &&
        (channel_programming(presentation.screen) || bank_programming(presentation.screen) ||
         presentation.screen == UiScreen::VfoStep ||
         presentation.screen == UiScreen::QuickControls ||
         presentation.screen == UiScreen::Backlight ||
         presentation.screen == UiScreen::TransmitLimit) &&
        !show_editor && !show_list;
    const bool show = show_system || show_home || show_menu || show_status || show_appearance ||
                      show_editor || show_list || show_form;
    if (show != content_visible) {
        if (show) {
            lv_obj_clear_flag(content_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_anim_del(selection, selection_y);
            lv_obj_add_flag(content_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (show_system || presentation.screen != drawn_screen || show_home != home_visible) {
        lv_anim_del(selection, selection_y);
    }
    const bool layout_changed = show_system != system_visible || show_home != home_visible ||
                                show_appearance != appearance_visible ||
                                show_editor != editor_visible || show_list != list_visible ||
                                presentation.screen != drawn_screen;
    if (!motion || layout_changed || show_home || !show) {
        lv_anim_del(content_root, page_y);
        // Coordinates can still be old before LVGL resolves a pending style.
        if (lv_obj_get_style_y(content_root, LV_PART_MAIN)) {
            lv_obj_set_y(content_root, 0);
        }
    }
    if (!motion) {
        lv_anim_del(selection, selection_y);
    }
    if (motion && layout_changed && content_visible && !show_home && !show_system) {
        // Reuse the shared page; no second screen or delayed input handover.
        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, content_root);
        lv_anim_set_exec_cb(&animation, page_y);
        lv_anim_set_values(&animation, 3, 0);
        lv_anim_set_time(&animation, 140);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_start(&animation);
    }
    if (layout_changed) {
        lv_obj_t *home_objects[] = {home_strip, home_mode, home_battery, signal[0],
                                    signal[1],  signal[2], signal[3],    signal[4]};
        for (auto *object : home_objects) {
            if (show_home) {
                lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_obj_set_style_text_font(title,
                                   show_home ? &lv_font_montserrat_10 : &lv_font_montserrat_12, 0);
        text_color(title, colors.white);
        lv_obj_set_width(title, show_home ? 56 : 144);
        lv_obj_set_width(detail, show_home ? 136 : 144);
        for (auto *object : theme_rows) {
            if (show_system || show_home || show_menu || show_status || show_appearance ||
                show_list || show_form) {
                lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (show_menu || show_appearance || show_list || show_form) {
            lv_obj_clear_flag(selection, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(selection, LV_OBJ_FLAG_HIDDEN);
        }
        for (unsigned row = 0; row < 4; ++row) {
            lv_obj_set_style_text_font(theme_rows[row],
                                       presentation.screen == UiScreen::TransmitLimit && row == 0
                                           ? &lv_font_montserrat_14
                                           : &lv_font_montserrat_10,
                                       0);
            if (show_list) {
                lv_obj_clear_flag(list_prefix[row], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(list_suffix[row], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(list_prefix[row], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(list_suffix[row], LV_OBJ_FLAG_HIDDEN);
            }
            const bool channels = presentation.screen == UiScreen::Channels ||
                                  presentation.screen == UiScreen::BankMembers ||
                                  presentation.screen == UiScreen::BankAdd;
            lv_obj_set_x(theme_rows[row], channels ? 37 : 12);
            lv_obj_set_width(theme_rows[row], channels ? 98 : show_list ? 123 : 140);
            lv_obj_set_y(theme_rows[row], 41 + 15 * row);
        }
        lv_obj_t *fields[] = {entry, entry_line, entry_help};
        for (auto *object : fields) {
            if (show_editor) {
                lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (show_system) {
        changed_text(title, system.title);
        text_color(title, status_color(system.color, colors));
        changed_text(detail, system.detail);
        text_color(detail, status_color(system.color, colors));
        for (unsigned row = 0; row < 4; ++row) {
            changed_text(theme_rows[row], system.rows[row]);
            text_color(theme_rows[row], row == 0 ? colors.amber : colors.white);
        }
    } else if (show_home) {
        changed_text(title, home.identity);
        text_color(title, colors.muted);
        changed_text(detail, home.name);
        text_color(detail, colors.white);
        changed_text(home_mode, home.mode);
        text_color(home_mode, colors.accent);
        changed_text(home_battery, home.battery);
        text_color(home_battery, status_color(home.battery_color, colors));
        box_color(home_strip, colors.panel);
        const char *values[] = {home.context, home.frequency, home.settings, home.activity};
        const int positions[] = {39, 49, 76, 88};
        for (unsigned row = 0; row < 4; ++row) {
            auto *object = theme_rows[row];
            if (layout_changed) {
                lv_obj_set_pos(object, row == 1 ? 6 : row == 2 ? 12 : 8, positions[row]);
                lv_obj_set_width(object, row == 1 ? 148 : row == 2 ? 136 : 144);
            }
            const auto *font = row == 1 ? &lv_font_montserrat_22 : &lv_font_montserrat_10;
            // Preserve every Hz if a future supported band needs more glyphs.
            if (row == 1 && lv_txt_get_width(home.frequency, strlen(home.frequency), font, 0,
                                             LV_TEXT_FLAG_NONE) > 148) {
                font = &lv_font_montserrat_14;
            }
            if (lv_obj_get_style_text_font(object, 0) != font) {
                lv_obj_set_style_text_font(object, font, 0);
            }
            changed_text(object, values[row]);
            text_color(object, row == 1   ? (home.transmitting ? colors.amber : colors.white)
                               : row == 3 ? status_color(home.status, colors)
                               : row == 0 ? status_color(home.context_color, colors)
                                          : colors.muted);
        }
        for (unsigned bar = 0; bar < 5; ++bar) {
            box_color(signal[bar], bar < home.bars ? colors.accent : colors.line);
        }
    } else if (show_menu) {
        const auto &page = presentation.list;
        changed_text(title, page.title);
        changed_text(detail, lines[6][0] ? lines[6] : page.detail);
        text_color(detail, presentation.error ? colors.red : colors.muted);
        const int y = 39 + 15 * (page.cursor % 4);
        if (layout_changed || !motion || page.cursor != drawn_list_cursor) {
            lv_anim_del(selection, selection_y);
            if (!layout_changed && motion) {
                lv_anim_t animation;
                lv_anim_init(&animation);
                lv_anim_set_var(&animation, selection);
                lv_anim_set_exec_cb(&animation, selection_y);
                lv_anim_set_values(&animation, lv_obj_get_y(selection), y);
                lv_anim_set_time(&animation, 140);
                lv_anim_start(&animation);
            } else {
                lv_obj_set_y(selection, y);
            }
        }
        for (unsigned row = 0; row < 4; ++row) {
            const bool present = page.cursor / 4 * 4 + row < page.count;
            if (present == lv_obj_has_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN)) {
                if (present) {
                    lv_obj_clear_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
                }
            }
            changed_text(theme_rows[row], present ? page.rows[row].name : "");
            text_color(theme_rows[row], colors.white);
        }
        drawn_list_cursor = page.cursor;
    } else if (show_status) {
        const auto &page = presentation.status;
        changed_text(title, page.title);
        changed_text(detail, page.detail);
        text_color(detail, status_color(page.color, colors));
        for (unsigned row = 0; row < 4; ++row) {
            changed_text(theme_rows[row], page.rows[row]);
            text_color(theme_rows[row], colors.white);
        }
    } else if (show_appearance) {
        changed_text(title, "APPEARANCE");
        if (!appearance_visible || preferences.theme != last_theme || !motion) {
            lv_anim_del(selection, selection_y);
            const int y = 39 + 15 * (static_cast<unsigned>(preferences.theme) % 4);
            if (appearance_visible && motion && preferences.theme != last_theme) {
                lv_anim_t animation;
                lv_anim_init(&animation);
                lv_anim_set_var(&animation, selection);
                lv_anim_set_exec_cb(&animation, selection_y);
                lv_anim_set_values(&animation, lv_obj_get_y(selection), y);
                lv_anim_set_time(&animation, 140);
                lv_anim_start(&animation);
            } else {
                lv_obj_set_y(selection, y);
            }
        }
        const unsigned first = static_cast<unsigned>(preferences.theme) / 4 * 4;
        for (unsigned row = 0; row < 4; ++row) {
            const bool present = first + row < ThemeCount;
            if (present) {
                lv_obj_clear_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
            }
            text_color(theme_rows[row], colors.white);
            changed_text(
                theme_rows[row],
                present ? ui_palette(static_cast<Theme>(first + row), Contrast::Normal).name : "");
        }
        changed_text(detail, lines[6][0] ? lines[6] : lines[1]);
        text_color(detail, presentation.error ? colors.red : colors.muted);
    } else if (show_list) {
        const auto &page = presentation.list;
        changed_text(title, show_diagnostic                                ? page.title
                            : bank_programming(presentation.screen)        ? page.title
                            : presentation.screen == UiScreen::ChannelBank ? "ADD TO BANK"
                            : presentation.screen == UiScreen::Banks       ? "BANKS"
                                                                           : "CHANNELS");
        changed_text(detail, show_diagnostic ? page.detail
                             : lines[6][0]   ? lines[6]
                             : page.ready    ? page.detail
                                             : "Refreshing...");
        text_color(detail, presentation.error ? colors.red : colors.muted);
        if (!page.ready || !page.count) {
            lv_anim_del(selection, selection_y);
        }
        const bool selected_visible = page.ready && page.count;
        if (selected_visible == lv_obj_has_flag(selection, LV_OBJ_FLAG_HIDDEN)) {
            if (selected_visible) {
                lv_obj_clear_flag(selection, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(selection, LV_OBJ_FLAG_HIDDEN);
            }
        }
        const int y = 39 + 15 * (page.cursor % 4);
        if (presentation.screen != drawn_screen || !motion || page.cursor != drawn_list_cursor) {
            lv_anim_del(selection, selection_y);
            if (presentation.screen == drawn_screen && motion && page.ready && page.count) {
                lv_anim_t animation;
                lv_anim_init(&animation);
                lv_anim_set_var(&animation, selection);
                lv_anim_set_exec_cb(&animation, selection_y);
                lv_anim_set_values(&animation, lv_obj_get_y(selection), y);
                lv_anim_set_time(&animation, 140);
                lv_anim_start(&animation);
            } else {
                lv_obj_set_y(selection, y);
            }
        }
        for (unsigned row = 0; row < 4; ++row) {
            const bool present = page.ready && page.cursor / 4 * 4 + row < page.count;
            const bool visible = present || (page.ready && !page.count && row == 0);
            if (visible == lv_obj_has_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN)) {
                if (visible) {
                    lv_obj_clear_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(theme_rows[row], LV_OBJ_FLAG_HIDDEN);
                }
            }
            changed_text(theme_rows[row], present ? page.rows[row].name
                                          : page.ready && !page.count && row == 0
                                              ? presentation.screen == UiScreen::BankMembers
                                                    ? "No members yet"
                                                    : "No channels yet"
                                              : "");
            text_color(theme_rows[row], colors.white);
            changed_text(list_prefix[row], present ? page.rows[row].prefix : "");
            changed_text(list_suffix[row], present ? page.rows[row].suffix : "");
            text_color(list_prefix[row], colors.muted);
            text_color(list_suffix[row],
                       presentation.screen != UiScreen::Channels ? colors.muted : colors.accent);
        }
        if (show_diagnostic) {
            drawn_list_cursor = page.cursor;
        }
    } else if (show_hex) {
        const auto &page = presentation.list;
        changed_text(title, page.title);
        changed_text(detail, page.detail);
        text_color(detail, presentation.error ? colors.red : colors.muted);
        changed_text(entry_help, page.rows[0].name);
        if (layout_changed) {
            lv_obj_set_style_text_font(entry, &lv_font_montserrat_22, 0);
            lv_obj_set_style_pad_right(lv_textarea_get_label(entry),
                                       lv_font_get_line_height(&lv_font_montserrat_22), 0);
            lv_obj_scroll_to(entry, 0, 0, LV_ANIM_OFF);
        }
        const char *value = presentation.diagnostic_text;
        if (strcmp(lv_textarea_get_text(entry), value)) {
            lv_textarea_set_text(entry, value);
        }
        if (lv_textarea_get_cursor_pos(entry) != strlen(value)) {
            lv_textarea_set_cursor_pos(entry, strlen(value));
        }
    } else if (show_editor) {
        const auto &draft = presentation.text;
        changed_text(title, presentation.screen == UiScreen::ChannelField ||
                                    presentation.screen == UiScreen::BankName
                                ? lines[0]
                            : presentation.screen == UiScreen::Frequency     ? "VFO FREQUENCY"
                            : presentation.screen == UiScreen::ChannelNumber ? "SELECT CHANNEL"
                                                                             : "LOCAL CALLSIGN");
        changed_text(detail, lines[6][0] ? lines[6] : lines[1]);
        text_color(detail, presentation.error ? colors.red : colors.muted);
        changed_text(entry_help, presentation.screen == UiScreen::ChannelField ||
                                         presentation.screen == UiScreen::BankName
                                     ? lines[4]
                                 : presentation.screen != UiScreen::Callsign
                                     ? "Up/Down cursor"
                                     : "Up/Down cursor / # Next");
        if (presentation.screen != drawn_screen) {
            const auto *font =
                draft.kind == TextKind::Frequency || draft.kind == TextKind::ChannelNumber
                    ? &lv_font_montserrat_22
                    : &lv_font_montserrat_14;
            lv_obj_set_style_text_font(entry, font, 0);
            // Native bounded scrolling stops at the label edge. Include space
            // for the end-of-text caret in the label's scrollable geometry.
            lv_obj_set_style_pad_right(lv_textarea_get_label(entry), lv_font_get_line_height(font),
                                       0);
            lv_obj_scroll_to(entry, 0, 0, LV_ANIM_OFF);
        }
        if (strcmp(lv_textarea_get_text(entry), draft.value)) {
            lv_textarea_set_text(entry, draft.value);
        }
        const size_t cursor = draft.pending ? draft.cursor - 1 : draft.cursor;
        lv_textarea_set_cursor_pos(entry, cursor);
        // Keep the caret visible immediately; the pinned textarea otherwise
        // animates automatic scrolling independently of the Motion setting.
        lv_obj_update_layout(entry);
        lv_point_t caret;
        lv_label_get_letter_pos(lv_textarea_get_label(entry), cursor, &caret);
        const int width = lv_obj_get_content_width(entry);
        const int caret_width = lv_font_get_line_height(lv_obj_get_style_text_font(entry, 0));
        int scroll = lv_obj_get_scroll_x(entry);
        if (caret.x < scroll) {
            scroll = caret.x;
        } else if (caret.x + caret_width > scroll + width) {
            scroll = caret.x + caret_width - width;
        }
        lv_obj_scroll_to(entry, scroll, 0, LV_ANIM_OFF);
    } else if (show_form) {
        changed_text(title, lines[0]);
        changed_text(detail, lines[6][0] ? lines[6] : lines[1]);
        text_color(detail, presentation.error ? colors.red : colors.muted);
        const bool selectable =
            presentation.screen == UiScreen::ChannelEditor ||
            presentation.screen == UiScreen::ChannelTone ||
            presentation.screen == UiScreen::BankEditor ||
            presentation.screen == UiScreen::BankActions ||
            presentation.screen == UiScreen::Backlight ||
            (presentation.screen == UiScreen::QuickControls && presentation.quick_available);
        if (selectable) {
            lv_obj_clear_flag(selection, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_anim_del(selection, selection_y);
            lv_obj_add_flag(selection, LV_OBJ_FLAG_HIDDEN);
        }
        const auto cursor = presentation.form_cursor;
        const int y = 39 + 15 * cursor;
        if (presentation.screen != drawn_screen || !motion || cursor != drawn_form_cursor) {
            lv_anim_del(selection, selection_y);
            if (presentation.screen == drawn_screen && selectable && motion) {
                lv_anim_t animation;
                lv_anim_init(&animation);
                lv_anim_set_var(&animation, selection);
                lv_anim_set_exec_cb(&animation, selection_y);
                lv_anim_set_values(&animation, lv_obj_get_y(selection), y);
                lv_anim_set_time(&animation, 140);
                lv_anim_start(&animation);
            } else {
                lv_obj_set_y(selection, y);
            }
        }
        for (unsigned row = 0; row < 4; ++row) {
            const bool fields = selectable || presentation.screen == UiScreen::ChannelReview ||
                                presentation.screen == UiScreen::QuickControls;
            changed_text(theme_rows[row], lines[row + 2] + (fields ? 2 : 0));
            text_color(theme_rows[row], colors.white);
        }
    }
    for (unsigned row = 0; row < 4; ++row) {
        changed_text(footer[row], presentation.actions[row]);
    }
    appearance_visible = show_appearance;
    editor_visible = show_editor;
    list_visible = show_list;
    content_visible = show;
    home_visible = show_home;
    system_visible = show_system;
    drawn_screen = presentation.screen;
    drawn_interruptions = presentation.interruptions;
    drawn_ptt_sequence = ptt_sequence;
    drawn_monitor_sequence = monitor_sequence;
    if (!show_menu && !show_diagnostic) {
        drawn_list_cursor = presentation.list.cursor;
    }
    drawn_form_cursor = presentation.form_cursor;
    last_theme = preferences.theme;
    last_contrast = preferences.contrast;
    styled = true;
}
} // namespace ht
