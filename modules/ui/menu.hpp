// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/radio.hpp>

namespace ht {
// Workflow actions first. These are private UI identities, never stored IDs.
enum MenuItem {
    ModeItem,
    BandwidthItem,
    SquelchItem,
    PowerItem,
    RxToneItem,
    TxToneItem,
    GainItem,
    CallsignItem,
    DiagnosticsItem,
    TransmitLimitItem,
    AppearanceItem,
    ChannelsItem,
    BanksItem,
    OperatingItem,
    FrequencyItem,
    SaveVfoItem,
    EditChannelItem,
    VfoStepItem,
    QuickControlsItem,
    BacklightItem,
    StatusItem,
    FmWeakFilterItem,
    FmAfDacGainItem,
    FmCtcssLevelItem,
    LnaPolarityItem,
    RadioItem,
    DevelopmentItem,
    CodecStatsItem,
    RxPathItem,
    RxGainItem,
    RxSquelchItem,
    WasmLoaderItem,
    MenuCount
};

unsigned menu_count(unsigned group = 0);
unsigned menu_item_at(unsigned position, unsigned group = 0);
unsigned menu_move(unsigned current, Mode mode, int direction, unsigned group = 0);
bool menu_available(unsigned item, Mode mode);
bool menu_inline(unsigned item);
void menu_label(unsigned item, const RadioState &state, uint32_t step, char (&value)[28]);
} // namespace ht
