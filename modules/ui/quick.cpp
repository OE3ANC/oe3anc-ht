// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/backend.hpp>
#include <ht/ui.hpp>
#include <stdio.h>
#include <string.h>

namespace ht {
uint8_t UiModel::quick_cursor() const {
    return quick_selected_;
}

bool UiModel::quick_available() const {
    return true;
}

void UiModel::open_quick(bool) {
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    quick_generation_ = state_.generation;
    quick_revision_ = state_.configuration_revision;
    quick_selection_ = state_.selection;
    quick_squelch_ = state_.config.squelch;
    quick_return_ = screen_;
    quick_selected_ = 0;
    quick_editing_ = false;
    screen_ = UiScreen::QuickControls;
}

void UiModel::cancel_quick(bool interruption) {
    if (screen_ == UiScreen::QuickControls) {
        screen_ = interruption ? UiScreen::Home : quick_return_;
        quick_editing_ = false;
    }
}

void UiModel::quick_input(const UiInput &input) {
    if (input.key == UiKey::Back) {
        if (quick_editing_) {
            quick_squelch_ = state_.config.squelch;
            quick_editing_ = false;
        } else {
            cancel_quick();
        }
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        if (quick_editing_) {
            const int next = quick_squelch_ + (input.key == UiKey::Up ? 1 : -1);
            if (next >= 0 && next <= 15) {
                quick_squelch_ = next;
            }
        } else {
            do {
                quick_selected_ = (quick_selected_ + (input.key == UiKey::Down ? 1 : 3)) % 4;
            } while (state_.config.mode != Mode::Fm &&
                     (quick_selected_ == 1 || quick_selected_ == 2));
        }
    } else if (input.key == UiKey::Enter) {
        if (quick_selected_ == 0) {
            open_current_editor(UiScreen::Home);
            return;
        }
        if (quick_selected_ == 1 && !quick_editing_) {
            quick_editing_ = true;
            return;
        }
        RadioCommand command;
        command.expected_generation = quick_generation_;
        command.expected_revision = quick_revision_;
        command.selection = quick_selection_;
        if (quick_selected_ == 1) {
            command.kind = CommandKind::QuickControls;
            command.config = state_.config;
            command.config.squelch = quick_squelch_;
        } else {
            command.kind = CommandKind::ReceiveOverride;
            const auto selected =
                quick_selected_ == 2 ? ReceiveOverride::Monitor : ReceiveOverride::ListenTx;
            command.receive_override =
                state_.receive_override == selected ? ReceiveOverride::Normal : selected;
        }
        submit(command);
    }
}

void UiModel::quick_lines(char (&text)[8][32]) const {
    strcpy(text[0], "QUICK CONTROLS");
    strcpy(text[1], quick_editing_ ? "Editing / Up-Down changes" : "Temporary receive controls");
    const char *names[] = {"Edit VFO / channel", "Squelch", "Monitor", "Listen on TX"};
    for (unsigned row = 0; row < 4; ++row) {
        if (state_.config.mode != Mode::Fm && (row == 1 || row == 2)) {
            continue;
        }
        snprintf(text[row + 2], 32, "%c %s", quick_selected_ == row ? '>' : ' ', names[row]);
    }
    if (state_.config.mode == Mode::Fm) {
        snprintf(text[3], 32, "%c Squelch: %u", quick_selected_ == 1 ? '>' : ' ', quick_squelch_);
        snprintf(text[4], 32, "%c Monitor: %s", quick_selected_ == 2 ? '>' : ' ',
                 state_.receive_override == ReceiveOverride::Monitor ? "on" : "off");
    }
    snprintf(text[5], 32, "%c Listen on TX: %s", quick_selected_ == 3 ? '>' : ' ',
             state_.receive_override == ReceiveOverride::ListenTx ? "on" : "off");
}

void UiModel::quick_actions(const char *(&actions)[4]) const {
    actions[0] = quick_editing_ ? "OK Accept" : "OK Select";
    actions[1] = "BACK";
    actions[2] = actions[3] = "";
    if (command_pending()) {
        for (auto &action : actions) {
            action = "";
        }
    }
}
} // namespace ht
