// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ht/ui_preferences.hpp>
#include <ht/ui_text.hpp>

namespace ht {
enum class UiScreen : uint8_t {
    Home,
    Frequency,
    Menu,
    Callsign,
    Diagnostics,
    Appearance,
    Channels,
    Banks,
    ChannelNumber,
    ChannelEditor,
    ChannelField,
    ChannelTone,
    ChannelBank,
    ChannelReview,
    ChannelConfirm,
    ChannelSaved,
    BankEditor,
    BankName,
    BankMembers,
    BankAdd,
    BankActions,
    BankDelete,
    BankSaved,
    VfoStep,
    QuickControls,
    Backlight,
    TransmitLimit,
    Status,
    CompanionExit
};

inline bool channel_programming(UiScreen screen) {
    return screen >= UiScreen::ChannelEditor && screen <= UiScreen::ChannelSaved;
}

inline bool bank_programming(UiScreen screen) {
    return screen >= UiScreen::BankEditor && screen <= UiScreen::BankSaved;
}

struct UiListRow {
    uint32_t id = 0;
    char name[25] = {}, prefix[5] = {}, suffix[5] = {};
};

struct UiListPage {
    char title[25] = {}, detail[32] = {};
    UiListRow rows[4];
    uint16_t cursor = 0, count = 0;
    bool ready = false; // Cursor/rows/context belong to one validated revision.
};
enum class UiStatusColor : uint8_t { Muted, Accent, Amber, Red };

struct UiHomeContent {
    char identity[16] = {}, name[25] = {}, context[32] = {}, frequency[16] = {};
    char settings[32] = {}, activity[32] = {}, battery[12] = {};
    UiStatusColor status = UiStatusColor::Muted, context_color = UiStatusColor::Muted,
                  battery_color = UiStatusColor::Muted;
    uint8_t bars = 0;
    bool visible = false, transmitting = false, locked = false;
};

struct UiStatus {
    char title[25] = {}, detail[32] = {}, rows[4][32] = {};
    UiStatusColor color = UiStatusColor::Muted;
};

// One owned presentation copy, captured by the UI owner. No model, controller,
// settings, LVGL, heap or string pointers cross this boundary. This is an internal
// representation; the companion wire codec serializes fields explicitly.
struct UiPresentation {
    UiScreen screen = UiScreen::Home, list_return = UiScreen::Menu;
    UiPreferences preferences;
    UiListPage list;
    UiStatus status;

    struct Home : UiHomeContent {
        char mode[4] = "FM";
    } home;

    struct Text {
        char value[25] = {};
        TextKind kind = TextKind::Name;
        uint8_t cursor = 0;
        bool pending = false;
    } text;

    char lines[8][32] = {}, actions[4][32] = {}, diagnostic_text[5] = {};
    uint32_t interruptions = 0, ptt_sequence = 0, monitor_sequence = 0;
    uint8_t form_cursor = 0;
    bool motion = false, error = false, system_visible = false;
    bool command_pending = false, appearance_pending = false, recall_pending = false;
    bool diagnostic_editing = false, quick_available = false;
};

static_assert(sizeof(UiPresentation) <= 1280, "Bound copied visible UI data");
} // namespace ht
