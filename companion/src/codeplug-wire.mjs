// SPDX-License-Identifier: GPL-3.0-or-later
import { validate, canonical, FORMAT, SCHEMA_VERSION } from './codeplug.mjs';
import { C, crc32 } from './protocol.mjs';

const themes = ['midnight', 'nord', 'solarized-dark', 'darcula'];
const contrasts = ['normal', 'high', 'maximum'];

function reject(reason) {
    throw new Error(`Radio codeplug: ${reason}`);
}

class Writer {
    constructor(size) {
        this.bytes = new Uint8Array(size);
        this.view = new DataView(this.bytes.buffer);
        this.at = 0;
    }

    u8(value) {
        this.view.setUint8(this.at++, value);
    }

    u16(value) {
        this.view.setUint16(this.at, value, true);
        this.at += 2;
    }

    u32(value) {
        this.view.setUint32(this.at, value, true);
        this.at += 4;
    }

    string(value, size) {
        this.bytes.set(new TextEncoder().encode(value), this.at);
        this.at += size;
    }

    append(bytes) {
        this.bytes.set(bytes, this.at);
        this.at += bytes.length;
    }
}

class Reader {
    constructor(bytes) {
        this.bytes = bytes;
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        this.at = 0;
    }

    get(size) {
        if (size > this.bytes.length - this.at) {
            reject('truncated record');
        }
        const bytes = this.bytes.subarray(this.at, this.at + size);
        this.at += size;
        return bytes;
    }

    u8() {
        return this.get(1)[0];
    }

    u16() {
        const at = this.at;
        this.get(2);
        return this.view.getUint16(at, true);
    }

    u32() {
        const at = this.at;
        this.get(4);
        return this.view.getUint32(at, true);
    }

    bool() {
        const value = this.u8();
        if (value > 1) {
            reject('invalid boolean');
        }
        return !!value;
    }

    enum(choices) {
        const value = this.u8();
        if (value >= choices.length) {
            reject('unknown enum');
        }
        return choices[value];
    }

    zero(size) {
        if (this.get(size).some(value => value)) {
            reject('nonzero reserved bytes');
        }
    }

    string(size) {
        const bytes = this.get(size);
        const end = bytes.indexOf(0);
        if (
            end < 0 ||
            bytes.subarray(end).some(value => value) ||
            bytes.subarray(0, end).some(value => value > 127)
        ) {
            reject('invalid string/padding');
        }
        return String.fromCharCode(...bytes.subarray(0, end));
    }

    done() {
        if (this.at !== this.bytes.length) {
            reject('trailing bytes');
        }
    }
}

function writeTone(w, tone) {
    w.u8(['none', 'ctcss', 'dcs'].indexOf(tone.kind));
    w.u8(tone.polarity === 'inverted');
    w.u16(tone.kind === 'dcs' ? parseInt(tone.code, 8) : (tone.tenths_hz ?? 0));
}

function readTone(r) {
    const kind = r.enum(['none', 'ctcss', 'dcs']);
    const inverted = r.bool();
    const value = r.u16();
    if (kind === 'none') {
        if (inverted || value) {
            reject('disabled tone has data');
        }
        return { kind };
    }
    if (kind === 'ctcss') {
        if (inverted) {
            reject('inverted CTCSS');
        }
        return { kind, tenths_hz: value };
    }
    return {
        kind,
        code: value.toString(8).padStart(3, '0'),
        polarity: inverted ? 'inverted' : 'normal'
    };
}

function writeOperating(w, op) {
    w.u8(op.mode === 'm17');
    w.u8(op.bandwidth === 'wide');
    w.u8(op.squelch);
    w.u8(op.tx_inhibit);
    w.u32(op.rx_frequency_hz);
    w.u32(op.tx_frequency_hz);
    w.u32(op.power_mw);
    writeTone(w, op.fm?.rx_tone ?? { kind: 'none' });
    writeTone(w, op.fm?.tx_tone ?? { kind: 'none' });
    const m17 = op.m17;
    const destination = m17?.destination;
    w.u8(destination?.kind === 'station');
    w.u8(m17?.can ?? 0);
    w.u8(m17?.rx_can_check ?? false);
    w.u8(0);
    w.string(destination?.callsign ?? '', 10);
    w.u16(0);
}

function readOperating(r) {
    const mode = r.enum(['fm', 'm17']);
    const bandwidth = r.enum(['narrow', 'wide']);
    const squelch = r.u8();
    const tx_inhibit = r.bool();
    const rx_frequency_hz = r.u32();
    const tx_frequency_hz = r.u32();
    const power_mw = r.u32();
    const rx_tone = readTone(r);
    const tx_tone = readTone(r);
    const kind = r.enum(['broadcast', 'station']);
    const can = r.u8();
    const rx_can_check = r.bool();
    r.zero(1);
    const callsign = r.string(10);
    r.zero(2);
    if (kind === 'broadcast' && callsign) {
        reject('broadcast destination has a callsign');
    }
    const op = { mode, bandwidth, squelch, tx_inhibit, rx_frequency_hz, tx_frequency_hz, power_mw };
    if (mode === 'fm') {
        if (kind !== 'broadcast' || can || rx_can_check) {
            reject('inactive M17 data');
        }
        op.fm = { rx_tone, tx_tone };
    } else {
        if (rx_tone.kind !== 'none' || tx_tone.kind !== 'none') {
            reject('inactive FM data');
        }
        op.m17 = {
            destination: kind === 'station' ? { kind, callsign } : { kind },
            can,
            rx_can_check
        };
    }
    return op;
}

function record(kind, payload) {
    const w = new Writer(16 + payload.length);
    w.append(Uint8Array.of(72, 84, 68, 66));
    w.u8(1);
    w.u8(kind);
    w.u16(w.bytes.length);
    // Transfer records use generation 1; the radio assigns the stored generation.
    w.u32(1);
    w.u32(0);
    w.append(payload);

    // CRC covers header fields and payload, omitting its own four-byte field.
    const checked = new Uint8Array(12 + payload.length);
    checked.set(w.bytes.subarray(0, 12));
    checked.set(payload, 12);
    w.view.setUint32(12, crc32(checked), true);
    return w.bytes;
}

export function encodeCodeplug(document) {
    const plug = JSON.parse(canonical(validate(document)));
    const records = [];
    const m = new Writer(83 + 4 * (plug.channels.length + plug.banks.length));
    m.u32(plug.allocation.channel_id_high_water);
    m.u32(plug.allocation.bank_id_high_water);
    m.u16(plug.channels.length);
    m.u8(plug.banks.length);
    const g = plug.global;
    const ui = g.ui;
    const light = ui.backlight;
    m.string(g.local_callsign, 10);
    m.u8(g.gain);
    m.u16(g.transmit_limit_s);
    m.u8(themes.indexOf(ui.theme));
    m.u8(contrasts.indexOf(ui.contrast));
    m.u8(ui.animations);
    m.u8(light.brightness_percent);
    m.u8(light.idle_s);
    m.u8(light.dim_percent);
    m.u8(plug.selection.operating === 'memory');
    m.u32(plug.selection.bank_id ?? 0);
    m.u32(plug.selection.channel_id ?? 0);
    writeOperating(m, plug.vfo);
    for (const item of [...plug.channels, ...plug.banks]) {
        m.u32(item.id);
    }
    m.u32(g.vfo_step_hz);
    records.push(record(1, m.bytes));
    for (const channel of plug.channels) {
        const w = new Writer(71);
        w.u32(channel.id);
        w.u16(channel.number);
        w.string(channel.name, 25);
        writeOperating(w, channel.configuration);
        records.push(record(2, w.bytes));
    }
    for (const bank of plug.banks) {
        const w = new Writer(31 + 4 * bank.channel_ids.length);
        w.u32(bank.id);
        w.string(bank.name, 25);
        w.u16(bank.channel_ids.length);
        for (const id of bank.channel_ids) {
            w.u32(id);
        }
        records.push(record(3, w.bytes));
    }
    const output = new Writer(records.reduce((size, item) => size + item.length, 0));
    for (const bytes of records) {
        output.append(bytes);
    }
    if (output.bytes.length > C.CPS_MAX_BYTES) {
        reject('capacity exceeded');
    }
    return output.bytes;
}

export function decodeCodeplug(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length > C.CPS_MAX_BYTES) {
        reject('capacity exceeded');
    }
    const stream = new Reader(bytes);
    function next(kind) {
        const header = stream.get(16);
        const h = new Reader(header);
        if (!header.subarray(0, 4).every((byte, index) => byte === [72, 84, 68, 66][index])) {
            reject('invalid magic');
        }
        h.at = 4;
        if (h.u8() !== 1 || h.u8() !== kind) {
            reject('unknown record version/kind');
        }
        const size = h.u16();
        if (size < 16 || size > 1187 || h.u32() !== 1) {
            reject('invalid record length/generation');
        }
        const checksum = h.u32();
        const payload = stream.get(size - 16);
        const checked = new Uint8Array(12 + payload.length);
        checked.set(header.subarray(0, 12));
        checked.set(payload, 12);
        if (crc32(checked) !== checksum) {
            reject('record CRC mismatch');
        }
        return new Reader(payload);
    }
    const m = next(1);
    const allocation = { channel_id_high_water: m.u32(), bank_id_high_water: m.u32() };
    const channelCount = m.u16();
    const bankCount = m.u8();
    if (channelCount > 256 || bankCount > 16) {
        reject('invalid record count');
    }
    const local_callsign = m.string(10);
    const gain = m.u8();
    const transmit_limit_s = m.u16();
    const theme = m.enum(themes);
    const contrast = m.enum(contrasts);
    const animations = m.bool();
    const backlight = { brightness_percent: m.u8(), idle_s: m.u8(), dim_percent: m.u8() };
    const selection = {
        operating: m.enum(['vfo', 'memory']),
        bank_id: m.u32() || null,
        channel_id: m.u32() || null
    };
    const vfo = readOperating(m);
    const channelIds = [];
    const bankIds = [];
    const channels = [];
    const banks = [];
    for (let i = 0; i < channelCount; i++) {
        channelIds.push(m.u32());
    }
    for (let i = 0; i < bankCount; i++) {
        bankIds.push(m.u32());
    }
    const vfo_step_hz = m.u32();
    m.done();
    for (const expected of channelIds) {
        const r = next(2);
        const id = r.u32();
        const number = r.u16();
        const name = r.string(25);
        const configuration = readOperating(r);
        r.done();
        if (id !== expected) {
            reject('channel differs from manifest');
        }
        channels.push({ id, number, name, configuration });
    }
    for (const expected of bankIds) {
        const r = next(3);
        const id = r.u32();
        const name = r.string(25);
        const count = r.u16();
        const channel_ids = [];
        if (count > 256) {
            reject('bank capacity exceeded');
        }
        for (let i = 0; i < count; i++) {
            channel_ids.push(r.u32());
        }
        r.done();
        if (id !== expected) {
            reject('bank differs from manifest');
        }
        banks.push({ id, name, channel_ids });
    }
    stream.done();
    return validate({
        format: FORMAT,
        schema_version: SCHEMA_VERSION,
        allocation,
        global: {
            local_callsign,
            gain,
            transmit_limit_s,
            vfo_step_hz,
            ui: { theme, contrast, animations, backlight }
        },
        selection,
        vfo,
        channels,
        banks
    });
}
