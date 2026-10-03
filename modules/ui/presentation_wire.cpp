// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_contract.hpp>
#include <ht/ui_presentation_wire.hpp>
#include <string.h>

namespace ht {
namespace {
enum class Layout { None, System, Home, Menu, Status, Appearance, List, Hex, Text, Form };

Layout layout(const UiPresentation &v) {
    if (v.system_visible) {
        return Layout::System;
    }
    if (v.home.visible) {
        return Layout::Home;
    }
    const auto s = v.screen;
    if (s == UiScreen::Menu) {
        return Layout::Menu;
    }
    if (s == UiScreen::Status || s == UiScreen::CompanionExit) {
        return Layout::Status;
    }
    if (s == UiScreen::Appearance) {
        return Layout::Appearance;
    }
    if (s == UiScreen::Diagnostics) {
        return v.diagnostic_editing ? Layout::Hex : Layout::List;
    }
    if (s == UiScreen::Frequency || s == UiScreen::Callsign || s == UiScreen::ChannelNumber ||
        s == UiScreen::ChannelField || s == UiScreen::BankName) {
        return Layout::Text;
    }
    if (s == UiScreen::Channels || s == UiScreen::Banks || s == UiScreen::ChannelBank ||
        s == UiScreen::BankMembers || s == UiScreen::BankAdd) {
        return Layout::List;
    }
    if (channel_programming(s) || bank_programming(s) || s == UiScreen::VfoStep ||
        s == UiScreen::QuickControls || s == UiScreen::Backlight || s == UiScreen::TransmitLimit) {
        return Layout::Form;
    }
    return Layout::None;
}

struct Writer {
    uint8_t *bytes;
    size_t capacity, offset = 0;
    bool ok = true;

    void number(uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (offset >= capacity) {
                ok = false;
                return;
            }
            bytes[offset++] = value >> (i * 8);
        }
    }

    template <size_t N> bool text(const char (&value)[N]) {
        size_t length = 0;
        while (length < N && value[length]) {
            if (value[length] < 32 || value[length] > 126) {
                ok = false;
                return false;
            }
            ++length;
        }
        if (length == N || offset + length + 1 > capacity) {
            ok = false;
            return false;
        }
        number(length, 1);
        memcpy(bytes + offset, value, length);
        offset += length;
        return ok;
    }
};

struct Reader {
    const uint8_t *bytes;
    size_t length, offset = 0;
    bool ok = true;

    uint32_t number(unsigned count) {
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (offset >= length) {
                ok = false;
                return 0;
            }
            value |= uint32_t(bytes[offset++]) << (i * 8);
        }
        return value;
    }

    template <size_t N> bool text(char (&value)[N]) {
        const auto size = number(1);
        if (!ok || size >= N || offset + size > length) {
            ok = false;
            return false;
        }
        for (size_t i = 0; i < size; ++i) {
            if (bytes[offset + i] < 32 || bytes[offset + i] > 126) {
                ok = false;
                return false;
            }
        }
        memcpy(value, bytes + offset, size);
        value[size] = 0;
        offset += size;
        return true;
    }
};

// One field order for reader and writer; inactive pages/drafts never travel.
template <typename View, typename Codec> bool strings(View &v, Layout kind, Codec &c) {
    for (auto &action : v.actions) {
        if (!c.text(action)) {
            return false;
        }
    }
    if (kind == Layout::System || kind == Layout::Status) {
        if (!c.text(v.status.title) || !c.text(v.status.detail)) {
            return false;
        }
        for (auto &row : v.status.rows) {
            if (!c.text(row)) {
                return false;
            }
        }
    } else if (kind == Layout::Home) {
        if (!c.text(v.home.identity) || !c.text(v.home.name) || !c.text(v.home.context) ||
            !c.text(v.home.frequency) || !c.text(v.home.settings) || !c.text(v.home.activity) ||
            !c.text(v.home.battery) || !c.text(v.home.mode)) {
            return false;
        }
    } else if (kind == Layout::Menu || kind == Layout::List || kind == Layout::Hex) {
        if (!c.text(v.list.title) || !c.text(v.list.detail)) {
            return false;
        }
        for (auto &row : v.list.rows) {
            if (!c.text(row.name) || !c.text(row.prefix) || !c.text(row.suffix)) {
                return false;
            }
        }
        if (kind == Layout::Hex) {
            if (!c.text(v.diagnostic_text)) {
                return false;
            }
        } else if (!c.text(v.lines[6])) {
            return false;
        }
    } else if (kind == Layout::Appearance || kind == Layout::Text || kind == Layout::Form) {
        if (!c.text(v.lines[1]) || !c.text(v.lines[6])) {
            return false;
        }
        if (kind == Layout::Text) {
            if (!c.text(v.lines[0]) || !c.text(v.lines[4]) || !c.text(v.text.value)) {
                return false;
            }
        } else if (kind == Layout::Form) {
            if (!c.text(v.lines[0])) {
                return false;
            }
            for (unsigned i = 2; i < 6; ++i) {
                if (!c.text(v.lines[i])) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool valid(const UiPresentation &v, Layout kind) {
    if (unsigned(v.screen) > companion::UI_SCREEN_COMPANION_EXIT ||
        unsigned(v.list_return) > companion::UI_SCREEN_COMPANION_EXIT ||
        v.preferences.theme > Theme::Darcula || v.preferences.contrast > Contrast::Maximum ||
        (v.system_visible && v.home.visible) || v.form_cursor > 3 ||
        (v.motion && !v.preferences.animations)) {
        return false;
    }
    if (kind == Layout::System || kind == Layout::Status) {
        if (v.status.color > UiStatusColor::Red) {
            return false;
        }
    }
    if (kind == Layout::Home) {
        if (v.home.bars > 5 || v.home.status > UiStatusColor::Red ||
            v.home.context_color > UiStatusColor::Red ||
            v.home.battery_color > UiStatusColor::Red ||
            (strcmp(v.home.mode, "FM") && strcmp(v.home.mode, "M17"))) {
            return false;
        }
    }
    if (kind == Layout::Menu || kind == Layout::List || kind == Layout::Hex) {
        if (v.list.count > 256 ||
            (v.list.count ? v.list.cursor >= v.list.count : v.list.cursor != 0)) {
            return false;
        }
    }
    if (kind == Layout::Text) {
        if (v.text.kind > TextKind::ChannelNumber) {
            return false;
        }
        const unsigned capacity[] = {24, 9, 11, 3};
        const auto length = strlen(v.text.value);
        if (length > capacity[unsigned(v.text.kind)] || v.text.cursor > length ||
            (v.text.pending && !v.text.cursor)) {
            return false;
        }
    }
    return true;
}
} // namespace

int ui_encode_presentation(const UiPresentation &v, uint8_t *bytes, size_t capacity,
                           size_t &length) {
    if (!bytes) {
        return -EINVAL;
    }
    const auto kind = layout(v);
    Writer w{bytes, capacity};
    const unsigned flags =
        v.motion | (v.error << 1) | (v.system_visible << 2) | (v.home.visible << 3) |
        ((kind == Layout::Home && v.home.transmitting) << 4) |
        ((kind == Layout::Home && v.home.locked) << 5) |
        ((v.screen == UiScreen::Diagnostics && v.diagnostic_editing) << 6) |
        ((v.screen == UiScreen::QuickControls && v.quick_available) << 7) |
        ((kind == Layout::Text && v.text.pending) << 8) | (v.preferences.animations << 9);
    w.number(1, 1);
    w.number(unsigned(v.screen), 1);
    w.number(unsigned(v.preferences.theme), 1);
    w.number(unsigned(v.preferences.contrast), 1);
    w.number(flags, 2);
    w.number(v.form_cursor, 1);
    w.number(unsigned(v.list_return), 1);
    w.number(v.interruptions, 4);
    w.number(v.ptt_sequence, 4);
    w.number(v.monitor_sequence, 4);
    if (!w.ok || !strings(v, kind, w) || !valid(v, kind)) {
        return -EINVAL;
    }
    if (kind == Layout::System || kind == Layout::Status) {
        w.number(unsigned(v.status.color), 1);
    }
    if (kind == Layout::Home) {
        w.number(unsigned(v.home.status), 1);
        w.number(unsigned(v.home.context_color), 1);
        w.number(unsigned(v.home.battery_color), 1);
        w.number(v.home.bars, 1);
    }
    if (kind == Layout::Menu || kind == Layout::List || kind == Layout::Hex) {
        w.number(v.list.cursor, 2);
        w.number(v.list.count, 2);
        w.number(v.list.ready, 1);
    }
    if (kind == Layout::Text) {
        w.number(unsigned(v.text.kind), 1);
        w.number(v.text.cursor, 1);
    }
    if (!w.ok || w.offset > companion::UI_MAX_BYTES) {
        return -ENOSPC;
    }
    length = w.offset;
    return 0;
}

int ui_decode_presentation(const uint8_t *bytes, size_t length, UiPresentation &v) {
    if (!bytes || length < 24 || length > companion::UI_MAX_BYTES) {
        return -EINVAL;
    }
    Reader r{bytes, length};
    v = {};
    if (r.number(1) != 1) {
        return -EINVAL;
    }
    v.screen = static_cast<UiScreen>(r.number(1));
    v.preferences.theme = static_cast<Theme>(r.number(1));
    v.preferences.contrast = static_cast<Contrast>(r.number(1));
    const auto flags = r.number(2);
    if (flags > 1023) {
        return -EINVAL;
    }
    v.motion = flags & 1;
    v.error = flags & 2;
    v.system_visible = flags & 4;
    v.home.visible = flags & 8;
    v.home.transmitting = flags & 16;
    v.home.locked = flags & 32;
    v.diagnostic_editing = flags & 64;
    v.quick_available = flags & 128;
    v.text.pending = flags & 256;
    v.preferences.animations = flags & 512;
    v.form_cursor = r.number(1);
    v.list_return = static_cast<UiScreen>(r.number(1));
    v.interruptions = r.number(4);
    v.ptt_sequence = r.number(4);
    v.monitor_sequence = r.number(4);
    const auto kind = layout(v);
    if (!strings(v, kind, r)) {
        return -EINVAL;
    }
    if (kind == Layout::System || kind == Layout::Status) {
        v.status.color = static_cast<UiStatusColor>(r.number(1));
    }
    if (kind == Layout::Home) {
        v.home.status = static_cast<UiStatusColor>(r.number(1));
        v.home.context_color = static_cast<UiStatusColor>(r.number(1));
        v.home.battery_color = static_cast<UiStatusColor>(r.number(1));
        v.home.bars = r.number(1);
    }
    if (kind == Layout::Menu || kind == Layout::List || kind == Layout::Hex) {
        v.list.cursor = r.number(2);
        v.list.count = r.number(2);
        const auto ready = r.number(1);
        if (ready > 1) {
            return -EINVAL;
        }
        v.list.ready = ready;
    }
    if (kind == Layout::Text) {
        v.text.kind = static_cast<TextKind>(r.number(1));
        v.text.cursor = r.number(1);
    }
    if (!r.ok || r.offset != length || !valid(v, kind)) {
        return -EINVAL;
    }
    // Flags for absent sections are rejected, rather than reinterpreted later.
    if ((v.home.transmitting || v.home.locked) && kind != Layout::Home) {
        return -EINVAL;
    }
    if (v.diagnostic_editing && v.screen != UiScreen::Diagnostics) {
        return -EINVAL;
    }
    if (v.quick_available && v.screen != UiScreen::QuickControls) {
        return -EINVAL;
    }
    if (v.text.pending && kind != Layout::Text) {
        return -EINVAL;
    }
    return 0;
}

int UiSnapshotStore::publish(const UiPresentation &view, int64_t now) {
    size_t size = 0;
    error_ = ui_encode_presentation(view, staging_, sizeof(staging_), size);
    if (error_) {
        return error_;
    }
    if (size != size_ || memcmp(staging_, bytes_, size)) {
        if (revision_ == UINT32_MAX) {
            return error_ = -EOVERFLOW;
        }
        memcpy(bytes_, staging_, size);
        size_ = size;
        ++revision_;
    }
    updated_ = now;
    return 0;
}

int UiSnapshotStore::copy(int64_t now, uint8_t *bytes, size_t capacity, size_t &length,
                          uint32_t &revision) const {
    if (error_) {
        return error_;
    }
    if (now < updated_ || now - updated_ >= companion::UI_STALE_MS) {
        return -ESTALE;
    }
    if (!bytes || capacity < size_) {
        return -ENOSPC;
    }
    memcpy(bytes, bytes_, size_);
    length = size_;
    revision = revision_;
    return 0;
}
} // namespace ht
