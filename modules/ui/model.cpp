// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#include "menu.hpp"
#include <errno.h>
#include <ht/backend.hpp>
#include <ht/ui.hpp>
#ifdef CONFIG_HT_CODEC2
#include <ht/voice.hpp>
#endif
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
// Standard tone table from reference OpenRTX core/cps.c, with 0 for disabled.
static const uint16_t tones[] = {0,    670,  693,  719,  744,  770,  797,  825,  854,  885,  915,
                                 948,  974,  1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273, 1318,
                                 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738,
                                 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995, 2035, 2065, 2107,
                                 2181, 2257, 2291, 2336, 2418, 2503, 2541};

void UiModel::begin_startup(int64_t now) {
    if (applied_ui_.startup_animation && state_.power_active && !state_.fault &&
        !radio_ptt_requested()) {
        startup_started_ = now;
        startup_position_ = 0;
        startup_ptt_ = radio_ptt_press_sequence();
    }
}

void UiModel::stop_startup() {
    startup_started_ = -1;
    startup_sound_ = false;
    radio_startup_chime(false);
}

void UiModel::sync(const RadioState &state) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    settings_ui_preferences(applied_ui_);
    vfo_step_hz_ = settings_vfo_step();
    const auto storage = settings_status();
    edit_save_error_ = storage.save_error;
    edit_save_pending_ = storage.pending;
    if (appearance_pending_ && storage.operation_id == appearance_pending_ &&
        !storage.operation_pending) {
        appearance_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_ && screen_ == UiScreen::Appearance) {
            awaiting_preferences_save_ = true;
        }
    }
    if (recall_pending_ && storage.operation_id == recall_pending_ && !storage.operation_pending) {
        recall_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_) {
            screen_ = UiScreen::Home;
            text_editor_ = {};
        }
    }
    if (edit_pending_ && storage.operation_id == edit_pending_ && !storage.operation_pending) {
        edit_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_ && channel_programming(screen_)) {
            if (!deleting_) {
                channel_draft_.id = storage.operation_object_id;
                browse_channel_ = channel_draft_.id;
            }
            awaiting_channel_save_ = true;
            edit_revision_ = storage.revision;
            screen_ = deleting_ ? UiScreen::ChannelSaved : UiScreen::ChannelEditor;
            text_editor_ = {};
        }
    }
    if (awaiting_channel_save_) {
        if (storage.save_error) {
            error_ = storage.save_error;
            awaiting_channel_save_ = deleting_;
        } else if (!storage.pending) {
            awaiting_channel_save_ = false;
            error_ = 0;
            strcpy(notice_, deleting_ ? "Channel deleted" : "Channel saved");
            notice_until_ = k_uptime_get() + 1500;
            cancel_channel();
        }
    }
    if (step_pending_ && storage.operation_id == step_pending_ && !storage.operation_pending) {
        step_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_) {
            vfo_step_hz_ = settings_vfo_step();
        }
        if (!error_ && screen_ == UiScreen::VfoStep) {
            awaiting_preferences_save_ = true;
        }
    }
    if (bank_pending_ && storage.operation_id == bank_pending_ && !storage.operation_pending) {
        bank_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_ && bank_programming(screen_)) {
            if (!bank_deleting_) {
                bank_draft_.id = storage.operation_object_id;
            }
            awaiting_bank_save_ = true;
            bank_revision_ = storage.revision;
            screen_ = bank_deleting_ ? UiScreen::BankSaved : UiScreen::BankEditor;
            text_editor_ = {};
        }
    }
    if (awaiting_bank_save_) {
        if (storage.save_error) {
            error_ = storage.save_error;
            awaiting_bank_save_ = bank_deleting_;
        } else if (!storage.pending) {
            awaiting_bank_save_ = false;
            error_ = 0;
            strcpy(notice_, bank_deleting_ ? "Bank deleted" : "Bank saved");
            notice_until_ = k_uptime_get() + 1500;
            cancel_bank();
        }
    }
    if (light_pending_ && storage.operation_id == light_pending_ && !storage.operation_pending) {
        light_pending_ = 0;
        error_ = storage.operation_error;
        if (!error_) {
            awaiting_preferences_save_ = true;
        }
    }
    if (awaiting_preferences_save_) {
        if (storage.save_error) {
            awaiting_preferences_save_ = false;
            appearance_revision_ = light_revision_ = step_revision_ = storage.revision;
            error_ = storage.save_error;
        } else if (!storage.pending) {
            awaiting_preferences_save_ = false;
            settings_ui_preferences(applied_ui_);
            cancel_appearance();
            cancel_light();
            cancel_step();
            wake_light(k_uptime_get());
        }
    }
#endif
    if (screen_ == UiScreen::TransmitLimit && (!state.power_active || state.fault)) {
        cancel_limit(true);
    }
    if (screen_ == UiScreen::TransmitLimit && !pending_ &&
        (state.generation != limit_generation_ || state.configuration_revision != limit_revision_ ||
         !same_selection(state.selection, limit_selection_))) {
        cancel_limit(true);
        error_ = -ESTALE;
    }
    if (screen_ == UiScreen::Backlight && (!state.power_active || state.fault)) {
        cancel_light(true);
    }
    if (screen_ == UiScreen::QuickControls && (!state.power_active || state.fault)) {
        cancel_quick(true);
    }
    if (screen_ == UiScreen::QuickControls && !pending_ &&
        (state.generation != quick_generation_ || state.configuration_revision != quick_revision_ ||
         !same_selection(state.selection, quick_selection_))) {
        cancel_quick(true);
        error_ = -ESTALE;
    }
    if (screen_ == UiScreen::VfoStep && (!state.power_active || state.fault)) {
        cancel_step(true);
    }
    if (bank_programming(screen_) && (!state.power_active || state.fault)) {
        cancel_bank(true);
    }
    if (channel_programming(screen_) && (!state.power_active || state.fault)) {
        cancel_channel(true);
    }
    if (screen_ == UiScreen::Appearance && (!state.power_active || state.fault)) {
        cancel_appearance();
    }
    if ((screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
         screen_ == UiScreen::ChannelNumber) &&
        (!state.power_active || state.fault)) {
        cancel_text();
    }
    if (screen_ == UiScreen::Diagnostics && editing_ &&
        (!state.power_active || state.fault || state.phase != RadioPhase::Diagnostics ||
         radio_ptt_press_sequence() != diagnostic_ptt_sequence_)) {
        cancel_diagnostic_edit();
    }
    if (state.generation != state_.generation) {
        discard_open_ = field_editing_ = false;
        awaiting_channel_save_ = awaiting_bank_save_ = awaiting_preferences_save_ = false;
        keypad_locked_ = false;
        cancel_lock();
        wake_light(k_uptime_get());
        wake_ptt_sequence_ = radio_ptt_press_sequence();
        wake_monitor_sequence_ = radio_monitor_press_sequence();
        screen_ = UiScreen::Home;
        pending_ = 0;
        appearance_pending_ = recall_pending_ = edit_pending_ = bank_pending_ = step_pending_ =
            light_pending_ = 0;
        browse_revision_ = 0;
        list_ = {};
        error_ = 0;
        editing_ = false;
        address_ = 0;
        value_ = 0;
        length_ = 0;
        draft_[0] = 0;
        text_editor_ = {};
    }
    state_ = state;
    if (!state.power_active || state.phase != RadioPhase::Receiving || !state.rx_active ||
        state.fault) {
        signal_bars_ = 0;
        signal_step_ms_ = -1;
    }
    if (!state.power_active || state.fault || state.phase != RadioPhase::Receiving ||
        radio_ptt_press_sequence() != lock_ptt_sequence_) {
        cancel_lock();
    }
    if ((screen_ == UiScreen::Channels || screen_ == UiScreen::Banks) &&
        (!state.power_active || state.fault || state.phase == RadioPhase::Diagnostics)) {
        screen_ = UiScreen::Home;
    }
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (!list_.ready || browse_revision_ != storage.revision) {
        refresh_list();
    }
    if ((screen_ == UiScreen::BankMembers || screen_ == UiScreen::BankAdd) &&
        (!list_.ready || storage.revision != bank_revision_)) {
        refresh_bank_list();
    }
#endif
    if (screen_ == UiScreen::Menu && !menu_available(selected_, state_.config.mode)) {
        selected_ = ModeItem;
    }
    if (pending_ && state.command_id == pending_) {
        pending_ = 0;
        error_ = state.command_error;
        if (!error_) {
            if (screen_ == UiScreen::ChannelEditor && editing_vfo_) {
                strcpy(notice_, "VFO applied");
                notice_until_ = k_uptime_get() + 1500;
                cancel_channel();
            }
            if (screen_ == UiScreen::QuickControls) {
                cancel_quick();
            }
            if (screen_ == UiScreen::TransmitLimit) {
                cancel_limit();
            }
            if (screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
                screen_ == UiScreen::ChannelNumber) {
                screen_ = UiScreen::Home;
            } else if (state.phase == RadioPhase::Diagnostics && screen_ != UiScreen::Diagnostics) {
                screen_ = UiScreen::Diagnostics;
                selected_ = 0;
                editing_ = false;
            } else if (screen_ == UiScreen::Diagnostics && state.phase == RadioPhase::Receiving) {
                screen_ = UiScreen::Menu;
                selected_ = DiagnosticsItem;
                editing_ = false;
            }
            if (pending_kind_ == CommandKind::ReadRegister) {
                value_ = state.register_value;
            }
        }
    }
}

bool UiModel::editor_dirty() const {
    if (screen_ == UiScreen::ChannelEditor) {
        const auto &a = channel_draft_.configuration;
        const auto &b = original_channel_.configuration;
        return channel_draft_.id != original_channel_.id ||
               channel_draft_.number != original_channel_.number ||
               strcmp(channel_draft_.name, original_channel_.name) ||
               a.rx_frequency_hz != b.rx_frequency_hz || a.tx_frequency_hz != b.tx_frequency_hz ||
               a.tx_inhibit != b.tx_inhibit || a.power_mw != b.power_mw || a.mode != b.mode ||
               a.bandwidth != b.bandwidth || a.squelch != b.squelch ||
               !same_tone(a.rx_tone, b.rx_tone) || !same_tone(a.tx_tone, b.tx_tone) ||
               a.m17.destination != b.m17.destination || a.m17.can != b.m17.can ||
               a.m17.rx_can_check != b.m17.rx_can_check || strcmp(a.m17.callsign, b.m17.callsign) ||
               edit_bank_ != original_bank_;
    }
    if (screen_ == UiScreen::Appearance) {
        return (appearance_draft_.theme != preference_original_.theme ||
                appearance_draft_.contrast != preference_original_.contrast ||
                appearance_draft_.animations != preference_original_.animations ||
                appearance_draft_.startup_animation != preference_original_.startup_animation ||
                appearance_draft_.brightness_percent != preference_original_.brightness_percent ||
                appearance_draft_.idle_s != preference_original_.idle_s ||
                appearance_draft_.dim_percent != preference_original_.dim_percent);
    }
    if (screen_ == UiScreen::Backlight) {
        return (light_draft_.theme != preference_original_.theme ||
                light_draft_.contrast != preference_original_.contrast ||
                light_draft_.animations != preference_original_.animations ||
                light_draft_.startup_animation != preference_original_.startup_animation ||
                light_draft_.brightness_percent != preference_original_.brightness_percent ||
                light_draft_.idle_s != preference_original_.idle_s ||
                light_draft_.dim_percent != preference_original_.dim_percent);
    }
    if (screen_ == UiScreen::VfoStep) {
        return step_draft_ != vfo_step_hz_;
    }
    if (screen_ == UiScreen::TransmitLimit) {
        return limit_draft_ != state_.config.transmit_limit_s;
    }
    if (screen_ == UiScreen::BankEditor) {
        return bank_dirty_;
    }
    return false;
}

void UiModel::save_editor() {
    if (screen_ == UiScreen::ChannelEditor) {
        review_channel();
    } else if (screen_ == UiScreen::BankEditor) {
        bank_deleting_ = false;
        save_bank();
    } else if (screen_ == UiScreen::Appearance) {
        appearance_cursor_ = 4;
        appearance({UiKey::Enter});
    } else if (screen_ == UiScreen::Backlight) {
        light_cursor_ = 3;
        backlight_input({UiKey::Enter});
    } else if (screen_ == UiScreen::VfoStep) {
        step_input({UiKey::Enter});
    } else if (screen_ == UiScreen::TransmitLimit) {
        limit_input({UiKey::Enter});
    }
}

void UiModel::discard_input(const UiInput &input) {
    if (input.key == UiKey::Back) {
        discard_open_ = false;
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        discard_cursor_ = (discard_cursor_ + (input.key == UiKey::Down ? 1 : 2)) % 3;
    } else if (input.key == UiKey::Enter) {
        discard_open_ = false;
        if (discard_cursor_ == 0) {
            save_editor();
        } else if (discard_cursor_ == 1) {
            cancel_channel();
            cancel_bank();
            cancel_appearance();
            cancel_light();
            cancel_step();
            cancel_limit();
        }
    }
}

void UiModel::submit(RadioCommand command) {
    if (pending_) {
        error_ = -EBUSY;
        return;
    }
    command.id = next_id_++;
    error_ = radio_submit(command);
    if (!error_) {
        pending_ = command.id;
        pending_kind_ = command.kind;
    }
}

void UiModel::menu(int direction) {
#ifdef CONFIG_HT_MODEM
    if (selected_ == ModemEnabledItem && modem_enabled()) {
        modem_disable(); // Independent exit, including while a packet owns TX.
        return;
    }
#endif
    if (selected_ != StatusItem &&
        (state_.phase != RadioPhase::Receiving || radio_ptt_requested())) {
        error_ = -EBUSY;
        return;
    }
    RadioCommand command;
    command.config = state_.config;
    auto &config = command.config;
    switch (selected_) {
    case ModemItem:
        menu_group_ = 3;
        selected_ = ModemEnabledItem;
        return;
    case ModemCountersItem:
    case ModemTxCountItem:
    case ModemDropsItem:
    case ModemQueueItem:
        return;
#ifdef CONFIG_HT_MODEM
    case ModemEnabledItem:
        command.kind = CommandKind::Modem;
        command.modem_enabled = true;
        command.modem_settings = modem_status().settings;
        command.modem_epoch = modem_epoch();
        command.modem_ptt_sequence = radio_ptt_press_sequence();
        command.expected_generation = state_.generation;
        command.expected_revision = state_.configuration_revision;
        submit(command);
        return;
#endif
    case LnaPolarityItem:
        command.kind = CommandKind::LnaPolarity;
        config.lna_inverted = !config.lna_inverted;
        command.expected_generation = state_.generation;
        command.expected_revision = state_.configuration_revision;
        command.selection = state_.selection;
        submit(command);
        return;
    case FmCtcssLevelItem: {
        const int next = int(config.fm_ctcss_level) + direction;
        if (next < 0 || next > 127) {
            return;
        }
        command.kind = CommandKind::FmCtcssLevel;
        config.fm_ctcss_level = next;
        command.expected_generation = state_.generation;
        command.expected_revision = state_.configuration_revision;
        command.selection = state_.selection;
        submit(command);
        return;
    }
    case FmWeakFilterItem:
    case FmAfDacGainItem: {
        command.kind = CommandKind::FmRxControls;
        command.fm_rx_controls = state_.fm_rx_controls;
        auto &value = selected_ == FmWeakFilterItem ? command.fm_rx_controls.weak_filter
                                                    : command.fm_rx_controls.af_dac_gain;
        const int maximum = selected_ == FmWeakFilterItem ? 7 : 15;
        const int next = int(value) + direction;
        if (next < 0 || next > maximum) {
            return;
        }
        value = next;
        command.expected_generation = state_.generation;
        command.expected_revision = state_.configuration_revision;
        command.selection = state_.selection;
        submit(command);
        return;
    }
    case ModeItem:
        if (!backend_capabilities().m17) {
            error_ = -ENOTSUP;
            return;
        }
        config.mode = config.mode == Mode::Fm ? Mode::M17 : Mode::Fm;
        break;
    case BandwidthItem:
        config.bandwidth =
            config.bandwidth == Bandwidth::Wide ? Bandwidth::Narrow : Bandwidth::Wide;
        break;
    case SquelchItem:
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        open_quick();
        return;
#endif
        config.squelch = (config.squelch + direction + 16) % 16;
        break;
    case GainItem:
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        open_quick(true);
        return;
#endif
        config.gain = (config.gain + direction + 16) % 16;
        break;
    case PowerItem: {
        const uint32_t powers[] = {1000, 2500, 5000};
        unsigned count = 0;
        for (uint32_t power : powers) {
            if (power <= backend_capabilities().max_power_mw) {
                ++count;
            }
        }
        if (!count) {
            error_ = -ENOTSUP;
            return;
        }
        unsigned index = 0;
        while (index + 1 < count && powers[index] < config.power_mw) {
            ++index;
        }
        config.power_mw = powers[(index + count + direction) % count];
        break;
    }
    case RxToneItem:
    case TxToneItem: {
        auto &tone = selected_ == RxToneItem ? config.rx_tone : config.tx_tone;
        const int count = sizeof(tones) / sizeof(tones[0]);
        int index = 0;
        while (index + 1 < count && tones[index] < tone.value) {
            ++index;
        }
        const auto value = tones[(index + count + direction) % count];
        tone = {value ? ToneKind::Ctcss : ToneKind::None, value, false};
        break;
    }
    case CallsignItem:
        text_ptt_sequence_ = radio_ptt_press_sequence();
        if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
            error_ = -EBUSY;
            return;
        }
        error_ = text_editor_.begin(TextKind::Callsign, config.callsign);
        if (!error_) {
            screen_ = UiScreen::Callsign;
        }
        return;
    case DiagnosticsItem:
        command.kind = CommandKind::EnterDiagnostics;
        break;
    case AppearanceItem:
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        // Anchor before the level check/readbacks: a later press remains visible
        // even if released or rejected before the next controller/UI tick.
        appearance_ptt_sequence_ = radio_ptt_press_sequence();
        if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
            error_ = -EBUSY;
            return;
        }
        settings_ui_preferences(applied_ui_, &appearance_revision_);
        appearance_draft_ = preference_original_ = applied_ui_;
        appearance_cursor_ = 0;
        field_editing_ = false;
        screen_ = UiScreen::Appearance;
#endif
        return;
    case VfoStepItem:
        open_step();
        return;
    case QuickControlsItem:
        open_quick();
        return;
    case BacklightItem:
        open_light();
        return;
    case ChannelsItem:
        open_channels(UiScreen::Menu);
        return;
    case BanksItem: {
        banks_return_ = UiScreen::Menu;
        open_channels(UiScreen::Menu);
        screen_ = UiScreen::Banks;
        browse_cursor_ = 0;
        refresh_list();
        return;
    }
    case OperatingItem:
        switch_operating();
        return;
    case FrequencyItem:
        input({UiKey::Back});
        input({UiKey::Hash});
        return;
    case SaveVfoItem:
        open_channel(0, UiScreen::Menu);
        return;
    case EditChannelItem:
        open_current_editor(UiScreen::Menu);
        return;
    case TransmitLimitItem:
        open_limit();
        return;
    case RadioItem:
    case DevelopmentItem:
        menu_group_ = selected_ == RadioItem ? 1 : 2;
        selected_ = menu_item_at(0, menu_group_);
        return;
    case CodecStatsItem:
    case RxPathItem:
    case RxGainItem:
    case RxSquelchItem:
        status_page_ =
            selected_ == CodecStatsItem ? 5 : rx_status_first_page + selected_ - RxPathItem;
        screen_ = UiScreen::Status;
        return;
    case StatusItem:
        status_page_ = 0;
        screen_ = UiScreen::Status;
        return;
    }
    submit(command);
}

void UiModel::cancel_appearance() {
    if (screen_ == UiScreen::Appearance) {
        screen_ = UiScreen::Menu;
        selected_ = AppearanceItem;
        appearance_draft_ = applied_ui_;
    }
}

void UiModel::appearance(const UiInput &input) {
    if (input.key == UiKey::Back) {
        if (field_editing_) {
            appearance_draft_ = field_preferences_;
            field_editing_ = false;
        } else {
            cancel_appearance();
        }
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        const int direction = input.key == UiKey::Down ? 1 : -1;
        if (!field_editing_) {
            appearance_cursor_ = (appearance_cursor_ + 5 + direction) % 5;
        } else if (appearance_cursor_ == 0) {
            appearance_draft_.theme = static_cast<Theme>(
                (unsigned(appearance_draft_.theme) + ThemeCount + direction) % ThemeCount);
        } else if (appearance_cursor_ == 1) {
            appearance_draft_.contrast =
                static_cast<Contrast>((unsigned(appearance_draft_.contrast) + 3 + direction) % 3);
        } else if (appearance_cursor_ == 2) {
            appearance_draft_.animations = !appearance_draft_.animations;
        } else {
            appearance_draft_.startup_animation = !appearance_draft_.startup_animation;
        }
    } else if (!field_editing_ && (input.key == UiKey::Left || input.key == UiKey::Right)) {
        appearance_cursor_ = appearance_cursor_ < 4 ? 4 : 0;
    } else if (input.key == UiKey::Enter) {
        if (field_editing_) {
            field_editing_ = false;
            return;
        }
        if (appearance_cursor_ < 4) {
            field_preferences_ = appearance_draft_;
            field_editing_ = true;
            return;
        }
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        if (!next_id_) {
            error_ = -EOVERFLOW;
            return;
        }
        const uint32_t id = next_id_++;
        error_ = settings_put_ui_preferences(appearance_draft_, id, state_, appearance_revision_);
        if (!error_) {
            appearance_pending_ = id;
        }
#endif
    }
}

void UiModel::cancel_text() {
    if (screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
        screen_ == UiScreen::ChannelNumber) {
        screen_ = screen_ == UiScreen::Callsign        ? UiScreen::Menu
                  : screen_ == UiScreen::ChannelNumber ? number_return_
                                                       : UiScreen::Home;
        if (screen_ == UiScreen::Menu) {
            selected_ = CallsignItem;
        }
        if (screen_ == UiScreen::Channels) {
            refresh_list();
        }
        text_editor_ = {};
    }
}

void UiModel::advance(int64_t now_ms) {
    if (startup_visible()) {
        const int64_t elapsed = now_ms - startup_started_;
        if (!state_.power_active || state_.fault || radio_ptt_requested() ||
            radio_ptt_press_sequence() != startup_ptt_ || elapsed < 0 || elapsed >= 2000) {
            stop_startup();
        } else {
            startup_position_ = elapsed >= 1000 ? 80 : elapsed * 80 / 1000;
            if (elapsed >= 1000 && !startup_sound_) {
                startup_sound_ = true;
                radio_startup_chime(true);
            }
        }
    }
    advance_signal(now_ms);
    advance_light(now_ms);
    advance_lock(now_ms);
    if (screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
        screen_ == UiScreen::ChannelNumber || screen_ == UiScreen::ChannelField ||
        screen_ == UiScreen::BankName) {
        text_editor_.advance(now_ms);
    }
}

void UiModel::text_input(const UiInput &input) {
    if (input.key == UiKey::Back) {
        cancel_text();
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        text_editor_.move(input.key == UiKey::Up ? -1 : 1);
    } else if (input.key == UiKey::Star || input.key == UiKey::Erase) {
        text_editor_.erase();
    } else if (input.key == UiKey::Hash) {
        text_editor_.finish();
    } else if (input.key == UiKey::Digit) {
        error_ = text_editor_.digit(input.character,
                                    input.timestamp_ms >= 0 ? input.timestamp_ms : k_uptime_get());
    } else if (input.key == UiKey::Character) {
        error_ = text_editor_.literal(input.character);
    } else if (input.key == UiKey::Enter) {
        text_editor_.finish();
        if (screen_ == UiScreen::ChannelNumber) {
            apply_number();
            return;
        }
        RadioCommand command;
        command.config = state_.config;
        if (screen_ == UiScreen::Frequency) {
            error_ = ui_parse_frequency(text_editor_.text(), command.config.rx_frequency_hz);
            command.config.tx_frequency_hz = command.config.rx_frequency_hz;
        } else {
            memset(command.config.callsign, 0, sizeof(command.config.callsign));
            memcpy(command.config.callsign, text_editor_.text(), text_editor_.length());
        }
        if (!error_) {
            error_ = validate_config(command.config);
        }
        if (!error_) {
            submit(command);
        }
    }
}

void UiModel::input(const UiInput &input, bool external_pending) {
    if (startup_visible() && input.pressed) {
        stop_startup();
        if (input.key != UiKey::Ptt && input.key != UiKey::Quit && input.key != UiKey::Release) {
            return;
        }
    }
    const bool consumed_wake_key = light_input(input);
    const bool consumed_lock_key = lock_input(input, consumed_wake_key);
    // Cancellation happens before pending/phase guards. PTT and release still
    // travel directly to the controller through the service's independent path.
    if (input.key == UiKey::Ptt || input.key == UiKey::Release || input.key == UiKey::Quit) {
        if (input.key != UiKey::Ptt || input.pressed) {
            ++motion_interruptions_;
        }
        if ((input.key != UiKey::Ptt || input.pressed) && screen_ == UiScreen::Diagnostics) {
            cancel_diagnostic_edit();
        }
        if (input.key != UiKey::Ptt) {
            cancel_appearance();
            cancel_text();
            cancel_channel(true);
            cancel_bank(true);
            cancel_step(true);
            cancel_quick(true);
            cancel_light(true);
            cancel_limit(true);
        }
        return;
    }
    if (input.key == UiKey::Monitor) {
        if (input.pressed && !consumed_wake_key && !keypad_locked_ && !command_pending() &&
            !external_pending && state_.phase == RadioPhase::Receiving) {
            if (screen_ == UiScreen::Home) {
                open_quick();
            } else if (screen_ == UiScreen::QuickControls && !quick_editing_) {
                cancel_quick();
            }
        }
        return;
    }
    if (external_pending) {
        error_ = -EBUSY;
        return;
    }
    if (consumed_wake_key || consumed_lock_key || !input.pressed || !state_.power_active ||
        state_.phase == RadioPhase::Fault) {
        return;
    }
#ifdef CONFIG_HT_MODEM
    if (screen_ == UiScreen::Menu && selected_ == ModemEnabledItem &&
        input.key == UiKey::Enter && modem_enabled()) {
        modem_disable();
        return;
    }
#endif
    if (state_.phase == RadioPhase::Transmitting || radio_ptt_requested()) {
        return;
    }
    // Warning/timeout presentation covers the current page. No hidden front
    // action may run beneath it; independent PTT/monitor paths remain live.
    if (state_.tx_warning || state_.tx_timed_out) {
        return;
    }
    if (screen_ == UiScreen::Home &&
        (state_.phase != RadioPhase::Receiving || radio_ptt_requested())) {
        error_ = -EBUSY;
        return;
    }
    if (input.key == UiKey::Back &&
        (screen_ == UiScreen::ChannelSaved || screen_ == UiScreen::BankSaved)) {
        cancel_channel();
        cancel_bank();
        error_ = 0;
        return;
    }
    if (appearance_pending_ || recall_pending_ || edit_pending_ || bank_pending_ || step_pending_ ||
        light_pending_ || awaiting_channel_save_ || awaiting_bank_save_ ||
        awaiting_preferences_save_) {
        return;
    }
    if (pending_) {
        error_ = -EBUSY;
        return;
    }
    error_ = 0;
    if (discard_open_) {
        discard_input(input);
        return;
    }
    if (input.key == UiKey::Back && !field_editing_ && !channel_editing_ && !tone_editing_ &&
        editor_dirty()) {
        discard_open_ = true;
        discard_cursor_ = 2;
        return;
    }
    if (screen_ == UiScreen::VfoStep) {
        step_input(input);
        return;
    }
    if (screen_ == UiScreen::QuickControls) {
        quick_input(input);
        return;
    }
    if (screen_ == UiScreen::TransmitLimit) {
        limit_input(input);
        return;
    }
    if (screen_ == UiScreen::Backlight) {
        backlight_input(input);
        return;
    }
    if (channel_programming(screen_)) {
        channel_input(input);
        return;
    }
    if (bank_programming(screen_)) {
        bank_input(input);
        return;
    }
    if (screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
        screen_ == UiScreen::ChannelNumber) {
        text_input(input);
        return;
    }
    if (screen_ == UiScreen::Appearance) {
        appearance(input);
        return;
    }
    if (screen_ == UiScreen::BrowseActions) {
        if (input.key == UiKey::Back) {
            screen_ = browse_origin_;
            refresh_list();
        } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
            browse_action_ ^= 1;
        } else if (input.key == UiKey::Enter) {
            const auto id = list_.rows[list_.cursor % 4].id;
            if (browse_action_) {
                if (browse_action_bank_) {
                    open_bank(id);
                } else {
                    open_channel(id, UiScreen::Channels);
                }
            } else if (browse_action_bank_) {
                browse_bank_ = id;
                browse_cursor_ = 0;
                browse_channel_ = 0;
                screen_ = UiScreen::Channels;
                refresh_list();
            } else {
                recall({Operating::Memory, browse_bank_, id}, browse_revision_);
            }
        }
        return;
    }
    if (screen_ == UiScreen::Channels || screen_ == UiScreen::Banks) {
        browse(input);
        return;
    }
    if (screen_ == UiScreen::Status) {
        if (input.key == UiKey::Back) {
            screen_ = UiScreen::Menu;
            selected_ = menu_group_ == 2 ? (codec_statistics_page()
                                                ? CodecStatsItem
                                                : RxPathItem + status_page_ - rx_status_first_page)
                                         : StatusItem;
        } else if (input.key == UiKey::Left || input.key == UiKey::Right) {
            const unsigned first = menu_group_ == 2 ? 5 : 0;
            const unsigned count = menu_group_ == 2 ? status_page_count - 5 : 5;
            status_page_ =
                first + (status_page_ - first + (input.key == UiKey::Left ? count - 1 : 1)) % count;
#ifdef CONFIG_HT_CODEC2
        } else if (input.key == UiKey::Enter && codec_statistics_page()) {
            m17::voice_statistics_reset();
#endif
        }
        return;
    }
    if (screen_ == UiScreen::Diagnostics) {
        diagnostics(input);
        return;
    }
    if (input.key == UiKey::Back) {
        if (screen_ == UiScreen::Menu && field_editing_) {
            field_editing_ = false;
            return;
        }
        if (screen_ == UiScreen::Menu && menu_group_) {
            selected_ = menu_group_ == 1 ? RadioItem
                        : menu_group_ == 3 ? ModemItem : DevelopmentItem;
            menu_group_ = 0;
        } else {
            screen_ = UiScreen::Home;
        }
        return;
    }
    if (screen_ == UiScreen::Home) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        if (state_.selection.operating == Operating::Vfo &&
            (input.key == UiKey::Up || input.key == UiKey::Down)) {
            tune_vfo(input.key == UiKey::Up ? 1 : -1);
            return;
        }
        if (state_.selection.operating == Operating::Memory) {
            if (input.key == UiKey::Up || input.key == UiKey::Down) {
                step_memory(input.key == UiKey::Down ? 1 : -1);
                return;
            }
            if ((input.key == UiKey::Character || input.key == UiKey::Digit) &&
                input.character >= '0' && input.character <= '9') {
                open_number(UiScreen::Home);
                if (screen_ == UiScreen::ChannelNumber) {
                    text_input(input);
                }
                return;
            }
        }
#endif
        if (input.key == UiKey::Hash ||
            ((input.key == UiKey::Character || input.key == UiKey::Digit) &&
             input.character >= '0' && input.character <= '9')) {
            text_ptt_sequence_ = radio_ptt_press_sequence();
            if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
                error_ = -EBUSY;
                return;
            }
            text_editor_.begin(TextKind::Frequency);
            screen_ = UiScreen::Frequency;
            if (input.key != UiKey::Hash) {
                text_input(input);
            }
        } else if (input.key == UiKey::Enter) {
            screen_ = UiScreen::Menu;
            menu_group_ = 0;
#ifdef CONFIG_HT_CODEPLUG_STORAGE
            selected_ = EditChannelItem;
#else
            selected_ = ModeItem;
#endif
        }
    } else if (screen_ == UiScreen::Menu) {
#ifndef CONFIG_HT_CODEPLUG_STORAGE
        if (input.key == UiKey::Enter || input.key == UiKey::Left || input.key == UiKey::Right) {
            menu(input.key == UiKey::Left ? -1 : 1);
            return;
        }
#endif
        if (field_editing_) {
            if (input.key == UiKey::Up || input.key == UiKey::Down) {
                const int delta = input.key == UiKey::Up ? 1 : -1;
                if (selected_ >= ModemDelayItem && selected_ <= ModemLevelItem) {
                    const int maximum = selected_ == ModemDuplexItem ? 1
                                        : selected_ == ModemLevelItem ? 100 : 255;
                    const int minimum = selected_ == ModemLevelItem ? 1 : 0;
                    if (int(modem_field_) + delta >= minimum &&
                        int(modem_field_) + delta <= maximum) {
                        modem_field_ += delta;
                    }
                } else if (selected_ == LnaPolarityItem) {
                    field_config_.lna_inverted = !field_config_.lna_inverted;
                } else {
                    auto &value = selected_ == FmCtcssLevelItem   ? field_config_.fm_ctcss_level
                                  : selected_ == FmWeakFilterItem ? field_controls_.weak_filter
                                                                  : field_controls_.af_dac_gain;
                    const int maximum = selected_ == FmCtcssLevelItem   ? 127
                                        : selected_ == FmWeakFilterItem ? 7
                                                                        : 15;
                    if (int(value) + delta >= 0 && int(value) + delta <= maximum) {
                        value += delta;
                    }
                }
            } else if (input.key == UiKey::Enter) {
                RadioCommand command;
                command.kind = selected_ == LnaPolarityItem    ? CommandKind::LnaPolarity
                               : selected_ == FmCtcssLevelItem ? CommandKind::FmCtcssLevel
                                                               : CommandKind::FmRxControls;
#ifdef CONFIG_HT_MODEM
                if (selected_ >= ModemDelayItem && selected_ <= ModemLevelItem) {
                    command.kind = CommandKind::Modem;
                    command.modem_settings = modem_status().settings;
                    command.modem_enabled = modem_enabled();
                    command.modem_epoch = modem_epoch();
                    command.modem_ptt_sequence = radio_ptt_press_sequence();
                    auto &settings = command.modem_settings;
                    switch (selected_) {
                    case ModemDelayItem:
                        settings.txdelay = modem_field_;
                        break;
                    case ModemPersistenceItem:
                        settings.persistence = modem_field_;
                        break;
                    case ModemSlotItem:
                        settings.slot_time = modem_field_;
                        break;
                    case ModemTailItem:
                        settings.txtail = modem_field_;
                        break;
                    case ModemDuplexItem:
                        settings.full_duplex = modem_field_;
                        break;
                    case ModemLevelItem:
                        settings.level = modem_field_;
                        break;
                    }
                }
#endif
                command.config = field_config_;
                command.fm_rx_controls = field_controls_;
                command.expected_generation = state_.generation;
                command.expected_revision = state_.configuration_revision;
                command.selection = state_.selection;
                submit(command);
                if (!error_) {
                    field_editing_ = false;
                }
            }
        } else if (input.key == UiKey::Up || input.key == UiKey::Down || input.key == UiKey::Left ||
                   input.key == UiKey::Right) {
            const bool previous = input.key == UiKey::Up || input.key == UiKey::Left;
            const unsigned count = input.key == UiKey::Left || input.key == UiKey::Right ? 4 : 1;
            for (unsigned i = 0; i < count; ++i) {
                selected_ =
                    menu_move(selected_, state_.config.mode, previous ? -1 : 1, menu_group_);
            }
        } else if (input.key == UiKey::Enter) {
            if (menu_inline(selected_)) {
                field_editing_ = true;
                field_config_ = state_.config;
                field_controls_ = state_.fm_rx_controls;
#ifdef CONFIG_HT_MODEM
                const auto settings = modem_status().settings;
                modem_field_ = selected_ == ModemDelayItem ? settings.txdelay
                               : selected_ == ModemPersistenceItem ? settings.persistence
                               : selected_ == ModemSlotItem ? settings.slot_time
                               : selected_ == ModemTailItem ? settings.txtail
                               : selected_ == ModemDuplexItem ? settings.full_duplex
                                                             : settings.level;
#endif
            } else {
                menu(1);
            }
        }
    }
}

void UiModel::lines(char (&text)[8][32]) const {
    memset(text, 0, sizeof(text));
    if (!state_.power_active) {
        snprintf(text[0], 32, "RADIO INACTIVE");
        snprintf(text[2], 32, "Switch on to start");
        snprintf(text[4], 32, "TX disabled");
        return;
    }
    if (state_.phase == RadioPhase::Fault) {
        snprintf(text[0], 32, "RADIO FAULT");
        snprintf(text[2], 32, "Error %d", state_.fault);
        snprintf(text[4], 32, "TX disabled");
        snprintf(text[6], 32, "Reboot required");
        return;
    }
    if (discard_open_) {
        strcpy(text[0], "UNSAVED CHANGES");
        strcpy(text[1], "Choose what to do");
        const char *choices[] = {"Save / Apply", "Discard", "Continue editing"};
        for (unsigned i = 0; i < 3; ++i) {
            snprintf(text[i + 2], 32, "%c %s", discard_cursor_ == i ? '>' : ' ', choices[i]);
        }
        return;
    }
    const auto &config = state_.config;
    if (screen_ == UiScreen::Home) {
        snprintf(text[0], 32, "OE3ANC HT | %s", config.mode == Mode::Fm ? "FM" : "M17");
        const uint32_t frequency = state_.phase == RadioPhase::Transmitting
                                       ? config.tx_frequency_hz
                                       : config.rx_frequency_hz;
        snprintf(text[1], 32, "%u.%06u MHz", frequency / 1000000, frequency % 1000000);
        snprintf(text[2], 32, "%s  %d dBm",
                 state_.phase == RadioPhase::Transmitting ? "TX"
                 : state_.monitor_active                  ? "MONITOR"
                 : state_.rx_active                       ? "RX active"
                                                          : "RX idle",
                 state_.rssi_dbm);
        snprintf(text[3], 32, "From %s",
                 state_.received_callsign[0] ? state_.received_callsign : "-");
        snprintf(text[4], 32, "Local %s", config.callsign[0] ? config.callsign : "unset");
        char power[16];
        ui_format_power(power, config.power_mw);
        snprintf(text[5], 32, "%s requested", power);
#ifdef CONFIG_HT_CODEPLUG_STORAGE
        if (state_.selection.operating == Operating::Memory) {
            Channel channel;
            if (!settings_channel(state_.selection.channel_id, channel)) {
                snprintf(text[0], 32, "MEM %03u | %s", channel.number,
                         config.mode == Mode::Fm ? "FM" : "M17");
                snprintf(text[4], 32, "%s", channel.name);
            }
        } else {
            snprintf(text[0], 32, "VFO | %s", config.mode == Mode::Fm ? "FM" : "M17");
        }
        strcpy(text[7], "OK Menu | PTT2 Quick");
#else
        snprintf(text[7], 32, "# Freq | Enter Menu");
#endif
        if (keypad_locked_) {
            snprintf(text[7], 32, "Hold * 1s to unlock");
        }
    } else if (screen_ == UiScreen::BrowseActions) {
        strcpy(text[0], browse_action_bank_ ? "BANK" : "CHANNEL");
        strcpy(text[1], list_.rows[list_.cursor % 4].name);
        strcpy(text[2], "> Select");
        strcpy(text[3], "> Edit");
    } else if (screen_ == UiScreen::Status) {
        UiStatus page;
        status(page);
        strcpy(text[0], page.title);
        strcpy(text[1], page.detail);
        for (unsigned row = 0; row < 4; ++row) {
            strcpy(text[row + 2], page.rows[row]);
        }
        strcpy(text[7], codec_statistics_page() ? "OK Reset | BACK Menu" : "P1 Prev | P2 Next");
        return;
    } else if (screen_ == UiScreen::TransmitLimit) {
        limit_lines(text);
    } else if (screen_ == UiScreen::Backlight) {
        backlight_lines(text);
    } else if (screen_ == UiScreen::QuickControls) {
        quick_lines(text);
    } else if (screen_ == UiScreen::VfoStep) {
        strcpy(text[0], "VFO TUNING STEP");
        strcpy(text[1], "Global / explicit Apply");
        snprintf(text[2], 32, "%u.%03u kHz", step_draft_ / 1000, step_draft_ % 1000);
        strcpy(text[3], "Up/Down changes step");
        strcpy(text[4], "RX/TX move together");
        strcpy(text[5], "OK applies; BACK cancels");
    } else if (bank_programming(screen_)) {
        bank_lines(text);
        if (bank_pending_) {
            snprintf(text[6], 32, "Applying...");
        }
        return;
    } else if (channel_programming(screen_)) {
        channel_lines(text);
        if (edit_pending_) {
            snprintf(text[6], 32, "Applying...");
        }
        return;
    } else if (screen_ == UiScreen::Channels || screen_ == UiScreen::Banks) {
        snprintf(text[0], 32, "%s", screen_ == UiScreen::Banks ? "BANKS" : "CHANNELS");
        snprintf(text[1], 32, "%s", list_.ready ? list_.detail : "Refreshing...");
        for (unsigned row = 0; row < 4; ++row) {
            if (list_.ready && list_.cursor / 4 * 4 + row < list_.count) {
                snprintf(text[row + 2], 32, "%c%s %.24s", row == list_.cursor % 4 ? '>' : ' ',
                         list_.rows[row].prefix, list_.rows[row].name);
            }
        }
        if (list_.ready && !list_.count) {
            snprintf(text[2], 32, "No channels in this group");
        }
        if (state_.phase == RadioPhase::Transmitting) {
            snprintf(text[6], 32, "TX / release PTT");
        }
        if (screen_ == UiScreen::Banks) {
            snprintf(text[7], 32, "OK Select | BACK Cancel");
        } else {
            strcpy(text[7], "OK Select | P1/P2 Pages");
        }
    } else if (screen_ == UiScreen::Appearance) {
        strcpy(text[0], "APPEARANCE");
        strcpy(text[1], field_editing_ ? "Editing / Up-Down changes" : "Select field");
        if (appearance_cursor_ < 4) {
            snprintf(text[2], 32, "> Theme: %s",
                     ui_palette(appearance_draft_.theme, Contrast::Normal).name);
            snprintf(text[3], 32, "> Contrast: %s", ui_contrast_name(appearance_draft_.contrast));
            snprintf(text[4], 32, "> Motion: %s", appearance_draft_.animations ? "on" : "off");
            snprintf(text[5], 32, "> Startup: %s",
                     appearance_draft_.startup_animation ? "on" : "off");
        } else {
            strcpy(text[2], "> Apply");
        }
    } else if (screen_ == UiScreen::Frequency || screen_ == UiScreen::Callsign ||
               screen_ == UiScreen::ChannelNumber) {
        snprintf(text[0], 32, "%s",
                 screen_ == UiScreen::Frequency       ? "Frequency (MHz)"
                 : screen_ == UiScreen::ChannelNumber ? "Select channel"
                                                      : "Local callsign");
        snprintf(text[1], 32, "%s",
                 screen_ == UiScreen::Frequency       ? "Direct entry / MHz"
                 : screen_ == UiScreen::ChannelNumber ? "1-256 / current bank"
                                                      : "9 characters / multi-tap");
        snprintf(text[2], 32, "%s", text_editor_.text());
        snprintf(text[4], 32, "%s",
                 screen_ != UiScreen::Callsign ? "Up/Down cursor" : "Up/Down cursor / # Next");
        snprintf(text[7], 32, "OK %s | BACK Cancel",
                 screen_ == UiScreen::ChannelNumber ? "Tune" : "Apply");
    } else if (screen_ == UiScreen::Menu) {
        UiListPage page;
        menu_page(page);
        strcpy(text[0], page.title);
        strcpy(text[1], page.detail);
        for (unsigned row = 0; row < 4 && page.cursor / 4 * 4 + row < page.count; ++row) {
            snprintf(text[row + 2], 32, "%c %s", row == page.cursor % 4 ? '>' : ' ',
                     page.rows[row].name);
        }
        strcpy(text[7], "OK Select | BACK Home");
    } else {
        snprintf(text[0], 32, "BK4819 diagnostics");
        snprintf(text[1], 32, "%c Address: %02X", selected_ == 0 ? '>' : ' ', address_);
        snprintf(text[2], 32, "%c Value: %04X", selected_ == 1 ? '>' : ' ', value_);
        snprintf(text[3], 32, "%c Read", selected_ == 2 ? '>' : ' ');
        snprintf(text[4], 32, "%c Write", selected_ == 3 ? '>' : ' ');
        snprintf(text[5], 32, "%c Exit / restore", selected_ == 4 ? '>' : ' ');
        if (editing_) {
            snprintf(text[selected_ + 1], 32, ">%s_ (hex)", draft_);
            snprintf(text[6], 32, "Up/Dn hex: %c |  # Add", "0123456789ABCDEF"[letter_]);
        }
        snprintf(text[7], 32, editing_ ? "OK Set | BACK Cancel" : "OK Select | BACK Exit");
    }
    if (state_.tx_timed_out) {
        snprintf(text[6], 32, "TX timeout: release PTT");
    } else if (state_.tx_warning) {
        snprintf(text[6], 32, "TX limit in %u s", state_.tx_remaining_s);
    } else if (error_ || state_.ptt_error) {
        snprintf(text[6], 32, "Error %d", error_ ? error_ : state_.ptt_error);
        if (screen_ == UiScreen::QuickControls || screen_ == UiScreen::TransmitLimit) {
            const char *message = error_ == -ENOMSG   ? "Queue busy; try again"
                                  : error_ == -EBUSY  ? "Radio busy; try again"
                                  : error_ == -ESTALE ? "Radio changed; reopen"
                                                      : nullptr;
            if (message) {
                snprintf(text[6], 32, "%s", message);
            }
        }
        if (screen_ == UiScreen::Backlight) {
            const char *message = error_ == -ESTALE  ? "Settings changed; reopen"
                                  : error_ == -EBUSY ? "Radio busy; try again"
                                  : error_ == -EROFS ? "Storage is read-only"
                                                     : "Apply failed; retry";
            snprintf(text[6], 32, "%s", message);
        }
        if (screen_ == UiScreen::ChannelNumber || screen_ == UiScreen::Channels ||
            screen_ == UiScreen::Banks) {
            const char *message = nullptr;
            switch (error_ ? error_ : state_.ptt_error) {
            case -ENOENT:
                message = "No channel in chosen bank";
                break;
            case -ERANGE:
                message = "Use channel 1-256";
                break;
            case -EINVAL:
                message = "Enter a channel number";
                break;
            case -ENOSPC:
                message =
                    screen_ == UiScreen::Banks ? "All 16 bank slots are used" : "At most 3 digits";
                break;
            case -EBUSY:
                message = "Radio busy; try again";
                break;
            case -ESTALE:
                message = "Radio changed; try again";
                break;
            case -EROFS:
                message = "Storage is read-only";
                break;
            case -EAGAIN:
                message = "List refreshed; try again";
                break;
            }
            if (message) {
                snprintf(text[6], 32, "%s", message);
            }
        }
    } else if (pending_ || appearance_pending_ || recall_pending_ || edit_pending_ ||
               step_pending_ || light_pending_) {
        snprintf(text[6], 32, "Applying...");
    } else if (screen_ == UiScreen::Home) {
        if (storage_first_run_) {
            snprintf(text[6], 32, "First-run defaults");
        } else if (storage_error_) {
            snprintf(text[6], 32, "Storage error %d", storage_error_);
        } else if (storage_pending_) {
            snprintf(text[6], 32, "Settings pending");
        }
    }
}
} // namespace ht
