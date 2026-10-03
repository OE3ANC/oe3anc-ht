// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/radio.hpp>
#include <ht/codeplug.hpp>
#include <ht/ui_theme.hpp>
#include <ht/ui_text.hpp>
#include <ht/ui_input.hpp>
#include <ht/ui_view.hpp>
#include <lvgl.h>
#include <errno.h>

namespace ht {
// Loss-independent Star level and gesture cancellation. Producers publish before
// queuing front keys; only the UI owner interprets this as a keypad-lock gesture.
struct KeypadGesture {
    uint32_t press_sequence = 0, cancelled_press = 0;
    int64_t pressed_at = 0;
    bool pressed = false;
};

// A simultaneous matrix navigation key cancels this newly published press too.
void ui_keypad_star(bool pressed, int64_t now_ms, bool cancelled = false);
void ui_keypad_remote_star(bool pressed, int64_t now_ms, int64_t deadline_ms);
void ui_keypad_cancel_gesture();
KeypadGesture ui_keypad_gesture();

struct UiHome : UiHomeContent {
    const char *mode = "FM";
    const char *actions[4] = {};
};

// The UI owns drafts only. Commands go through the radio controller; snapshots
// provide the configuration displayed after completion. One UI thread owns this.
class UiModel {
  public:
    void sync(const RadioState &state);
    // Target producers also publish Star levels/cancellation outside the queue.
    void input(const UiInput &input, bool external_pending = false);

    bool cps_available() const {
        return !command_pending() && !appearance_pending() &&
               (screen_ == UiScreen::Home || screen_ == UiScreen::Menu ||
                screen_ == UiScreen::Status || screen_ == UiScreen::Channels ||
                screen_ == UiScreen::Banks);
    }

    // Call after draining inputs, so queued producer timestamps precede expiry.
    void advance(int64_t now_ms);

    const TextEditor &text_editor() const {
        return text_editor_;
    }

    bool command_pending() const {
        return pending_ != 0 || recall_pending_ != 0 || edit_pending_ != 0 || bank_pending_ != 0 ||
               step_pending_ != 0 || light_pending_ != 0;
    }

    bool recall_pending() const {
        return recall_pending_ != 0;
    }

    const UiListPage &list_page() const {
        return list_;
    }

    UiScreen list_return_screen() const {
        return list_return_;
    }

    void lines(char (&text)[8][32]) const;
    void home(UiHome &view) const;
    void menu_page(UiListPage &page) const;
    void menu_actions(const char *(&actions)[4]) const;
    void status(UiStatus &view) const;
    bool system_state(UiStatus &view) const; // Fills only for fault/inactive priority.
    void diagnostic_page(UiListPage &page) const;
    void diagnostic_actions(const char *(&actions)[4]) const;

    bool diagnostic_editing() const {
        return editing_;
    }

    const char *diagnostic_text() const {
        return draft_;
    }

    UiScreen screen() const {
        return screen_;
    }

    int error() const {
        return error_;
    }

    bool motion_allowed() const {
        return preferences().animations && state_.power_active && !state_.fault &&
               (state_.phase == RadioPhase::Receiving || state_.phase == RadioPhase::Diagnostics) &&
               !state_.rx_active && !state_.monitor_active && !state_.tx_warning &&
               !state_.tx_timed_out && !radio_ptt_requested() && !radio_monitor_requested() &&
               !state_.ptt_error && !error_ && (!storage_error_ || storage_first_run_);
    }

    uint32_t motion_interruptions() const {
        return motion_interruptions_;
    } // UI-only PTT/focus cancellation.

    const UiPreferences &preferences() const {
        return screen_ == UiScreen::Appearance ? appearance_draft_ : applied_ui_;
    }

    bool appearance_pending() const {
        return appearance_pending_ != 0;
    }

    uint8_t form_cursor() const {
        return screen_ == UiScreen::Backlight       ? light_cursor_
               : screen_ == UiScreen::QuickControls ? quick_cursor()
               : bank_programming(screen_)          ? bank_form_cursor_
               : screen_ == UiScreen::ChannelTone   ? tone_cursor_
                                                    : edit_cursor_;
    }

    uint8_t backlight_percent() const;

    bool dimmed() const {
        return dimmed_;
    }

    bool backlight_pending() const {
        return light_pending_ != 0;
    }

    bool keypad_locked() const {
        return keypad_locked_;
    }

    bool quick_available() const;
    void quick_actions(const char *(&actions)[4]) const;
    void channel_actions(const char *(&actions)[4]) const;
    void bank_actions(const char *(&actions)[4]) const;

    // Preserve origin: ENOENT on save is an error, only a missing load is defaults.
    void storage_status(int load_error, int save_error, bool pending) {
        storage_error_ = save_error ? save_error : load_error;
        storage_first_run_ = !save_error && load_error == -ENOENT;
        storage_pending_ = pending;
    }

  private:
    RadioState state_;
    uint32_t motion_interruptions_ = 0;
    uint8_t signal_bars_ = 0;
    int64_t signal_step_ms_ = -1;
    void advance_signal(int64_t now_ms);
    UiScreen screen_ = UiScreen::Home;
    uint8_t selected_ = 0;
    uint8_t status_page_ = 0;
    char draft_[5] = {}; // At most four diagnostic hexadecimal digits.
    uint32_t diagnostic_ptt_sequence_ = 0;
    TextEditor text_editor_;
    uint32_t text_ptt_sequence_ = 0;
    uint32_t vfo_step_hz_ = 12500, step_draft_ = 12500;
    uint32_t step_revision_ = 0, step_pending_ = 0, step_ptt_sequence_ = 0;
    void tune_vfo(int direction);
    void open_step();
    void cancel_step(bool interruption = false);
    void step_input(const UiInput &input);
    uint8_t quick_gain_ = 0, quick_squelch_ = 0;
    bool quick_gain_field_ = false;
    uint32_t quick_generation_ = 0, quick_revision_ = 0, quick_ptt_sequence_ = 0;
    Selection quick_selection_;
    UiScreen quick_return_ = UiScreen::Home;
    uint8_t quick_cursor() const;
    void open_quick(bool gain = false);
    void cancel_quick(bool interruption = false);
    void quick_input(const UiInput &input);
    void quick_lines(char (&text)[8][32]) const;
    uint16_t limit_draft_ = 180;
    uint32_t limit_generation_ = 0, limit_revision_ = 0, limit_ptt_sequence_ = 0;
    Selection limit_selection_;
    void open_limit();
    void cancel_limit(bool interruption = false);
    void limit_input(const UiInput &input);
    void limit_lines(char (&text)[8][32]) const;
    UiPreferences light_draft_;
    uint32_t light_revision_ = 0, light_pending_ = 0, light_ptt_sequence_ = 0;
    uint32_t wake_ptt_sequence_ = 0, wake_monitor_sequence_ = 0;
    uint8_t light_cursor_ = 0;
    bool dimmed_ = false;
    int64_t light_activity_ms_ = 0;
    void wake_light(int64_t now);
    bool light_input(const UiInput &input);
    void advance_light(int64_t now);
    void open_light();
    void cancel_light(bool interruption = false);
    void backlight_input(const UiInput &input);
    void backlight_lines(char (&text)[8][32]) const;
    bool keypad_locked_ = false;
    uint32_t lock_press_sequence_ = 0, lock_ptt_sequence_ = 0;
    int64_t lock_since_ = -1;
    void cancel_lock();
    void advance_lock(int64_t now_ms);
    bool lock_input(const UiInput &input, bool consumed_wake);
    void cancel_text();
    void text_input(const UiInput &input);
    UiListPage list_;
    uint32_t browse_bank_ = 0, browse_channel_ = 0, browse_revision_ = 0;
    uint32_t recall_pending_ = 0;
    uint16_t browse_cursor_ = 0; // Staged cursor; published only with its rows.
    UiScreen list_return_ = UiScreen::Menu, number_return_ = UiScreen::Home;
    void refresh_list();
    void open_channels(UiScreen return_screen);
    void browse(const UiInput &input);
    void recall(const Selection &selection, uint32_t revision = 0);
    void switch_operating();
    void step_memory(int direction);
    void open_number(UiScreen return_screen);
    void apply_number();
    Channel channel_draft_;
    Tone tone_draft_;
    uint32_t edit_revision_ = 0, edit_pending_ = 0, edit_ptt_sequence_ = 0, edit_bank_ = 0,
             replacement_id_ = 0;
    uint16_t replacement_number_ = 0;
    char replacement_name_[25] = {};
    UiScreen edit_return_ = UiScreen::Home;
    uint8_t edit_cursor_ = 0, edit_page_ = 0, review_page_ = 0, tone_cursor_ = 0;
    int edit_save_error_ = 0;
    bool edit_save_pending_ = false;
    bool deleting_ = false, duplicate_draft_ = false;
    enum class ChannelField : uint8_t { Name, Number, Rx, Tx, Station, ToneValue };
    ChannelField channel_field_ = ChannelField::Name;
    void open_channel(uint32_t id, UiScreen return_screen);
    void cancel_channel(bool interruption = false);
    void channel_input(const UiInput &input);
    void channel_field(const UiInput &input);
    void begin_channel_field(ChannelField field);
    void review_channel();
    void save_channel();
    void channel_lines(char (&text)[8][32]) const;
    Bank bank_draft_;
    char bank_original_name_[25] = {};
    uint32_t bank_revision_ = 0, bank_pending_ = 0, bank_ptt_sequence_ = 0;
    uint16_t bank_cursor_ = 0,
             bank_add_cursor_ = 0; // Desired cursors; list_ is published separately.
    uint8_t bank_form_cursor_ = 0;
    bool bank_deleting_ = false;
    void open_bank(uint32_t id);
    void cancel_bank(bool interruption = false);
    void bank_input(const UiInput &input);
    void refresh_bank_list();
    void save_bank();
    void bank_lines(char (&text)[8][32]) const;
    size_t length_ = 0;
    uint8_t letter_ = 0;
    uint8_t address_ = 0;
    uint16_t value_ = 0;
    bool editing_ = false;
    uint32_t next_id_ = 1;
    uint32_t pending_ = 0;
    CommandKind pending_kind_ = CommandKind::Configure;
    int error_ = 0;
    int storage_error_ = 0;
    bool storage_pending_ = false;
    bool storage_first_run_ = false;
    UiPreferences applied_ui_, appearance_draft_;
    uint32_t appearance_revision_ = 0, appearance_pending_ = 0, appearance_ptt_sequence_ = 0;
    void cancel_appearance();
    void appearance(const UiInput &input);
    void submit(RadioCommand command);
    void cancel_diagnostic_edit();
    void menu(int direction);
    void diagnostics(const UiInput &input);
};

// LVGL view: caller supplies the target display's screen. Fixed bounded objects.
void ui_capture_presentation(const UiModel &model, UiPresentation &presentation);
void ui_view_update(const UiModel &model);
void ui_run();

// Target UI adapter, called only by the LVGL thread. Errors are visible in logs.
int ui_backend_start(uint8_t brightness_percent = 100);
bool ui_backend_has_backlight();
int ui_backend_backlight(uint8_t percent);
void ui_backend_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels);
bool ui_backend_input(UiInput &input);
void ui_backend_service();
} // namespace ht
