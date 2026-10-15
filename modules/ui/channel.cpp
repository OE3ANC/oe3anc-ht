// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui.hpp>
#include <ht/backend.hpp>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_HT_CODEPLUG_STORAGE
#include <ht/settings.hpp>
#endif

namespace ht {
static void frequency_text(char *text, size_t size, uint32_t hz) {
    snprintf(text, size, "%u.%06u", hz / 1000000, hz % 1000000);
}

static void tone_text(char *text, size_t size, const Tone &tone) {
    if (tone.kind == ToneKind::Ctcss) {
        snprintf(text, size, "%u.%u Hz", tone.value / 10, tone.value % 10);
    } else if (tone.kind == ToneKind::Dcs) {
        snprintf(text, size, "D%03o %s", tone.value, tone.inverted ? "Inv" : "Normal");
    } else {
        snprintf(text, size, "None");
    }
}

void UiModel::open_channel(uint32_t id, UiScreen return_screen) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    const auto marker = radio_ptt_press_sequence();
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    Channel draft;
    uint32_t revision;
    if (id) {
        error_ = settings_channel(id, draft, &revision);
        if (error_) {
            return;
        }
    } else {
        settings_vfo(draft.configuration, &revision);
        error_ = settings_free_channel_number(draft.number);
        if (error_ && error_ != -ENOSPC) {
            return;
        }
        // A full store still permits an explicitly confirmed replacement.
        if (error_) {
            draft.number = 1;
        }
        snprintf(draft.name, sizeof(draft.name), "CHANNEL %03u", draft.number);
    }
    if (settings_status().revision != revision) {
        error_ = -ESTALE;
        return;
    }
    channel_draft_ = original_channel_ = draft;
    editing_vfo_ = channel_editing_ = tone_editing_ = awaiting_channel_save_ = false;
    edit_state_ = state_;
    edit_revision_ = revision;
    edit_ptt_sequence_ = marker;
    edit_return_ = return_screen;
    edit_page_ = edit_cursor_ = 0;
    edit_bank_ = return_screen == UiScreen::Channels ? browse_bank_ : state_.selection.bank_id;
    original_bank_ = edit_bank_;
    replacement_id_ = 0;
    deleting_ = duplicate_draft_ = false;
    screen_ = UiScreen::ChannelEditor;
#else
    (void)id;
    (void)return_screen;
    error_ = -ENOTSUP;
#endif
}

void UiModel::open_current_editor(UiScreen return_screen) {
    const bool vfo = state_.selection.operating == Operating::Vfo;
    open_channel(vfo ? 0 : state_.selection.channel_id, return_screen);
    if (screen_ == UiScreen::ChannelEditor) {
        editing_vfo_ = vfo;
        edit_cursor_ = vfo ? 2 : 0;
    }
}

bool UiModel::channel_row_available(unsigned position) const {
    if (editing_vfo_ && (position < 2 || position == 7)) {
        return false;
    }
    if (position == 13) {
        return editing_vfo_ || channel_draft_.id;
    }
    if (position == 14) {
        return !editing_vfo_ && channel_draft_.id;
    }
    return position < 15;
}

void UiModel::cancel_channel(bool interruption) {
    if (!channel_programming(screen_)) {
        return;
    }
    awaiting_channel_save_ = false;
    screen_ = interruption ? UiScreen::Home : edit_return_;
    text_editor_ = {};
    if (screen_ == UiScreen::Channels) {
        refresh_list();
    }
}

void UiModel::begin_channel_field(ChannelField field) {
    channel_field_ = field;
    char initial[25] = {};
    TextKind kind = TextKind::Frequency;
    const auto &op = channel_draft_.configuration;
    switch (field) {
    case ChannelField::Name:
        kind = TextKind::Name;
        strcpy(initial, channel_draft_.name);
        break;
    case ChannelField::Number:
        kind = TextKind::ChannelNumber;
        snprintf(initial, sizeof(initial), "%u", channel_draft_.number);
        break;
    case ChannelField::Rx:
        frequency_text(initial, sizeof(initial), op.rx_frequency_hz);
        break;
    case ChannelField::Tx:
        frequency_text(initial, sizeof(initial), op.tx_frequency_hz);
        break;
    case ChannelField::Station:
        kind = TextKind::Callsign;
        strcpy(initial, op.m17.callsign);
        break;
    case ChannelField::ToneValue:
        if (tone_draft_.kind == ToneKind::Dcs) {
            kind = TextKind::ChannelNumber;
            snprintf(initial, sizeof(initial), "%03o", tone_draft_.value);
        } else {
            snprintf(initial, sizeof(initial), "%03u.%u", tone_draft_.value / 10,
                     tone_draft_.value % 10);
        }
        break;
    }
    error_ = text_editor_.begin(kind, initial);
    if (!error_) {
        screen_ = UiScreen::ChannelField;
    }
}

void UiModel::channel_field(const UiInput &input) {
    if (input.key == UiKey::Back) {
        screen_ = channel_field_ == ChannelField::ToneValue ? UiScreen::ChannelTone
                                                            : UiScreen::ChannelEditor;
        text_editor_ = {};
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        text_editor_.move(input.key == UiKey::Up ? -1 : 1);
    } else if (input.key == UiKey::Star || input.key == UiKey::Erase) {
        text_editor_.erase();
    } else if (input.key == UiKey::Hash) {
        text_editor_.finish();
    } else if (input.key == UiKey::Digit) {
        error_ = text_editor_.digit(input.character,
                                    input.timestamp_ms >= 0 ? input.timestamp_ms : k_uptime_get());
    } else if (input.key == UiKey::Character) {
        error_ = text_editor_.literal(input.character);
    } else if (input.key == UiKey::Enter) {
        text_editor_.finish();
        const char *value = text_editor_.text();
        auto &op = channel_draft_.configuration;
        uint32_t parsed = 0;
        if (channel_field_ == ChannelField::Name) {
            if (!text_editor_.length()) {
                error_ = -EINVAL;
            } else {
                memset(channel_draft_.name, 0, sizeof(channel_draft_.name));
                strcpy(channel_draft_.name, value);
            }
        } else if (channel_field_ == ChannelField::Station) {
            char callsign[10] = {};
            memcpy(callsign, value, text_editor_.length());
            if (!valid_callsign(callsign)) {
                error_ = -EINVAL;
            } else {
                memcpy(op.m17.callsign, callsign, sizeof(callsign));
                op.m17.destination = Destination::Station;
            }
        } else if (channel_field_ == ChannelField::Number ||
                   (channel_field_ == ChannelField::ToneValue &&
                    tone_draft_.kind == ToneKind::Dcs)) {
            const bool octal = channel_field_ == ChannelField::ToneValue;
            if (!*value) {
                error_ = -EINVAL;
            }
            for (const char *p = value; !error_ && *p; ++p) {
                if (*p < '0' || *p > (octal ? '7' : '9')) {
                    error_ = -EINVAL;
                    break;
                }
                parsed = parsed * (octal ? 8 : 10) + (*p - '0');
            }
            if (!error_ && !octal && (!parsed || parsed > channel_capacity)) {
                error_ = -ERANGE;
            }
            if (!error_) {
                if (octal) {
                    tone_draft_.value = parsed;
                } else {
                    channel_draft_.number = parsed;
                }
            }
        } else {
            error_ = ui_parse_frequency(value, parsed);
            if (!error_ && channel_field_ == ChannelField::ToneValue) {
                // Editor unit is Hz: parsing as MHz gives exact tenths by scaling.
                if (parsed % 100000 || parsed / 100000 < 670 || parsed / 100000 > 2541) {
                    error_ = -ERANGE;
                } else {
                    tone_draft_.value = parsed / 100000;
                }
            } else if (!error_) {
                const auto &caps = backend_capabilities();
                bool supported = false;
                for (const auto &band : caps.bands) {
                    supported |= parsed >= band.min_hz && parsed <= band.max_hz;
                }
                if (!supported) {
                    error_ = -EINVAL;
                } else if (channel_field_ == ChannelField::Rx) {
                    op.rx_frequency_hz = parsed;
                } else {
                    op.tx_frequency_hz = parsed;
                }
            }
        }
        if (!error_) {
            screen_ = channel_field_ == ChannelField::ToneValue ? UiScreen::ChannelTone
                                                                : UiScreen::ChannelEditor;
            text_editor_ = {};
        }
    }
}

void UiModel::review_channel() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (settings_status().revision != edit_revision_) {
        error_ = -ESTALE;
        return;
    }
    Channel checked = channel_draft_;
    if (!checked.id) {
        checked.id = 1;
    }
    error_ = validate_channel(checked);
    RadioConfig radio = state_.config;
    const auto &op = checked.configuration;
    radio.rx_frequency_hz = op.rx_frequency_hz;
    radio.tx_frequency_hz = op.tx_frequency_hz;
    radio.tx_inhibit = op.tx_inhibit;
    radio.mode = op.mode;
    radio.power_mw = op.power_mw;
    radio.bandwidth = op.bandwidth;
    radio.squelch = op.squelch;
    radio.rx_tone = op.rx_tone;
    radio.tx_tone = op.tx_tone;
    radio.m17 = op.m17;
    if (!error_) {
        error_ = validate_config(radio);
    }
    if (error_) {
        return;
    }
    if (editing_vfo_) {
        if (state_.generation != edit_state_.generation ||
            state_.configuration_revision != edit_state_.configuration_revision ||
            !same_selection(state_.selection, edit_state_.selection)) {
            error_ = -ESTALE;
            return;
        }
        RadioCommand command;
        command.kind = CommandKind::ApplyVfo;
        command.config = radio;
        command.expected_generation = edit_state_.generation;
        command.expected_revision = edit_state_.configuration_revision;
        command.selection = edit_state_.selection;
        submit(command);
        return;
    }
    Channel occupied;
    uint32_t revision;
    const int found = settings_channel_number(channel_draft_.number, occupied, &revision);
    if (found && found != -ENOENT) {
        error_ = found;
        return;
    }
    if (!found && revision != edit_revision_) {
        error_ = -ESTALE;
        return;
    }
    replacement_id_ = 0;
    deleting_ = false;
    if (!found && occupied.id != channel_draft_.id) {
        if (channel_draft_.id) {
            error_ = -EEXIST;
            return;
        }
        replacement_id_ = occupied.id;
        replacement_number_ = occupied.number;
        strcpy(replacement_name_, occupied.name);
        screen_ = UiScreen::ChannelConfirm;
    } else {
        save_channel();
    }
#endif
}

void UiModel::save_channel() {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (!next_id_) {
        error_ = -EOVERFLOW;
        return;
    }
    if (state_.phase != RadioPhase::Receiving || radio_ptt_requested()) {
        error_ = -EBUSY;
        return;
    }
    const auto id = next_id_++;
    if (deleting_) {
        error_ = settings_delete_channel(channel_draft_.id, id, state_, edit_revision_);
    } else {
        Channel submitted = channel_draft_;
        if (replacement_id_) {
            submitted.id = replacement_id_;
        }
        error_ = settings_put_channel(submitted, id, state_, edit_revision_, edit_bank_);
    }
    if (!error_) {
        edit_pending_ = id;
    }
#endif
}

void UiModel::channel_input(const UiInput &input) {
#ifdef CONFIG_HT_CODEPLUG_STORAGE
    if (screen_ == UiScreen::ChannelField) {
        channel_field(input);
        return;
    }
    if (screen_ == UiScreen::ChannelSaved) {
        if (input.key == UiKey::Back || input.key == UiKey::Enter) {
            cancel_channel();
        }
        return;
    }
    if (input.key == UiKey::Back) {
        if (tone_editing_) {
            tone_draft_ = tone_original_;
            tone_editing_ = false;
            return;
        }
        if (channel_editing_) {
            channel_draft_.configuration = channel_field_original_;
            channel_editing_ = false;
            return;
        }
        if (screen_ == UiScreen::ChannelEditor) {
            cancel_channel();
        } else if (screen_ == UiScreen::ChannelConfirm && !deleting_) {
            screen_ = UiScreen::ChannelEditor;
        } else {
            screen_ = UiScreen::ChannelEditor;
        }
        return;
    }
    if (screen_ == UiScreen::ChannelConfirm) {
        if (input.key == UiKey::Enter) {
            save_channel();
        }
        return;
    }
    if (screen_ == UiScreen::ChannelBank) {
        if (!list_.ready || settings_status().revision != browse_revision_) {
            refresh_list();
            error_ = -EAGAIN;
            return;
        }
        if (input.key == UiKey::Up || input.key == UiKey::Down || input.key == UiKey::Left ||
            input.key == UiKey::Right) {
            browse_cursor_ = (list_.cursor + list_.count * 4 +
                              (input.key == UiKey::Down    ? 1
                               : input.key == UiKey::Up    ? -1
                               : input.key == UiKey::Right ? 4
                                                           : -4)) %
                             list_.count;
            refresh_list();
        } else if (input.key == UiKey::Enter) {
            edit_bank_ = list_.rows[list_.cursor % 4].id;
            screen_ = UiScreen::ChannelEditor;
        }
        return;
    }
    if (screen_ == UiScreen::ChannelTone) {
        if (tone_editing_) {
            if (input.key == UiKey::Enter) {
                tone_editing_ = false;
            } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
                if (tone_cursor_ == 2) {
                    tone_draft_.inverted = !tone_draft_.inverted;
                } else {
                    const auto &caps = backend_capabilities();
                    unsigned kind = unsigned(tone_draft_.kind);
                    do {
                        kind = (kind + (input.key == UiKey::Up ? 1 : 2)) % 3;
                    } while ((kind == 1 && !caps.ctcss) || (kind == 2 && !caps.dcs));
                    tone_draft_ = {static_cast<ToneKind>(kind),
                                   uint16_t(kind == 1   ? 670
                                            : kind == 2 ? 0023
                                                        : 0),
                                   false};
                }
            }
        } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
            do {
                tone_cursor_ = (tone_cursor_ + (input.key == UiKey::Down ? 1 : 3)) % 4;
            } while ((tone_cursor_ == 1 && tone_draft_.kind == ToneKind::None) ||
                     (tone_cursor_ == 2 && tone_draft_.kind != ToneKind::Dcs));
        } else if (input.key == UiKey::Enter) {
            if (tone_cursor_ == 3) {
                error_ = valid_tone(tone_draft_) ? 0 : -EINVAL;
                if (!error_) {
                    (edit_cursor_ == 2 ? channel_draft_.configuration.rx_tone
                                       : channel_draft_.configuration.tx_tone) = tone_draft_;
                    screen_ = UiScreen::ChannelEditor;
                }
            } else if (tone_cursor_ == 1) {
                begin_channel_field(ChannelField::ToneValue);
            } else {
                tone_original_ = tone_draft_;
                tone_editing_ = true;
            }
        }
        return;
    }
    if (channel_editing_) {
        if (input.key == UiKey::Enter) {
            channel_editing_ = false;
        } else if (input.key == UiKey::Up || input.key == UiKey::Down) {
            change_channel_value(input.key == UiKey::Up ? 1 : -1);
        }
        return;
    }
    if (input.key == UiKey::Left || input.key == UiKey::Right) {
        edit_page_ = (edit_page_ + (input.key == UiKey::Right ? 1 : 3)) % 4;
        edit_cursor_ = 0;
        while (!channel_row_available(edit_page_ * 4 + edit_cursor_)) {
            ++edit_cursor_;
        }
        return;
    }
    if (input.key == UiKey::Up || input.key == UiKey::Down) {
        unsigned position = edit_page_ * 4 + edit_cursor_;
        do {
            position = (position + (input.key == UiKey::Down ? 1 : 15)) % 16;
        } while (!channel_row_available(position));
        edit_page_ = position / 4;
        edit_cursor_ = position % 4;
        return;
    }
    if (input.key != UiKey::Enter) {
        return;
    }
    const unsigned position = edit_page_ * 4 + edit_cursor_;
    if ((position >= 4 && position <= 6) || position == 8 ||
        (position == 9 && channel_draft_.configuration.mode == Mode::Fm) ||
        ((position == 10 || position == 11) && channel_draft_.configuration.mode == Mode::M17)) {
        channel_field_original_ = channel_draft_.configuration;
        channel_editing_ = true;
        return;
    }
    auto &op = channel_draft_.configuration;
    switch (edit_page_ * 4 + edit_cursor_) {
    case 0:
        begin_channel_field(ChannelField::Name);
        break;
    case 1:
        begin_channel_field(ChannelField::Number);
        break;
    case 2:
        begin_channel_field(ChannelField::Rx);
        break;
    case 3:
        begin_channel_field(ChannelField::Tx);
        break;
    case 7:
        screen_ = UiScreen::ChannelBank;
        browse_cursor_ = 0;
        list_ = {};
        refresh_list();
        break;
    case 9:
        begin_channel_field(ChannelField::Station);
        break;
    case 10:
    case 11:
        if (!backend_capabilities().ctcss && !backend_capabilities().dcs) {
            error_ = -ENOTSUP;
            break;
        }
        tone_draft_ = edit_cursor_ == 2 ? op.rx_tone : op.tx_tone;
        tone_cursor_ = 0;
        screen_ = UiScreen::ChannelTone;
        break;
    case 12:
        review_channel();
        break;
    case 13: {
        if (!channel_draft_.id && !editing_vfo_) {
            error_ = -ENOTSUP;
            break;
        }
        uint16_t number;
        error_ = settings_free_channel_number(number);
        if (editing_vfo_ && error_ == -ENOSPC) {
            number = 1;
            error_ = 0;
        }
        if (!error_ && settings_status().revision != edit_revision_) {
            error_ = -ESTALE;
        }
        if (!error_) {
            channel_draft_.id = 0;
            channel_draft_.number = number;
            duplicate_draft_ = !editing_vfo_;
            editing_vfo_ = false;
            snprintf(channel_draft_.name, sizeof(channel_draft_.name), "CHANNEL %03u", number);
            edit_page_ = edit_cursor_ = 0;
        }
        break;
    }
    case 14: {
        if (!channel_draft_.id) {
            error_ = -ENOTSUP;
            break;
        }
        Channel original;
        uint32_t revision;
        error_ = settings_channel(channel_draft_.id, original, &revision);
        if (!error_ && revision != edit_revision_) {
            error_ = -ESTALE;
        }
        if (!error_) {
            deleting_ = true;
            replacement_number_ = original.number;
            strcpy(replacement_name_, original.name);
            screen_ = UiScreen::ChannelConfirm;
        }
        break;
    }
    }
#else
    (void)input;
#endif
}

void UiModel::change_channel_value(int direction) {
    auto &op = channel_draft_.configuration;
    switch (edit_page_ * 4 + edit_cursor_) {
    case 4:
        if (!backend_capabilities().m17) {
            return;
        }
        op.mode = op.mode == Mode::Fm ? Mode::M17 : Mode::Fm;
        op.rx_tone = {};
        op.tx_tone = {};
        op.m17 = {};
        break;
    case 5:
        op.tx_inhibit = !op.tx_inhibit;
        break;
    case 6: {
        const uint32_t powers[] = {1000, 2500, 5000};
        unsigned index = 0;
        while (index < 2 && powers[index] < op.power_mw) {
            ++index;
        }
        do {
            index = (index + 3 + direction) % 3;
        } while (powers[index] > backend_capabilities().max_power_mw);
        op.power_mw = powers[index];
        break;
    }
    case 8:
        if (op.mode == Mode::Fm) {
            op.bandwidth = op.bandwidth == Bandwidth::Wide ? Bandwidth::Narrow : Bandwidth::Wide;
        } else {
            op.m17.destination = op.m17.destination == Destination::Station ? Destination::Broadcast
                                                                            : Destination::Station;
            if (op.m17.destination == Destination::Broadcast) {
                memset(op.m17.callsign, 0, sizeof(op.m17.callsign));
            }
        }
        break;
    case 9:
        op.squelch = (op.squelch + 16 + direction) % 16;
        break;
    case 10:
        op.m17.can = (op.m17.can + 16 + direction) % 16;
        break;
    case 11:
        op.m17.rx_can_check = !op.m17.rx_can_check;
        break;
    }
}

void UiModel::channel_actions(const char *(&actions)[4]) const {
    if (screen_ == UiScreen::ChannelSaved) {
        actions[0] = actions[2] = actions[3] = "";
        actions[1] = "BACK Back";
        return;
    }
    actions[0] = "OK Edit";
    actions[1] = "BACK Cancel";
    actions[2] = "P1 Prev page";
    actions[3] = "P2 Next page";
    if (screen_ == UiScreen::ChannelEditor) {
        actions[0] = channel_editing_ ? "OK Accept" : "OK Select";
        if (channel_editing_) {
            actions[2] = actions[3] = "";
        }
    }
    if (screen_ == UiScreen::ChannelField) {
        actions[0] = "OK Use";
        actions[2] = actions[3] = "";
    } else if (screen_ == UiScreen::ChannelTone) {
        actions[0] = tone_editing_                            ? "OK Accept"
                     : tone_cursor_ == 3                      ? "OK Use"
                     : tone_cursor_ == 0 || tone_cursor_ == 2 ? "OK Change"
                                                              : "OK Edit";
        if ((tone_cursor_ == 1 && tone_draft_.kind == ToneKind::None) ||
            (tone_cursor_ == 2 && tone_draft_.kind != ToneKind::Dcs)) {
            actions[0] = "";
        }
        actions[2] = "";
        actions[3] = "";
    } else if (screen_ == UiScreen::ChannelBank) {
        actions[0] = list_.ready ? "OK Select" : "";
    } else if (screen_ == UiScreen::ChannelReview) {
        actions[0] = "OK Save";
        actions[1] = "BACK Edit";
        actions[3] = "";
    } else if (screen_ == UiScreen::ChannelConfirm) {
        actions[0] = deleting_ ? "OK Delete" : "OK Replace";
        actions[2] = actions[3] = "";
    } else if (screen_ == UiScreen::ChannelSaved) {
        actions[0] = "OK Done";
        actions[1] = "BACK Done";
        actions[2] = actions[3] = "";
    }
    if (edit_pending_ || awaiting_channel_save_) {
        for (auto &action : actions) {
            action = "";
        }
    }
}

void UiModel::channel_lines(char (&text)[8][32]) const {
    const auto &op = channel_draft_.configuration;
    const bool form = screen_ == UiScreen::ChannelEditor || screen_ == UiScreen::ChannelReview;
    if (form) {
        const unsigned page = screen_ == UiScreen::ChannelReview ? review_page_ : edit_page_;
        strcpy(text[0], editing_vfo_                         ? "VFO SETTINGS"
                        : screen_ == UiScreen::ChannelReview ? "REVIEW CHANNEL"
                        : channel_draft_.id                  ? "EDIT CHANNEL"
                                                             : "NEW CHANNEL");
        snprintf(text[1], 32, "%s / Page %u of %u",
                 channel_editing_ ? "Editing value" : "Select field", page + 1,
                 screen_ == UiScreen::ChannelReview ? 3 : 4);
        const char *names[4];
        char values[4][25] = {};
        switch (page) {
        case 0:
            names[0] = "Name";
            names[1] = "Number";
            names[2] = "RX frequency";
            names[3] = "TX frequency";
            snprintf(values[0], 25, "%s", channel_draft_.name);
            snprintf(values[1], 25, "%03u", channel_draft_.number);
            frequency_text(values[2], 25, op.rx_frequency_hz);
            frequency_text(values[3], 25, op.tx_frequency_hz);
            break;
        case 1:
            names[0] = "Mode";
            names[1] = "TX inhibit";
            names[2] = "Power";
            names[3] = "Bank";
            strcpy(values[0], op.mode == Mode::Fm ? "FM" : "M17");
            strcpy(values[1], op.tx_inhibit ? "On" : "Off");
            char power[16];
            ui_format_power(power, op.power_mw);
            snprintf(values[2], 25, "%s", power);
#ifdef CONFIG_HT_CODEPLUG_STORAGE
            if (edit_bank_) {
                Bank bank;
                if (!settings_bank(edit_bank_, bank)) {
                    strcpy(values[3], bank.name);
                } else {
                    strcpy(values[3], "Missing");
                }
            } else
#endif
            {
                strcpy(values[3], "None");
            }
            break;
        case 2:
            if (op.mode == Mode::Fm) {
                names[0] = "Bandwidth";
                names[1] = "Squelch";
                names[2] = "RX tone";
                names[3] = "TX tone";
                strcpy(values[0], op.bandwidth == Bandwidth::Wide ? "25 kHz" : "12.5 kHz");
                snprintf(values[1], 25, "%u", op.squelch);
                tone_text(values[2], 25, op.rx_tone);
                tone_text(values[3], 25, op.tx_tone);
            } else {
                names[0] = "Destination";
                names[1] = "Station";
                names[2] = "CAN";
                names[3] = "RX CAN check";
                strcpy(values[0],
                       op.m17.destination == Destination::Broadcast ? "Broadcast" : "Station");
                strcpy(values[1], op.m17.callsign[0] ? op.m17.callsign : "Unset");
                snprintf(values[2], 25, "%u", op.m17.can);
                strcpy(values[3], op.m17.rx_can_check ? "On" : "Off");
            }
            break;
        default:
            names[0] = editing_vfo_ ? "Apply" : "Save";
            names[1] = editing_vfo_ ? "Save as channel" : channel_draft_.id ? "Duplicate" : "";
            names[2] = channel_draft_.id ? "Delete channel" : "";
            names[3] = "";
        }
        for (unsigned row = 0; row < 4; ++row) {
            if (!names[row][0] || !channel_row_available(page * 4 + row)) {
                continue;
            }
            snprintf(text[row + 2], 32, "%c %s%s%.*s", edit_cursor_ == row ? '>' : ' ', names[row],
                     values[row][0] ? ": " : "", int(27 - strlen(names[row])), values[row]);
        }
    } else if (screen_ == UiScreen::ChannelTone) {
        strcpy(text[0], edit_cursor_ == 2 ? "RX TONE" : "TX TONE");
        strcpy(text[1], tone_editing_ ? "Editing / Up-Down changes" : "Select field");
        snprintf(text[2], 32, "%c Type: %s", tone_cursor_ == 0 ? '>' : ' ',
                 tone_draft_.kind == ToneKind::None    ? "None"
                 : tone_draft_.kind == ToneKind::Ctcss ? "CTCSS"
                                                       : "DCS");
        char value[13];
        tone_text(value, sizeof(value), tone_draft_);
        snprintf(text[3], 32, "%c Value: %s", tone_cursor_ == 1 ? '>' : ' ', value);
        snprintf(text[4], 32, "%c %s", tone_cursor_ == 2 ? '>' : ' ',
                 tone_draft_.kind == ToneKind::Dcs
                     ? tone_draft_.inverted ? "Polarity: Inverted" : "Polarity: Normal"
                     : "");
        snprintf(text[5], 32, "%c Use tone", tone_cursor_ == 3 ? '>' : ' ');
    } else if (screen_ == UiScreen::ChannelField) {
        static const char *titles[] = {"CHANNEL NAME", "CHANNEL NUMBER",  "RX FREQUENCY",
                                       "TX FREQUENCY", "M17 DESTINATION", "TONE VALUE"};
        strcpy(text[0], titles[static_cast<unsigned>(channel_field_)]);
        strcpy(text[1], text_editor_.kind() == TextKind::Frequency
                            ? channel_field_ == ChannelField::ToneValue ? "CTCSS / Hz / one decimal"
                                                                        : "Direct entry / MHz"
                        : text_editor_.kind() == TextKind::ChannelNumber
                            ? channel_field_ == ChannelField::ToneValue ? "DCS / three octal digits"
                                                                        : "Number 1-256"
                            : "Keypad multi-tap / # Next");
        strcpy(text[2], text_editor_.text());
        strcpy(text[4], "Up/Down cursor / # Next");
    } else if (screen_ == UiScreen::ChannelConfirm) {
        strcpy(text[0], deleting_ ? "DELETE CHANNEL?" : "REPLACE CHANNEL?");
        snprintf(text[1], 32, "Affected channel %03u", replacement_number_);
        strcpy(text[2], replacement_name_);
        strcpy(text[3], deleting_ ? "Remove from every bank" : "Replace saved settings");
        strcpy(text[5], "Confirm with green OK");
    } else if (screen_ == UiScreen::ChannelSaved) {
        strcpy(text[0], edit_save_error_ ? "DELETION NOT SAVED" : "DELETION PENDING");
        strcpy(text[1], "Keep power on");
        strcpy(text[2], replacement_name_);
        strcpy(text[5], edit_save_error_ ? "Storage error / unsaved" : "Saving deletion");
    }
    if (error_ && screen_ != UiScreen::ChannelSaved) {
        const char *message = "Check field / radio settings";
        switch (error_) {
        case -ENOSPC:
            message = "Full / choose replacement";
            break;
        case -ESTALE:
            message = "Changed; cancel and reopen";
            break;
        case -EBUSY:
            message = "Radio busy; try again";
            break;
        case -EROFS:
            message = "Storage is read-only";
            break;
        case -EEXIST:
            message = "Number occupied; choose free";
            break;
        case -ENOTSUP:
            message = "Unavailable on this target";
            break;
        case -ERANGE:
            message = "Value out of range";
            break;
        }
        if (screen_ == UiScreen::ChannelField) {
            if (error_ == -ENOSPC) {
                message = "Field is full";
            } else if (error_ == -EINVAL || error_ == -ERANGE) {
                switch (channel_field_) {
                case ChannelField::Name:
                    message = "Enter a channel name";
                    break;
                case ChannelField::Number:
                    message = "Use channel number 1-256";
                    break;
                case ChannelField::Station:
                    message = "Use a valid station callsign";
                    break;
                case ChannelField::ToneValue:
                    message = tone_draft_.kind == ToneKind::Dcs ? "Use octal digits 0-7"
                                                                : "CTCSS: 67.0-254.1 Hz";
                    break;
                default:
                    message = "Use a supported frequency";
                    break;
                }
            }
        }
        snprintf(text[6], 32, "%s", message);
    }
}
} // namespace ht
