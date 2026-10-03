// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/backend.hpp>
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace ht {
uint8_t UiModel::quick_cursor() const {
    return quick_gain_field_ || !backend_capabilities().gain ? 0 : 1;
}

bool UiModel::quick_available() const {
    return backend_capabilities().gain || state_.config.mode == Mode::Fm;
}

void UiModel::open_quick(bool gain) {
    quick_ptt_sequence_ = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    quick_gain_ = state_.config.gain;
    quick_squelch_ = state_.config.squelch;
    quick_gain_field_ = backend_capabilities().gain && (gain || state_.config.mode != Mode::Fm);
    quick_generation_ = state_.generation;
    quick_revision_ = state_.configuration_revision;
    quick_selection_ = state_.selection;
    quick_return_ = screen_;
    screen_ = UiScreen::QuickControls;
}

void UiModel::cancel_quick(bool interruption) {
    if (screen_ == UiScreen::QuickControls) {
        screen_ = interruption ? UiScreen::Home : quick_return_;
    }
}

void UiModel::quick_input(const UiInput &input) {
    if (input.key == UiKey::Back) {
        cancel_quick();
        return;
    }
    if (!quick_available()) {
        return;
    }
    if (input.key == UiKey::Left && backend_capabilities().gain && state_.config.mode == Mode::Fm) {
        quick_gain_field_ = !quick_gain_field_;
    } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
        auto &value = quick_gain_field_ ? quick_gain_ : quick_squelch_;
        const int next = int(value) + (input.key == UiKey::Up ? 1 : -1);
        value = next < 0 ? 0 : next > 15 ? 15 : next;
    } else if (input.key == UiKey::Enter) {
        RadioCommand command;
        command.kind = CommandKind::QuickControls;
        command.config.gain = quick_gain_;
        command.config.squelch = quick_squelch_;
        command.expected_generation = quick_generation_;
        command.expected_revision = quick_revision_;
        command.selection = quick_selection_;
        submit(command);
    }
}

void UiModel::quick_lines(char (&text)[8][32]) const {
    strcpy(text[0], "QUICK CONTROLS");
    if (!quick_available()) {
        strcpy(text[1], "No controls for this mode");
        strcpy(text[2], "  Gain unavailable");
        strcpy(text[3], "  Squelch is FM only");
        return;
    }
    strcpy(text[1], "Local draft / explicit Apply");
    unsigned row = 2;
    if (backend_capabilities().gain) {
        snprintf(text[row++], 32, "%c Gain: %u", quick_gain_field_ ? '>' : ' ', quick_gain_);
    }
    if (state_.config.mode == Mode::Fm) {
        snprintf(text[row++], 32, "%c Squelch: %u", quick_gain_field_ ? ' ' : '>', quick_squelch_);
    }
    strcpy(text[4], state_.config.mode != Mode::Fm                    ? "  Gain is global"
                    : state_.selection.operating == Operating::Memory ? "  Memory: temporary SQL"
                                                                      : "  VFO: persistent SQL");
    strcpy(text[5], "  Up/Down adjusts value");
}

void UiModel::quick_actions(const char *(&actions)[4]) const {
    actions[0] = quick_available() ? "OK Apply" : "";
    actions[1] = quick_available()                 ? "BACK Cancel"
                 : quick_return_ == UiScreen::Home ? "BACK Home"
                                                   : "BACK Menu";
    actions[2] = backend_capabilities().gain && state_.config.mode == Mode::Fm ? "P1 Field" : "";
    actions[3] = "";
    if (command_pending()) {
        for (auto &action : actions) {
            action = "";
        }
    }
}
} // namespace ht
