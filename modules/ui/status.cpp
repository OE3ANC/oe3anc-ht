// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <ht/backend.hpp>
#ifdef CONFIG_HT_BATTERY
#include <ht/battery.hpp>
#endif
#ifdef CONFIG_HT_SETTINGS
#include <ht/settings.hpp>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

namespace ht {
bool UiModel::system_state(UiStatus &view) const {
    if (state_.power_active && state_.phase != RadioPhase::Fault) {
        return false;
    }
    view = {};
    strcpy(view.rows[0], "TX disabled");
    if (state_.power_active) {
        strcpy(view.title, "RADIO FAULT");
        snprintf(view.detail, sizeof(view.detail), "Latched error %d", state_.fault);
        strcpy(view.rows[1], "Normal operation stopped");
        strcpy(view.rows[2], "Release PTT");
        strcpy(view.rows[3], "Restart radio to recover");
        view.color = UiStatusColor::Red;
        return true;
    }
    strcpy(view.title, "RADIO INACTIVE");
    strcpy(view.detail, "Power gate inactive");
    strcpy(view.rows[1], "Awaiting target resume");
    strcpy(view.rows[3], "Check power / input");
    view.color = UiStatusColor::Muted;
#ifdef CONFIG_HT_BATTERY
    const auto caps = battery_capabilities();
    const auto sample = battery_snapshot();
    if (caps.voltage) {
        if (sample.freshness == BatteryFreshness::Unknown) {
            strcpy(view.rows[2], "Battery unknown");
        } else {
            snprintf(view.rows[2], sizeof(view.rows[2]), "%sBattery %u mV",
                     sample.freshness == BatteryFreshness::Stale ? "Last " : "",
                     sample.reading.millivolts);
        }
    }
    if (caps.power_switch) {
        const bool fresh = sample.freshness == BatteryFreshness::Fresh;
        strcpy(view.rows[1], !fresh                     ? "Switch input unknown"
                             : sample.reading.switch_on ? "Confirming switch-on"
                                                        : "Power switch off");
        if (fresh) {
            strcpy(view.rows[3],
                   sample.reading.switch_on ? "Wait for confirmation" : "Switch on to resume");
        }
    }
    if (caps.charger_input && sample.freshness != BatteryFreshness::Unknown) {
        snprintf(view.detail, sizeof(view.detail), "%sCharger %s",
                 sample.freshness == BatteryFreshness::Stale ? "Last " : "",
                 sample.reading.charger_input ? "present" : "absent");
    }
    if (sample.freshness == BatteryFreshness::Stale) {
        view.color = UiStatusColor::Amber;
    }
    if (sample.error) {
        snprintf(view.detail, sizeof(view.detail), "Battery read error %d", sample.error);
        view.color = UiStatusColor::Red;
    }
#endif
    if (state_.fault) {
        snprintf(view.detail, sizeof(view.detail), "Fault latched / error %d", state_.fault);
        strcpy(view.rows[1], "Fault survives switch-on");
        strcpy(view.rows[3], "Restart radio to recover");
        view.color = UiStatusColor::Red;
    }
    return true;
}

static void tone(char (&text)[32], const char *name, const Tone &tone) {
    if (tone.kind == ToneKind::Dcs) {
        snprintf(text, sizeof(text), "%s D%03o%s", name, tone.value, tone.inverted ? "I" : "N");
    } else if (tone.kind == ToneKind::Ctcss) {
        snprintf(text, sizeof(text), "%s CTCSS %u.%u Hz", name, tone.value / 10, tone.value % 10);
    } else {
        snprintf(text, sizeof(text), "%s tone off", name);
    }
}

void UiModel::status(UiStatus &view) const {
    view = {};
    if (screen_ == UiScreen::CompanionExit) {
        strcpy(view.title, "DISCONNECT CABLE");
        strcpy(view.detail, "Before restoring PTT");
        strcpy(view.rows[0], "Unplug companion cable");
        strcpy(view.rows[1], "Serial shares the PTT pin");
        strcpy(view.rows[2], "Then confirm below");
        strcpy(view.rows[3], "BACK keeps UART active");
        view.color = UiStatusColor::Amber;
        return;
    }
    const auto &config = state_.config;
    switch (status_page_) {
    case 0: {
        strcpy(view.title, "RADIO / 1 OF 5");
        snprintf(view.detail, sizeof(view.detail), "%s / %s / %s",
                 state_.selection.operating == Operating::Memory ? "Memory" : "VFO",
                 config.mode == Mode::Fm ? "FM" : "M17",
                 state_.phase == RadioPhase::Transmitting ? "TX" : "RX");
        snprintf(view.rows[0], 32, "RX %u.%06u MHz", config.rx_frequency_hz / 1000000,
                 config.rx_frequency_hz % 1000000);
        snprintf(view.rows[1], 32, "TX %u.%06u MHz", config.tx_frequency_hz / 1000000,
                 config.tx_frequency_hz % 1000000);
        snprintf(view.rows[2], 32, "Requested power %u mW", config.power_mw);
        const char *duplex = config.tx_inhibit                                  ? "Receive only"
                             : config.rx_frequency_hz != config.tx_frequency_hz ? "Split TX"
                                                                                : "Simplex";
        if (backend_capabilities().gain) {
            snprintf(view.rows[3], 32, "%s / Gain %u", duplex, config.gain);
        } else {
            snprintf(view.rows[3], 32, "%s", duplex);
        }
        break;
    }
    case 1:
        strcpy(view.title, "MODE / 2 OF 5");
        snprintf(view.detail, sizeof(view.detail), "%s / limit %s",
                 config.mode == Mode::Fm ? "FM" : "M17",
                 config.transmit_limit_s ? "enabled" : "off");
        if (config.mode == Mode::Fm) {
            snprintf(view.rows[0], 32, "%s / SQL %u",
                     config.bandwidth == Bandwidth::Wide ? "25 kHz" : "12.5 kHz", config.squelch);
            tone(view.rows[1], "RX", config.rx_tone);
            tone(view.rows[2], "TX", config.tx_tone);
        } else {
            snprintf(view.rows[0], 32, "Destination %s",
                     config.m17.destination == Destination::Broadcast ? "ALL"
                                                                      : config.m17.callsign);
            snprintf(view.rows[1], 32, "CAN %u / RX filter %s", config.m17.can,
                     config.m17.rx_can_check ? "on" : "off");
            snprintf(view.rows[2], 32, "Local %s", config.callsign[0] ? config.callsign : "unset");
        }
        if (config.transmit_limit_s) {
            snprintf(view.rows[3], 32, "TX limit %u s", config.transmit_limit_s);
        } else {
            strcpy(view.rows[3], "TX limit off");
        }
        break;
    case 2:
        strcpy(view.title, "ACTIVITY / 3 OF 5");
        strcpy(view.detail, "Live controller snapshot");
        snprintf(view.rows[0], 32, "RSSI %d dBm / relative", state_.rssi_dbm);
        snprintf(view.rows[1], 32, "RX %s", state_.rx_active ? "active" : "idle");
        snprintf(view.rows[2], 32, "From %s",
                 config.mode == Mode::M17 && state_.rx_active && state_.received_callsign[0]
                     ? state_.received_callsign
                     : "-");
        snprintf(view.rows[3], 32, "Monitor %s", state_.monitor_active ? "held" : "off");
        break;
    case 3: {
        strcpy(view.title, "BATTERY / 4 OF 5");
#ifdef CONFIG_HT_BATTERY
        const auto caps = battery_capabilities();
        const auto snapshot = battery_snapshot();
        if (snapshot.freshness == BatteryFreshness::Unknown) {
            strcpy(view.detail, "Unknown / no valid sample");
            strcpy(view.rows[0], caps.voltage ? "Voltage unknown" : "Voltage unavailable");
            strcpy(view.rows[1],
                   caps.charger_input ? "Charger input unknown" : "Charger unavailable");
            strcpy(view.rows[2], caps.power_switch ? "Switch input unknown" : "Switch unavailable");
            strcpy(view.rows[3], "No valid sample age");
        } else {
            const bool stale = snapshot.freshness == BatteryFreshness::Stale;
            strcpy(view.detail, stale ? "Stale / retained sample" : "Fresh / circuit inputs");
            view.color = stale ? UiStatusColor::Amber : UiStatusColor::Muted;
            const char *last = stale ? "Last " : "";
            if (caps.voltage) {
                snprintf(view.rows[0], 32, "%s%u mV", last, snapshot.reading.millivolts);
            } else {
                strcpy(view.rows[0], "Voltage unavailable");
            }
            if (caps.charger_input) {
                snprintf(view.rows[1], 32, "%sCharger %s", last,
                         snapshot.reading.charger_input ? "present" : "absent");
            } else {
                strcpy(view.rows[1], "Charger unavailable");
            }
            if (caps.power_switch) {
                snprintf(view.rows[2], 32, "%sSwitch %s", last,
                         snapshot.reading.switch_on ? "on" : "off");
            } else {
                strcpy(view.rows[2], "Switch unavailable");
            }
            const int64_t now = k_uptime_get();
            const int64_t age = now >= snapshot.sample_ms ? now - snapshot.sample_ms : 0;
            snprintf(view.rows[3], 32, "Age %lld ms", static_cast<long long>(age));
        }
        if (snapshot.error) {
            snprintf(view.detail, sizeof(view.detail), "%s / read error %d",
                     snapshot.freshness == BatteryFreshness::Unknown ? "Unknown" : "Stale",
                     snapshot.error);
            view.color = UiStatusColor::Red;
        }
#else
        strcpy(view.detail, "Telemetry unavailable");
#endif
        break;
    }
    default:
        strcpy(view.title, "STORAGE / 5 OF 5");
#ifdef CONFIG_HT_SETTINGS
        const auto storage = settings_status();
        strcpy(view.detail,
               storage.read_only ? "Read-only / retained data" : "Settings owner snapshot");
        if (storage.save_error) {
            snprintf(view.detail, sizeof(view.detail), "Save error %d / unsaved",
                     storage.save_error);
            view.color = UiStatusColor::Red;
        } else if (storage.load_error) {
            snprintf(view.detail, sizeof(view.detail),
                     storage.load_error == -ENOENT ? "Loaded first-run defaults" : "Load error %d",
                     storage.load_error);
            view.color = storage.load_error == -ENOENT ? UiStatusColor::Muted : UiStatusColor::Red;
        }
        strcpy(view.rows[0], storage.read_only ? "Read-only / retained data"
                             : storage.pending ? "Storage pending / dirty"
                                               : "Storage clean");
        snprintf(view.rows[1], 32, "Channels %u / Banks %u", storage.channel_count,
                 storage.bank_count);
        snprintf(view.rows[2], 32, "Generation %u", storage.generation);
        if (storage.operation_pending) {
            snprintf(view.rows[3], 32, "Applying operation %u", storage.operation_id);
        } else if (storage.operation_error) {
            snprintf(view.rows[3], 32, "Operation error %d", storage.operation_error);
        } else {
            strcpy(view.rows[3], "No operation pending");
        }
#else
        strcpy(view.detail, "Persistence unavailable");
#endif
        break;
    }
}
} // namespace ht
