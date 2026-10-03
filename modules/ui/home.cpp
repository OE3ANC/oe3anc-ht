// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <ht/battery.hpp>
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace ht {
void UiModel::advance_signal(int64_t now) {
    if (!state_.power_active || state_.fault || state_.phase != RadioPhase::Receiving ||
        !state_.rx_active) {
        signal_bars_ = 0;
        signal_step_ms_ = -1;
        return;
    }
    // Relative indicator only: one bar per 80 ms, with prompt activity clear.
    // Do not catch up multiple steps after delayed UI service or clock reversal.
    uint8_t target = 0;
    const int16_t thresholds[] = {-120, -110, -100, -90, -80};
    for (const auto threshold : thresholds) {
        target += state_.rssi_dbm >= threshold;
    }
    if (signal_step_ms_ < 0 || now < signal_step_ms_ || now - signal_step_ms_ >= 80) {
        if (signal_bars_ < target) {
            ++signal_bars_;
        } else if (signal_bars_ > target) {
            --signal_bars_;
        }
        signal_step_ms_ = now;
    }
}

void UiModel::home(UiHome &view) const {
    view = {};
    view.visible =
        state_.power_active && !state_.fault &&
        (state_.phase == RadioPhase::Receiving || state_.phase == RadioPhase::Transmitting) &&
        (screen_ == UiScreen::Home || state_.tx_warning || state_.tx_timed_out);
    if (!view.visible) {
        return;
    }
    const auto &config = state_.config;
    view.transmitting = state_.phase == RadioPhase::Transmitting;
    view.locked = keypad_locked_;
    view.mode = config.mode == Mode::Fm ? "FM" : "M17";
    view.bars = signal_bars_;
    strcpy(view.identity, "VFO");
    snprintf(view.name, sizeof(view.name), "%s %s",
             config.rx_frequency_hz < 200000000 ? "VHF" : "UHF",
             config.tx_inhibit                                  ? "RECEIVE ONLY"
             : config.tx_frequency_hz != config.rx_frequency_hz ? "SPLIT"
                                                                : "SIMPLEX");
    strcpy(view.context, "MHz / direct tuning");
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (state_.selection.operating == Operating::Memory) {
        uint16_t number = 0;
        char bank[25];
        if (!settings_selection_labels(state_.selection, view.name, number, bank)) {
            snprintf(view.identity, sizeof(view.identity), "MEM %03u", number);
            snprintf(view.context, sizeof(view.context), "MHz / %.24s", bank);
        } else {
            strcpy(view.identity, "MEM");
            strcpy(view.name, "Channel unavailable");
            strcpy(view.context, "Refreshing / MHz");
        }
    }
#endif
    const uint32_t frequency = view.transmitting ? config.tx_frequency_hz : config.rx_frequency_hz;
    snprintf(view.frequency, sizeof(view.frequency), "%u.%06u", frequency / 1000000,
             frequency % 1000000);
    // Trim only zero digits: retain at least 3 decimals and every integer Hz.
    size_t length = strlen(view.frequency);
    const size_t minimum = size_t(strchr(view.frequency, '.') - view.frequency) + 4;
    while (length > minimum && view.frequency[length - 1] == '0') {
        view.frequency[--length] = 0;
    }
    const char *duplex = config.tx_inhibit                                  ? "RX ONLY"
                         : config.tx_frequency_hz != config.rx_frequency_hz ? "SPLIT"
                                                                            : "";
    if (config.mode == Mode::Fm) {
        const bool tone =
            config.rx_tone.kind != ToneKind::None || config.tx_tone.kind != ToneKind::None;
        snprintf(view.settings, sizeof(view.settings), "%s%s%s SQL%u %umW%s", duplex,
                 *duplex ? " " : "", config.bandwidth == Bandwidth::Wide ? "W" : "N",
                 config.squelch, config.power_mw, tone ? " T" : "");
    } else {
        snprintf(view.settings, sizeof(view.settings), "%s%sCAN%u%s %s", duplex, *duplex ? " " : "",
                 config.m17.can, config.m17.rx_can_check ? "F" : "",
                 config.m17.destination == Destination::Broadcast ? "ALL" : config.m17.callsign);
    }
    strcpy(view.activity, state_.rx_active ? "Receiving" : "Listening");
    view.status = state_.rx_active ? UiStatusColor::Accent : UiStatusColor::Muted;
    if (config.mode == Mode::M17 && state_.rx_active && state_.received_callsign[0]) {
        snprintf(view.activity, sizeof(view.activity), "RX %.9s", state_.received_callsign);
    }
    if (state_.monitor_active) {
        strcpy(view.activity, "FM monitor / held");
        view.status = UiStatusColor::Accent;
    }
    if (state_.companion_mode) {
        strcpy(view.activity, "Companion / PTT disabled");
        view.status = UiStatusColor::Accent;
    }
    if (storage_pending_) {
        strcpy(view.context, "Settings pending");
    }
    if (storage_error_) {
        snprintf(view.context, sizeof(view.context),
                 storage_first_run_ ? "First-run defaults" : "Unsaved / storage %d",
                 storage_error_);
        view.context_color = storage_first_run_ ? UiStatusColor::Muted : UiStatusColor::Red;
    }
    if (command_pending() || appearance_pending_) {
        strcpy(view.context, "Applying...");
    }
    if (error_ || state_.ptt_error) {
        const int error = error_ ? error_ : state_.ptt_error;
        snprintf(view.activity, sizeof(view.activity), "Radio error %d", error);
        view.status = UiStatusColor::Red;
    }
    if (view.transmitting) {
        strcpy(view.activity, "Transmitting");
        view.status = UiStatusColor::Amber;
        if (state_.tx_warning) {
            snprintf(view.activity, sizeof(view.activity), "TX ends in %u s",
                     state_.tx_remaining_s);
        }
    }
    if (state_.tx_timed_out) {
        strcpy(view.activity, "Timeout / release PTT");
        view.status = UiStatusColor::Red;
    }
#ifdef CONFIG_HT_BATTERY
    const auto battery = battery_snapshot();
    if (battery_capabilities().voltage) {
        if (battery.freshness == BatteryFreshness::Unknown) {
            strcpy(view.battery, "? V");
        } else {
            snprintf(view.battery, sizeof(view.battery), "%u.%02u%s",
                     battery.reading.millivolts / 1000, battery.reading.millivolts % 1000 / 10,
                     battery.freshness == BatteryFreshness::Stale ? "!"
                     : battery.reading.charger_input              ? "+"
                                                                  : "V");
            view.battery_color = battery.freshness == BatteryFreshness::Stale ? UiStatusColor::Amber
                                 : battery.reading.charger_input ? UiStatusColor::Accent
                                                                 : UiStatusColor::Muted;
        }
    }
#endif
    const bool available = state_.phase == RadioPhase::Receiving && !state_.tx_timed_out &&
                           !radio_ptt_requested() && !command_pending() && !appearance_pending_;
    if (available) {
        if (keypad_locked_) {
            view.actions[0] = "Hold * 1s";
            view.actions[1] = "to unlock";
        } else {
            view.actions[0] = "OK Menu";
            view.actions[1] = "BACK Quick";
#ifdef CONFIG_HT_CODEPLUG_STORAGE
            view.actions[2] =
                state_.selection.operating == Operating::Memory ? "P1 VFO" : "P1 Memory";
            view.actions[3] =
                state_.selection.operating == Operating::Memory ? "P2 Edit" : "P2 Save";
#endif
        }
    }
}
} // namespace ht
