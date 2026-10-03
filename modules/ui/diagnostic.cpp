// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace ht {
void UiModel::cancel_diagnostic_edit() {
    editing_ = false;
    length_ = 0;
    letter_ = 0;
    draft_[0] = 0;
}

void UiModel::diagnostics(const UiInput &input) {
    if (editing_ && radio_ptt_press_sequence() != diagnostic_ptt_sequence_) {
        cancel_diagnostic_edit();
        return;
    }
    if (radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    if (editing_) {
        static const char hex[] = "0123456789ABCDEF";
        const size_t capacity = selected_ == 0 ? 2 : 4;
        if (input.key == UiKey::Back) {
            cancel_diagnostic_edit();
        } else if (input.key == UiKey::Enter) {
            if (!length_) {
                error_ = -EINVAL;
                return;
            }
            unsigned value = 0;
            for (size_t i = 0; i < length_; ++i) {
                const char c = draft_[i];
                value = value * 16 + (c <= '9' ? c - '0' : c - 'A' + 10);
            }
            if (selected_ == 0 && value > 0x7f) {
                error_ = -ERANGE;
                return;
            }
            if (selected_ == 0) {
                address_ = value;
            } else {
                value_ = value;
            }
            cancel_diagnostic_edit();
        } else if (input.key == UiKey::Erase || input.key == UiKey::Hash ||
                   input.key == UiKey::Left) {
            if (length_) {
                draft_[--length_] = 0;
            }
        } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
            letter_ = (letter_ + (input.key == UiKey::Up ? 1 : 15)) % 16;
        } else {
            char c = 0;
            if (input.key == UiKey::Right || input.key == UiKey::Star) {
                c = hex[letter_];
            } else if (input.key == UiKey::Character || input.key == UiKey::Digit) {
                c = input.character;
                if (c >= 'a' && c <= 'f') {
                    c -= 'a' - 'A';
                }
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
                    error_ = -EINVAL;
                    return;
                }
            }
            if (c) {
                if (length_ == capacity) {
                    error_ = -ENOSPC;
                    return;
                }
                draft_[length_++] = c;
                draft_[length_] = 0;
            }
        }
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        selected_ = (selected_ + (input.key == UiKey::Down ? 1 : 4)) % 5;
        return;
    }
    if (input.key != UiKey::Enter && input.key != UiKey::Back) {
        return;
    }
    if (input.key == UiKey::Enter && selected_ < 2) {
        diagnostic_ptt_sequence_ = radio_ptt_press_sequence();
        if (radio_ptt_requested()) {
            error_ = -EBUSY;
            return;
        }
        cancel_diagnostic_edit();
        editing_ = true;
        return;
    }
    RadioCommand command;
    command.register_address = address_;
    command.register_value = value_;
    command.kind = input.key == UiKey::Back || selected_ == 4 ? CommandKind::ExitDiagnostics
                   : selected_ == 2                           ? CommandKind::ReadRegister
                                                              : CommandKind::WriteRegister;
    submit(command);
}

void UiModel::diagnostic_page(UiListPage &page) const {
    page = {};
    page.ready = true;
    page.cursor = selected_;
    page.count = 5;
    strcpy(page.title, editing_ ? selected_ == 0 ? "REGISTER ADDRESS" : "REGISTER VALUE"
                                : "BK4819 DIAGNOSTICS");
    if (editing_) {
        snprintf(page.detail, sizeof(page.detail),
                 selected_ == 0 ? "Current 0x%02X / 00-7F" : "Current 0x%04X / 0000-FFFF",
                 selected_ == 0 ? address_ : value_);
        snprintf(page.rows[0].name, sizeof(page.rows[0].name), "Up/Down hex digit %c",
                 "0123456789ABCDEF"[letter_]);
    } else {
        strcpy(page.detail, "Exclusive / temporary");
        if (selected_ < 4) {
            snprintf(page.rows[0].name, sizeof(page.rows[0].name), "Address 0x%02X", address_);
            snprintf(page.rows[1].name, sizeof(page.rows[1].name), "Value 0x%04X", value_);
            strcpy(page.rows[2].name, "Read register");
            strcpy(page.rows[3].name, "Write register");
        } else {
            strcpy(page.rows[0].name, "Exit / restore radio");
        }
    }
    if (radio_ptt_requested()) {
        strcpy(page.detail, "Release PTT to continue");
    } else if (pending_) {
        strcpy(page.detail, "Applying...");
    } else if (error_) {
        if (editing_ && error_ == -ERANGE) {
            strcpy(page.detail, "Address must be 00-7F");
        } else if (editing_ && error_ == -EINVAL) {
            strcpy(page.detail, "Enter valid hex digits");
        } else if (editing_ && error_ == -ENOSPC) {
            strcpy(page.detail, "Field is full");
        } else {
            snprintf(page.detail, sizeof(page.detail), "Register error %d", error_);
        }
    }
}

void UiModel::diagnostic_actions(const char *(&actions)[4]) const {
    for (unsigned i = 0; i < 4; ++i) {
        actions[i] = "";
    }
    if (pending_ || radio_ptt_requested()) {
        return;
    }
    if (editing_) {
        actions[0] = length_ ? "OK Set" : "";
        actions[1] = "BACK Cancel";
        actions[2] = length_ ? "P1 Erase" : "";
        actions[3] = length_ < (selected_ == 0 ? 2 : 4) ? "P2 Add" : "";
    } else {
        actions[0] = selected_ < 2    ? "OK Edit"
                     : selected_ == 2 ? "OK Read"
                     : selected_ == 3 ? "OK Write"
                                      : "OK Restore";
        actions[1] = "BACK Exit";
    }
}
} // namespace ht
