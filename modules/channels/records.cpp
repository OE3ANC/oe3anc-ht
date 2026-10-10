// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/codeplug_records.hpp>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

namespace ht {
namespace {
enum class Kind : uint8_t { Manifest = 1, Channel = 2, Bank = 3 };
constexpr uint8_t version = 1;

struct Writer {
    uint8_t *p;

    void u8(uint8_t v) {
        *p++ = v;
    }

    void u16(uint16_t v) {
        sys_put_le16(v, p);
        p += 2;
    }

    void u32(uint32_t v) {
        sys_put_le32(v, p);
        p += 4;
    }

    void string(const char *s, size_t n) {
        memset(p, 0, n);
        memcpy(p, s, strlen(s)); // All callers validate bounded NUL termination first.
        p += n;
    }
};

struct Reader {
    const uint8_t *p;
    const uint8_t *end;
    bool valid = true;

    uint8_t u8() {
        if (p == end) {
            valid = false;
            return 0;
        }
        return *p++;
    }

    uint16_t u16() {
        const uint16_t lo = u8();
        return lo | (uint16_t(u8()) << 8);
    }

    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | (uint32_t(u16()) << 16);
    }

    bool boolean() {
        const uint8_t v = u8();
        valid &= v <= 1;
        return v != 0;
    }

    void zero() {
        valid &= u8() == 0;
    }

    void string(char *s, size_t n) {
        bool ended = false;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t v = u8();
            if (ended && v) {
                valid = false;
            }
            s[i] = static_cast<char>(v);
            ended |= v == 0;
        }
        valid &= ended;
    }

    bool done() const {
        return valid && p == end;
    }
};

void put_tone(Writer &w, const Tone &tone) {
    w.u8(static_cast<uint8_t>(tone.kind));
    w.u8(tone.inverted);
    w.u16(tone.value);
}

Tone get_tone(Reader &r) {
    Tone t;
    t.kind = static_cast<ToneKind>(r.u8());
    t.inverted = r.boolean();
    t.value = r.u16();
    return t;
}

void put_operating(Writer &w, const OperatingConfig &c) {
    w.u8(static_cast<uint8_t>(c.mode));
    w.u8(static_cast<uint8_t>(c.bandwidth));
    w.u8(c.squelch);
    w.u8(c.tx_inhibit);
    w.u32(c.rx_frequency_hz);
    w.u32(c.tx_frequency_hz);
    w.u32(c.power_mw);
    put_tone(w, c.rx_tone);
    put_tone(w, c.tx_tone);
    w.u8(static_cast<uint8_t>(c.m17.destination));
    w.u8(c.m17.can);
    w.u8(c.m17.rx_can_check);
    w.u8(0);
    w.string(c.m17.callsign, sizeof(c.m17.callsign));
    w.u16(0);
}

OperatingConfig get_operating(Reader &r) {
    OperatingConfig c;
    c.mode = static_cast<Mode>(r.u8());
    c.bandwidth = static_cast<Bandwidth>(r.u8());
    c.squelch = r.u8();
    c.tx_inhibit = r.boolean();
    c.rx_frequency_hz = r.u32();
    c.tx_frequency_hz = r.u32();
    c.power_mw = r.u32();
    c.rx_tone = get_tone(r);
    c.tx_tone = get_tone(r);
    c.m17.destination = static_cast<Destination>(r.u8());
    c.m17.can = r.u8();
    c.m17.rx_can_check = r.boolean();
    r.zero();
    r.string(c.m17.callsign, sizeof(c.m17.callsign));
    r.zero();
    r.zero();
    return c;
}

void put_global(Writer &w, const GlobalSettings &g) {
    w.string(g.local_callsign, sizeof(g.local_callsign));
    w.u8(g.gain);
    w.u16(g.transmit_limit_s);
    w.u8(static_cast<uint8_t>(g.ui.theme));
    w.u8(static_cast<uint8_t>(g.ui.contrast));
    w.u8(g.ui.animations);
    w.u8(g.ui.brightness_percent);
    w.u8(g.ui.idle_s);
    w.u8(g.ui.dim_percent);
}

GlobalSettings get_global(Reader &r) {
    GlobalSettings g;
    r.string(g.local_callsign, sizeof(g.local_callsign));
    g.gain = r.u8();
    g.transmit_limit_s = r.u16();
    const uint8_t theme = r.u8();
    const uint8_t contrast = r.u8();
    // Stored preference IDs can outlive a firmware's available palettes. The
    // rest of the codeplug remains recoverable; JSON imports stay strict.
    g.ui.theme = theme <= static_cast<uint8_t>(Theme::TerminalIce) ? static_cast<Theme>(theme)
                                                                   : Theme::Midnight;
    g.ui.contrast = contrast <= static_cast<uint8_t>(Contrast::Maximum)
                        ? static_cast<Contrast>(contrast)
                        : Contrast::Normal;
    g.ui.animations = r.boolean();
    g.ui.brightness_percent = r.u8();
    g.ui.idle_s = r.u8();
    g.ui.dim_percent = r.u8();
    return g;
}

void put_selection(Writer &w, const Selection &s) {
    w.u8(static_cast<uint8_t>(s.operating));
    w.u32(s.bank_id);
    w.u32(s.channel_id);
}

Selection get_selection(Reader &r) {
    Selection s;
    s.operating = static_cast<Operating>(r.u8());
    s.bank_id = r.u32();
    s.channel_id = r.u32();
    return s;
}

bool ids_valid(const uint32_t *ids, size_t count, uint32_t high_water) {
    for (size_t i = 0; i < count; ++i) {
        if (!ids[i] || ids[i] > high_water) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (ids[i] == ids[j]) {
                return false;
            }
        }
    }
    return true;
}

bool has_id(const uint32_t *ids, size_t count, uint32_t id) {
    for (size_t i = 0; i < count; ++i) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

int manifest_valid(const CodeplugManifest &m) {
    if (m.channel_count > channel_capacity || m.bank_count > bank_capacity ||
        validate_global(m.global) || validate_operating(m.vfo) ||
        !ids_valid(m.channel_ids, m.channel_count, m.channel_id_high_water) ||
        !ids_valid(m.bank_ids, m.bank_count, m.bank_id_high_water) ||
        (m.selection.operating != Operating::Vfo && m.selection.operating != Operating::Memory) ||
        (m.selection.operating == Operating::Memory && !m.selection.channel_id) ||
        (m.selection.channel_id &&
         !has_id(m.channel_ids, m.channel_count, m.selection.channel_id)) ||
        (m.selection.bank_id && !has_id(m.bank_ids, m.bank_count, m.selection.bank_id))) {
        return -EINVAL;
    }
    return 0;
}

uint32_t checksum(const uint8_t *b, size_t n) {
    return crc32_ieee_update(crc32_ieee(b, 12), b + codeplug_envelope_size,
                             n - codeplug_envelope_size);
}

int begin(uint8_t *b, size_t capacity, size_t n, Kind kind, uint32_t generation) {
    if (!b || !generation) {
        return -EINVAL;
    }
    if (capacity < n) {
        return -ENOSPC;
    }
    memcpy(b, "HTDB", 4);
    b[4] = version;
    b[5] = static_cast<uint8_t>(kind);
    sys_put_le16(n, b + 6);
    sys_put_le32(generation, b + 8);
    sys_put_le32(0, b + 12);
    return 0;
}

int check(const uint8_t *b, size_t n, Kind kind, uint32_t generation) {
    if (!b || !generation) {
        return -EINVAL;
    }
    if (n < codeplug_envelope_size || n > codeplug_record_max || memcmp(b, "HTDB", 4) ||
        sys_get_le16(b + 6) != n || checksum(b, n) != sys_get_le32(b + 12)) {
        return -EBADMSG;
    }
    if (b[4] != version) {
        return -ENOTSUP;
    }
    return b[5] != static_cast<uint8_t>(kind) || sys_get_le32(b + 8) != generation ? -EBADMSG : 0;
}

void finish(uint8_t *b, size_t n, size_t &written) {
    sys_put_le32(checksum(b, n), b + 12);
    written = n;
}
} // namespace

int make_manifest(const Codeplug &p, CodeplugManifest &m) {
    const int error = validate_codeplug(p);
    if (error) {
        return error;
    }
    m.channel_id_high_water = p.channel_id_high_water;
    m.bank_id_high_water = p.bank_id_high_water;
    m.global = p.global;
    m.vfo = p.vfo;
    m.selection = p.selection;
    m.channel_count = p.channel_count;
    m.bank_count = p.bank_count;
    memset(m.channel_ids, 0, sizeof(m.channel_ids));
    memset(m.bank_ids, 0, sizeof(m.bank_ids));
    for (size_t i = 0; i < p.channel_count; ++i) {
        m.channel_ids[i] = p.channels[i].id;
    }
    for (size_t i = 0; i < p.bank_count; ++i) {
        m.bank_ids[i] = p.banks[i].id;
    }
    return 0;
}

int encode_manifest(const CodeplugManifest &m, uint32_t generation, uint8_t *b, size_t capacity,
                    size_t &written) {
    const int validation = manifest_valid(m);
    if (validation) {
        return validation;
    }
    const size_t n = codeplug_envelope_size + 83 + 4 * (m.channel_count + m.bank_count);
    const int error = begin(b, capacity, n, Kind::Manifest, generation);
    if (error) {
        return error;
    }
    Writer w{b + codeplug_envelope_size};
    w.u32(m.channel_id_high_water);
    w.u32(m.bank_id_high_water);
    w.u16(m.channel_count);
    w.u8(m.bank_count);
    put_global(w, m.global);
    put_selection(w, m.selection);
    put_operating(w, m.vfo);
    for (size_t i = 0; i < m.channel_count; ++i) {
        w.u32(m.channel_ids[i]);
    }
    for (size_t i = 0; i < m.bank_count; ++i) {
        w.u32(m.bank_ids[i]);
    }
    w.u32(m.global.vfo_step_hz);
    finish(b, n, written);
    return 0;
}

int decode_manifest(const uint8_t *b, size_t n, uint32_t generation, CodeplugManifest &out) {
    const int error = check(b, n, Kind::Manifest, generation);
    if (error) {
        return error;
    }
    CodeplugManifest m;
    Reader r{b + codeplug_envelope_size, b + n};
    m.channel_id_high_water = r.u32();
    m.bank_id_high_water = r.u32();
    m.channel_count = r.u16();
    m.bank_count = r.u8();
    if (m.channel_count > channel_capacity || m.bank_count > bank_capacity) {
        return -EBADMSG;
    }
    m.global = get_global(r);
    m.selection = get_selection(r);
    m.vfo = get_operating(r);
    for (size_t i = 0; i < m.channel_count; ++i) {
        m.channel_ids[i] = r.u32();
    }
    for (size_t i = 0; i < m.bank_count; ++i) {
        m.bank_ids[i] = r.u32();
    }
    m.global.vfo_step_hz = r.u32();
    if (!r.done() || manifest_valid(m)) {
        return -EBADMSG;
    }
    out = m;
    return 0;
}

int encode_channel(const Channel &c, uint32_t generation, uint8_t *b, size_t capacity,
                   size_t &written) {
    const int validation = validate_channel(c);
    if (validation) {
        return validation;
    }
    const int error = begin(b, capacity, channel_record_size, Kind::Channel, generation);
    if (error) {
        return error;
    }
    Writer w{b + codeplug_envelope_size};
    w.u32(c.id);
    w.u16(c.number);
    w.string(c.name, sizeof(c.name));
    put_operating(w, c.configuration);
    finish(b, channel_record_size, written);
    return 0;
}

int decode_channel(const uint8_t *b, size_t n, uint32_t generation, Channel &out) {
    const int error = check(b, n, Kind::Channel, generation);
    if (error) {
        return error;
    }
    Channel c;
    Reader r{b + codeplug_envelope_size, b + n};
    c.id = r.u32();
    c.number = r.u16();
    r.string(c.name, sizeof(c.name));
    c.configuration = get_operating(r);
    if (!r.done() || validate_channel(c)) {
        return -EBADMSG;
    }
    out = c;
    return 0;
}

int encode_bank(const Bank &bank, uint32_t generation, uint8_t *b, size_t capacity,
                size_t &written) {
    const int validation = validate_bank(bank);
    if (validation || !bank.id) {
        return validation ? validation : -EINVAL;
    }
    const size_t n = codeplug_envelope_size + 4 + radio_name_size + 2 + 4 * bank.count;
    const int error = begin(b, capacity, n, Kind::Bank, generation);
    if (error) {
        return error;
    }
    Writer w{b + codeplug_envelope_size};
    w.u32(bank.id);
    w.string(bank.name, sizeof(bank.name));
    w.u16(bank.count);
    for (size_t i = 0; i < bank.count; ++i) {
        w.u32(bank.channel_ids[i]);
    }
    finish(b, n, written);
    return 0;
}

int decode_bank(const uint8_t *b, size_t n, uint32_t generation, Bank &out) {
    const int error = check(b, n, Kind::Bank, generation);
    if (error) {
        return error;
    }
    Bank bank;
    Reader r{b + codeplug_envelope_size, b + n};
    bank.id = r.u32();
    r.string(bank.name, sizeof(bank.name));
    bank.count = r.u16();
    if (bank.count > channel_capacity) {
        return -EBADMSG;
    }
    for (size_t i = 0; i < bank.count; ++i) {
        bank.channel_ids[i] = r.u32();
    }
    if (!r.done() || !bank.id || validate_bank(bank)) {
        return -EBADMSG;
    }
    out = bank;
    return 0;
}
} // namespace ht
