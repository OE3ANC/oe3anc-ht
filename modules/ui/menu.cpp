// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.hpp"
#include <ht/backend.hpp>
#include <ht/ui.hpp>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

namespace ht {
static const MenuItem main_menu[] = {EditChannelItem, ChannelsItem,  BanksItem,  RadioItem,
                                     AppearanceItem,  BacklightItem, StatusItem, DevelopmentItem};
static const MenuItem radio_menu[] = {OperatingItem, VfoStepItem, CallsignItem, TransmitLimitItem};
static const MenuItem development_menu[] = {DiagnosticsItem,  FmWeakFilterItem, FmAfDacGainItem,
                                            FmCtcssLevelItem, LnaPolarityItem,  CodecStatsItem,
                                            RxPathItem,       RxGainItem,       RxSquelchItem,
                                            WasmLoaderItem};

unsigned menu_count(unsigned group) {
#ifndef CONFIG_HT_CODEPLUG_STORAGE
    return RadioItem;
#endif
    return group == 1   ? ARRAY_SIZE(radio_menu)
           : group == 2 ? ARRAY_SIZE(development_menu)
                        : ARRAY_SIZE(main_menu);
}

unsigned menu_item_at(unsigned position, unsigned group) {
#ifndef CONFIG_HT_CODEPLUG_STORAGE
    return position;
#endif
    return group == 1   ? radio_menu[position]
           : group == 2 ? development_menu[position]
                        : main_menu[position];
}

unsigned menu_move(unsigned current, Mode mode, int direction, unsigned group) {
    const unsigned count = menu_count(group);
    unsigned index = 0;
    while (index + 1 < count && menu_item_at(index, group) != current) {
        ++index;
    }
    for (unsigned attempt = 0; attempt < count; ++attempt) {
        index = (index + count + direction) % count;
        if (menu_available(menu_item_at(index, group), mode)) {
            return menu_item_at(index, group);
        }
    }
    return current;
}

bool menu_available(unsigned item, Mode mode) {
    if (item == WasmLoaderItem) {
#ifdef CONFIG_HT_WASM_LOADER
        return true;
#else
        return false;
#endif
    }
    if (item == CodecStatsItem) {
#ifdef CONFIG_HT_CODEC2
        return true;
#else
        return false;
#endif
    }
    if (item == LnaPolarityItem || item == RxPathItem || item == RxGainItem ||
        item == RxSquelchItem) {
        return backend_capabilities().registers;
    }
    if (item == FmCtcssLevelItem) {
        return mode == Mode::Fm && backend_capabilities().ctcss;
    }
    if (item == FmWeakFilterItem || item == FmAfDacGainItem) {
        return mode == Mode::Fm && backend_capabilities().registers;
    }
#ifndef CONFIG_HT_CODEPLUG_STORAGE
    if (item >= AppearanceItem && item != StatusItem) {
        return false;
    }
#endif
    const auto &caps = backend_capabilities();
    if (item == BacklightItem && !ui_backend_has_backlight()) {
        return false;
    }
    if (item == QuickControlsItem && !caps.gain && mode != Mode::Fm) {
        return false;
    }
    if (mode == Mode::M17 && (item == BandwidthItem || item == SquelchItem || item == RxToneItem ||
                              item == TxToneItem)) {
        return false;
    }
    return !((item == RxToneItem || item == TxToneItem) && !caps.ctcss) &&
           !(item == GainItem && !caps.gain) && !(item == DiagnosticsItem && !caps.registers);
}

bool menu_inline(unsigned item) {
    return item == LnaPolarityItem || item == FmCtcssLevelItem || item == FmWeakFilterItem ||
           item == FmAfDacGainItem || item == ModeItem || item == BandwidthItem ||
           item == PowerItem || item == RxToneItem || item == TxToneItem
#ifndef CONFIG_HT_CODEPLUG_STORAGE
           || item == SquelchItem || item == GainItem
#endif
        ;
}

void menu_label(unsigned item, const RadioState &state, uint32_t step, char (&value)[28]) {
    const auto &config = state.config;
    switch (item) {
    case RadioItem:
        strcpy(value, "Radio");
        break;
    case DevelopmentItem:
        strcpy(value, "Development");
        break;
    case WasmLoaderItem:
        strcpy(value, "WASM loader");
        break;
    case CodecStatsItem:
        strcpy(value, "Codec2 statistics");
        break;
    case RxPathItem:
        strcpy(value, "RX Path");
        break;
    case RxGainItem:
        strcpy(value, "RX Gain");
        break;
    case RxSquelchItem:
        strcpy(value, "RX Squelch");
        break;
    case FmWeakFilterItem: {
        static constexpr uint16_t bandwidth_hz[] = {1700, 2000, 2500, 3000, 3750, 4000, 4250, 4500};
        const unsigned hz = bandwidth_hz[state.fm_rx_controls.weak_filter] *
                            (config.bandwidth == Bandwidth::Wide ? 2 : 1);
        snprintf(value, sizeof(value), "FM weak BW: %u.%02uk", hz / 1000, hz % 1000 / 10);
        break;
    }
    case LnaPolarityItem:
        snprintf(value, sizeof(value), "Invert LNA GPIOs: %s", config.lna_inverted ? "on" : "off");
        break;
    case FmCtcssLevelItem:
        snprintf(value, sizeof(value), "FM CTCSS level: %u", state.config.fm_ctcss_level);
        break;
    case FmAfDacGainItem:
        snprintf(value, sizeof(value), "FM AF DAC: %u", state.fm_rx_controls.af_dac_gain);
        break;
    case ModeItem:
        snprintf(value, sizeof(value), "Mode: %s", config.mode == Mode::Fm ? "FM" : "M17");
        break;
    case BandwidthItem:
        snprintf(value, sizeof(value), "BW: %s",
                 config.bandwidth == Bandwidth::Wide ? "25 kHz" : "12.5 kHz");
        break;
    case SquelchItem:
        snprintf(value, sizeof(value), "Squelch: %u", config.squelch);
        break;
    case PowerItem: {
        char power[16];
        ui_format_power(power, config.power_mw);
        snprintf(value, sizeof(value), "Power: %s", power);
        break;
    }
    case GainItem:
        snprintf(value, sizeof(value), "Gain: %u", config.gain);
        break;
    case RxToneItem:
    case TxToneItem: {
        const auto &tone = item == RxToneItem ? config.rx_tone : config.tx_tone;
        const char *name = item == RxToneItem ? "RX" : "TX";
        if (tone.kind == ToneKind::Dcs) {
            snprintf(value, sizeof(value), "%s: D%03o%s", name, tone.value,
                     tone.inverted ? "I" : "N");
        } else if (tone.kind == ToneKind::Ctcss) {
            snprintf(value, sizeof(value), "%s tone: %u.%u", name, tone.value / 10,
                     tone.value % 10);
        } else {
            snprintf(value, sizeof(value), "%s tone: off", name);
        }
        break;
    }
    case CallsignItem:
        strcpy(value, "Local callsign");
        break;
    case ChannelsItem:
        strcpy(value, "Channels");
        break;
    case BanksItem:
        strcpy(value, "Banks");
        break;
    case VfoStepItem:
        snprintf(value, sizeof(value), "VFO step %u.%03u kHz", step / 1000, step % 1000);
        break;
    case QuickControlsItem:
        strcpy(value, "Quick controls");
        break;
    case BacklightItem:
        strcpy(value, "Backlight");
        break;
    case OperatingItem:
        strcpy(value, "VFO / Memory");
        break;
    case FrequencyItem:
        strcpy(value, "VFO frequency");
        break;
    case SaveVfoItem:
        strcpy(value, "Save VFO as channel");
        break;
    case EditChannelItem:
        strcpy(value, state.selection.operating == Operating::Vfo ? "VFO Settings"
                                                                  : "Edit current channel");
        break;
    case AppearanceItem:
        strcpy(value, "Appearance");
        break;
    case TransmitLimitItem:
        if (config.transmit_limit_s) {
            snprintf(value, sizeof(value), "TX limit: %u s", config.transmit_limit_s);
        } else {
            strcpy(value, "TX limit: off");
        }
        break;
    case StatusItem:
        strcpy(value, "Status");
        break;
    default:
        strcpy(value, "BK4819 diagnostics");
        break;
    }
}

void UiModel::menu_page(UiListPage &page) const {
    page = {};
    strcpy(page.title, menu_group_ == 1 ? "RADIO" : menu_group_ == 2 ? "DEVELOPMENT" : "MENU");
    strcpy(page.detail, state_.phase == RadioPhase::Transmitting ? "TX / read-only actions"
                        : selected_ == LnaPolarityItem           ? "Global / RX, TX and idle"
                        : selected_ == FmCtcssLevelItem          ? "Global / 0-127 / default 74"
                        : selected_ == FmWeakFilterItem || selected_ == FmAfDacGainItem
                            ? "FM RX test / until reboot"
                            : "Choose an action");
    for (unsigned position = 0; position < menu_count(menu_group_); ++position) {
        const unsigned item = menu_item_at(position, menu_group_);
        if (menu_available(item, state_.config.mode)) {
            if (item == selected_) {
                page.cursor = page.count;
            }
            ++page.count;
        }
    }
    const unsigned first = page.cursor / 4 * 4;
    unsigned index = 0;
    for (unsigned position = 0; position < menu_count(menu_group_); ++position) {
        const unsigned item = menu_item_at(position, menu_group_);
        if (!menu_available(item, state_.config.mode)) {
            continue;
        }
        if (index >= first && index < first + 4) {
            auto &row = page.rows[index - first];
            row.id = item;
            char value[28];
            auto displayed = state_;
            if (field_editing_ && item == selected_) {
                displayed.config = field_config_;
                displayed.fm_rx_controls = field_controls_;
            }
            menu_label(item, displayed, vfo_step_hz_, value);
            snprintf(row.name, sizeof(row.name), "%.24s", value);
        }
        ++index;
    }
    page.ready = true;
}

void UiModel::menu_actions(const char *(&actions)[4]) const {
    actions[0] = actions[2] = actions[3] = "";
    actions[1] = "BACK";
    actions[2] = "P1 Prev page";
    actions[3] = "P2 Next page";
    if (command_pending() || appearance_pending_) {
        actions[1] = "";
        return;
    }
    if (selected_ == StatusItem) {
        actions[0] = "OK View";
        return;
    }
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        actions[0] = actions[2] = actions[3] = "";
        return;
    }
    if (menu_inline(selected_)) {
        actions[0] = field_editing_ ? "OK Accept" : "OK Edit";
        if (field_editing_) {
            actions[2] = actions[3] = "";
        }
    } else {
        actions[0] = "OK Select";
    }
}
} // namespace ht
