// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <string.h>

namespace ht {
void ui_capture_presentation(const UiModel &model, UiPresentation &view) {
    view = {};
    view.screen = model.screen();
    view.list_return = model.list_return_screen();
    view.preferences = model.preferences();
    view.interruptions = model.motion_interruptions();
    view.ptt_sequence = radio_ptt_press_sequence();
    view.monitor_sequence = radio_monitor_press_sequence();
    view.motion = model.motion_allowed();
    view.error = model.error() != 0;
    view.command_pending = model.command_pending();
    view.appearance_pending = model.appearance_pending();
    view.recall_pending = model.recall_pending();
    view.diagnostic_editing = model.diagnostic_editing();
    view.quick_available = model.quick_available();
    view.form_cursor = model.form_cursor();
    model.lines(view.lines);
    UiHome home;
    model.home(home);
    static_cast<UiHomeContent &>(view.home) = home;
    strcpy(view.home.mode, home.mode);
    view.system_visible = model.system_state(view.status);
    if (!view.system_visible && view.screen == UiScreen::Status) {
        model.status(view.status);
    }
    view.list = model.list_page();
    if (view.screen == UiScreen::Menu) {
        model.menu_page(view.list);
    }
    if (view.screen == UiScreen::Diagnostics) {
        model.diagnostic_page(view.list);
    }
    if (view.screen == UiScreen::Startup) {
        view.list = {};
        view.list.cursor = model.startup_position();
        view.list.count = 81;
        view.list.ready = true;
    }
    const auto &text = model.text_editor();
    strcpy(view.text.value, text.text());
    view.text.kind = text.kind();
    view.text.cursor = text.cursor();
    view.text.pending = text.pending();
    strcpy(view.diagnostic_text, model.diagnostic_text());
    const char *actions[4] = {"", "", "", ""};
    const auto screen = view.screen;
    if (model.discard_open() && !view.system_visible && !home.visible) {
        strcpy(view.actions[0], "OK Select");
        strcpy(view.actions[1], "BACK Edit");
        return;
    }
    if (view.system_visible) {
        return;
    }
    if (home.visible) {
        for (unsigned row = 0; row < 4; ++row) {
            actions[row] = home.actions[row];
        }
    } else if (screen == UiScreen::BrowseActions) {
        actions[0] = "OK Select";
        actions[1] = "BACK";
    } else if (screen == UiScreen::Menu) {
        model.menu_actions(actions);
    } else if (screen == UiScreen::Status) {
        actions[0] = model.codec_statistics_page() ? "OK Reset" : "";
        actions[1] = "BACK Menu";
        actions[2] = "P1 Prev page";
        actions[3] = "P2 Next page";
    } else if (screen == UiScreen::Appearance) {
        if (!view.appearance_pending) {
            actions[0] = model.value_editing() ? "OK Accept" : "OK Select";
            actions[1] = "BACK";
            actions[2] = model.value_editing() ? "" : "P1 Prev page";
            actions[3] = model.value_editing() ? "" : "P2 Next page";
        }
    } else if (screen == UiScreen::Diagnostics) {
        model.diagnostic_actions(actions);
        if (!view.diagnostic_editing && view.recall_pending) {
            for (auto &action : actions) {
                action = "";
            }
        }
    } else if (screen == UiScreen::Frequency || screen == UiScreen::Callsign ||
               screen == UiScreen::ChannelNumber || screen == UiScreen::ChannelField ||
               screen == UiScreen::BankName) {
        actions[0] = screen == UiScreen::ChannelNumber ? "OK Tune" : "OK Apply";
        actions[1] = "BACK Cancel";
        actions[2] = "";
        actions[3] = "";
        if (screen == UiScreen::ChannelField) {
            model.channel_actions(actions);
        }
        if (screen == UiScreen::BankName) {
            model.bank_actions(actions);
        }
        if (view.command_pending) {
            for (auto &action : actions) {
                action = "";
            }
        }
    } else if (screen == UiScreen::Channels || screen == UiScreen::Banks ||
               screen == UiScreen::ChannelBank || screen == UiScreen::BankMembers ||
               screen == UiScreen::BankAdd) {
        const auto &page = view.list;
        actions[0] = page.ready ? screen != UiScreen::Channels ? "OK Select"
                                  : page.count                 ? "OK Tune"
                                                               : "OK Save VFO"
                                : "";
        actions[1] = screen == UiScreen::Banks            ? "BACK Cancel"
                     : view.list_return == UiScreen::Home ? "BACK Home"
                                                          : "BACK Menu";
        actions[0] = "OK Select";
        actions[2] = "P1 Prev page";
        actions[3] = "P2 Next page";
        if (screen == UiScreen::ChannelBank) {
            model.channel_actions(actions);
        }
        if (bank_programming(screen)) {
            model.bank_actions(actions);
        }
        if (view.recall_pending) {
            for (auto &action : actions) {
                action = "";
            }
        }
    } else if (screen == UiScreen::VfoStep || screen == UiScreen::Backlight ||
               screen == UiScreen::TransmitLimit) {
        if (!view.command_pending) {
            actions[0] = "OK Apply";
            actions[1] = "BACK Cancel";
            actions[0] = screen == UiScreen::Backlight
                             ? (model.value_editing() ? "OK Accept" : "OK Select")
                             : "OK Apply";
            actions[2] = "";
        }
    } else if (screen == UiScreen::QuickControls) {
        model.quick_actions(actions);
    } else if (bank_programming(screen)) {
        model.bank_actions(actions);
    } else if (channel_programming(screen)) {
        model.channel_actions(actions);
    }
    for (unsigned row = 0; row < 4; ++row) {
        if (actions[row]) {
            strcpy(view.actions[row], actions[row]);
        }
    }
}

void ui_view_update(const UiModel &model) {
    // One display owner, one fixed copy. Tests and local adapters retain the
    // model convenience overload; the standalone renderer sees only owned data.
    static UiPresentation presentation;
    ui_capture_presentation(model, presentation);
    ui_view_update(presentation);
}
} // namespace ht
