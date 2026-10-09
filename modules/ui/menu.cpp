// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu.hpp"
#include <ht/backend.hpp>
#include <ht/ui.hpp>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

namespace ht {
unsigned menu_item_at(unsigned position) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    static const MenuItem order[] = {
        ChannelsItem,      SaveVfoItem,     EditChannelItem, BanksItem,         AppearanceItem,
        OperatingItem,     FrequencyItem,   VfoStepItem,     QuickControlsItem, BacklightItem,
        TransmitLimitItem, CallsignItem,    StatusItem,      ModeItem,          BandwidthItem,
        SquelchItem,       PowerItem,       RxToneItem,      TxToneItem,        GainItem,
        FmWeakFilterItem,  FmAfDacGainItem, CompanionItem,   DiagnosticsItem};
    static_assert(sizeof(order) / sizeof(order[0]) == MenuCount,
                  "Every menu identity needs one position");
    return order[position];
#else
    return position;
#endif
}

unsigned menu_move(unsigned current, Mode mode, int direction) {
    unsigned index = 0;
    while (index + 1 < MenuCount && menu_item_at(index) != current) {
        ++index;
    }
    do {
        index = (index + (direction > 0 ? 1 : MenuCount - 1)) % MenuCount;
    } while (!menu_available(menu_item_at(index), mode));
    return menu_item_at(index);
}

bool menu_available(unsigned item, Mode mode) {
    if (item == FmWeakFilterItem || item == FmAfDacGainItem) {
        return mode == Mode::Fm && backend_capabilities().registers;
    }
    if (item == CompanionItem) {
        return IS_ENABLED(CONFIG_HT_COMPANION);
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
    return item == FmWeakFilterItem || item == FmAfDacGainItem || item == CompanionItem ||
           item == ModeItem || item == BandwidthItem || item == PowerItem || item == RxToneItem ||
           item == TxToneItem
#ifndef CONFIG_HT_CODEPLUG_STORAGE
           || item == SquelchItem || item == GainItem
#endif
        ;
}

void menu_label(unsigned item, const RadioState &state, uint32_t step, char (&value)[28]) {
    const auto &config = state.config;
    switch (item) {
    case FmWeakFilterItem: {
        static constexpr uint16_t bandwidth_hz[] = {1700, 2000, 2500, 3000, 3750, 4000, 4250, 4500};
        const unsigned hz = bandwidth_hz[state.fm_rx_controls.weak_filter] *
                            (config.bandwidth == Bandwidth::Wide ? 2 : 1);
        snprintf(value, sizeof(value), "FM weak BW: %u.%02uk", hz / 1000, hz % 1000 / 10);
        break;
    }
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
    case PowerItem:
        snprintf(value, sizeof(value), "Power: %u mW", config.power_mw);
        break;
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
        strcpy(value, "Edit / select channel");
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
    strcpy(page.title, "MENU");
    strcpy(page.detail, state_.phase == RadioPhase::Transmitting ? "TX / read-only actions"
                        : selected_ == FmWeakFilterItem || selected_ == FmAfDacGainItem
                            ? "FM RX test / until reboot"
                            : "Choose an action");
    for (unsigned position = 0; position < MenuCount; ++position) {
        const unsigned item = menu_item_at(position);
        if (menu_available(item, state_.config.mode)) {
            if (item == selected_) {
                page.cursor = page.count;
            }
            ++page.count;
        }
    }
    const unsigned first = page.cursor / 4 * 4;
    unsigned index = 0;
    for (unsigned position = 0; position < MenuCount; ++position) {
        const unsigned item = menu_item_at(position);
        if (!menu_available(item, state_.config.mode)) {
            continue;
        }
        if (index >= first && index < first + 4) {
            auto &row = page.rows[index - first];
            row.id = item;
            char value[28];
            if (item == CompanionItem) {
                snprintf(value, sizeof(value), "Companion: %s",
                         state_.companion_mode ? "on" : "off");
            } else {
                menu_label(item, state_, vfo_step_hz_, value);
            }
            snprintf(row.name, sizeof(row.name), "%.24s", value);
        }
        ++index;
    }
    page.ready = true;
}

void UiModel::menu_actions(const char *(&actions)[4]) const {
    actions[0] = actions[2] = actions[3] = "";
    actions[1] = "BACK Home";
    if (command_pending() || appearance_pending_) {
        actions[1] = "";
        return;
    }
    if (selected_ == StatusItem) {
        actions[0] = "OK View";
        return;
    }
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        return;
    }
    if (menu_inline(selected_)) {
        actions[0] = "OK Change";
        actions[2] = "P1 Prev";
        actions[3] = "P2 Next";
    } else {
        actions[0] = "OK Select";
    }
}
} // namespace ht
