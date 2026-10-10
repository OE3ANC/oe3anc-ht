// SPDX-License-Identifier: GPL-3.0-or-later
import { parseJson } from './json.mjs';
// Browser codeplug validation, cross-checked against tools/codeplug.py.
export const THEMES = [
    'midnight',
    'nord',
    'solarized-dark',
    'darcula',
    'terminal-green',
    'terminal-amber',
    'terminal-ice'
];
export const FORMAT = 'oe3anc-ht-codeplug';
export const SCHEMA_VERSION = 1;
export const MAX_BYTES = 512 * 1024;
export const MAX_ID = 0xffffffff;
export const MAX_CHANNELS = 256;
export const MAX_BANKS = 16;
export const VFO_STEPS_HZ = [1000, 2500, 5000, 6250, 10000, 12500, 20000, 25000, 50000, 100000];
export function fail(path, reason) {
    throw new Error(`${path}: ${reason}`);
}

function object(value, path) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
        fail(path, 'expected an object');
    }
}

function fields(value, names, path) {
    object(value, path);
    const expected = names.split(' ');
    const found = Object.keys(value);
    const missing = expected.filter(key => !Object.hasOwn(value, key));
    const extra = found.filter(key => !expected.includes(key));
    if (missing.length || extra.length) {
        fail(path, `missing fields [${missing}]; unknown fields [${extra}]`);
    }
}
export function integer(value, low, high, path) {
    if (!Number.isSafeInteger(value) || value < low || value > high) {
        fail(path, `expected integer ${low}..${high}`);
    }
}

function choice(value, options, path) {
    if (!options.includes(value)) {
        fail(path, `expected one of ${options.join(', ')}`);
    }
}

function boolean(value, path) {
    if (typeof value !== 'boolean') {
        fail(path, 'expected a boolean');
    }
}

function name(value, path) {
    if (
        typeof value !== 'string' ||
        value.length < 1 ||
        value.length > 24 ||
        /[^\x20-\x7e]/.test(value)
    ) {
        fail(path, 'expected 1..24 printable ASCII characters');
    }
}

function callsign(value, path, allowEmpty = false) {
    if (allowEmpty && value === '') {
        return;
    }
    if (
        typeof value !== 'string' ||
        value.length < 1 ||
        value.length > 9 ||
        /[^A-Z0-9./-]/.test(value) ||
        ['ALL', 'INVALID'].includes(value)
    ) {
        fail(path, 'expected a local/station callsign (1..9 uppercase radio characters)');
    }
}

function tone(value, path) {
    object(value, path);
    choice(value.kind, ['none', 'ctcss', 'dcs'], path + '.kind');
    fields(
        value,
        { none: 'kind', ctcss: 'kind tenths_hz', dcs: 'kind code polarity' }[value.kind],
        path
    );
    if (value.kind === 'ctcss') {
        integer(value.tenths_hz, 670, 2541, path + '.tenths_hz');
    }
    if (value.kind === 'dcs') {
        if (
            typeof value.code !== 'string' ||
            value.code.length !== 3 ||
            /[^0-7]/.test(value.code)
        ) {
            fail(path + '.code', 'expected exactly three octal digits, e.g. "023"');
        }
        choice(value.polarity, ['normal', 'inverted'], path + '.polarity');
    }
}
export function validateConfiguration(value, path) {
    object(value, path);
    choice(value.mode, ['fm', 'm17'], path + '.mode');
    fields(
        value,
        'mode rx_frequency_hz tx_frequency_hz tx_inhibit power_mw bandwidth squelch ' + value.mode,
        path
    );
    for (const key of ['rx_frequency_hz', 'tx_frequency_hz']) {
        const hz = value[key];
        integer(hz, 1, MAX_ID, path + '.' + key);
        if (!((hz >= 136000000 && hz <= 174000000) || (hz >= 400000000 && hz <= 480000000))) {
            fail(path + '.' + key, 'frequency outside C62/emulator supported bands');
        }
    }
    boolean(value.tx_inhibit, path + '.tx_inhibit');
    integer(value.power_mw, 1, 5000, path + '.power_mw');
    choice(value.bandwidth, ['narrow', 'wide'], path + '.bandwidth');
    integer(value.squelch, 0, 15, path + '.squelch');
    const specific = value[value.mode];
    const subpath = path + '.' + value.mode;
    if (value.mode === 'fm') {
        fields(specific, 'rx_tone tx_tone', subpath);
        tone(specific.rx_tone, subpath + '.rx_tone');
        tone(specific.tx_tone, subpath + '.tx_tone');
    } else {
        fields(specific, 'destination can rx_can_check', subpath);
        integer(specific.can, 0, 15, subpath + '.can');
        boolean(specific.rx_can_check, subpath + '.rx_can_check');
        const destination = specific.destination;
        const destpath = subpath + '.destination';
        object(destination, destpath);
        choice(destination.kind, ['broadcast', 'station'], destpath + '.kind');
        fields(destination, destination.kind === 'station' ? 'kind callsign' : 'kind', destpath);
        if (destination.kind === 'station') {
            callsign(destination.callsign, destpath + '.callsign');
        }
    }
}

function records(value, limit, path) {
    if (!Array.isArray(value) || value.length > limit) {
        fail(path, `expected an array with at most ${limit} items`);
    }
}
export function validate(document) {
    fields(document, 'format schema_version allocation global vfo selection channels banks', '$');
    choice(document.format, [FORMAT], '$.format');
    choice(document.schema_version, [SCHEMA_VERSION], '$.schema_version');
    const allocation = document.allocation;
    fields(allocation, 'channel_id_high_water bank_id_high_water', '$.allocation');
    for (const [key, value] of Object.entries(allocation)) {
        integer(value, 0, MAX_ID, '$.allocation.' + key);
    }
    const g = document.global;
    fields(g, 'local_callsign gain transmit_limit_s ui vfo_step_hz', '$.global');
    choice(g.vfo_step_hz, VFO_STEPS_HZ, '$.global.vfo_step_hz');
    callsign(g.local_callsign, '$.global.local_callsign', true);
    integer(g.gain, 0, 15, '$.global.gain');
    choice(g.transmit_limit_s, [0, 60, 120, 180], '$.global.transmit_limit_s');
    const ui = g.ui;
    fields(ui, 'theme contrast animations backlight', '$.global.ui');
    choice(ui.theme, THEMES, '$.global.ui.theme');
    choice(ui.contrast, ['normal', 'high', 'maximum'], '$.global.ui.contrast');
    boolean(ui.animations, '$.global.ui.animations');
    const light = ui.backlight;
    fields(light, 'brightness_percent idle_s dim_percent', '$.global.ui.backlight');
    choice(light.brightness_percent, [25, 50, 75, 100], '$.global.ui.backlight.brightness_percent');
    choice(light.idle_s, [0, 15, 30, 60], '$.global.ui.backlight.idle_s');
    choice(light.dim_percent, [10, 20, 30], '$.global.ui.backlight.dim_percent');
    validateConfiguration(document.vfo, '$.vfo');
    records(document.channels, MAX_CHANNELS, '$.channels');
    const channels = new Map();
    const numbers = new Set();
    for (const [index, channel] of document.channels.entries()) {
        const path = `$.channels[${index}]`;
        fields(channel, 'id number name configuration', path);
        integer(channel.id, 1, allocation.channel_id_high_water, path + '.id');
        integer(channel.number, 1, MAX_CHANNELS, path + '.number');
        if (channels.has(channel.id) || numbers.has(channel.number)) {
            fail(path, 'duplicate channel ID or number');
        }
        name(channel.name, path + '.name');
        validateConfiguration(channel.configuration, path + '.configuration');
        channels.set(channel.id, channel);
        numbers.add(channel.number);
    }
    records(document.banks, MAX_BANKS, '$.banks');
    const banks = new Map();
    for (const [index, bank] of document.banks.entries()) {
        const path = `$.banks[${index}]`;
        fields(bank, 'id name channel_ids', path);
        integer(bank.id, 1, allocation.bank_id_high_water, path + '.id');
        if (banks.has(bank.id)) {
            fail(path + '.id', 'duplicate bank ID');
        }
        name(bank.name, path + '.name');
        records(bank.channel_ids, MAX_CHANNELS, path + '.channel_ids');
        const seen = new Set();
        for (const [index, id] of bank.channel_ids.entries()) {
            const subpath = `${path}.channel_ids[${index}]`;
            integer(id, 1, MAX_ID, subpath);
            if (!channels.has(id) || seen.has(id)) {
                fail(subpath, 'missing or duplicate channel reference');
            }
            seen.add(id);
        }
        banks.set(bank.id, bank);
    }
    const s = document.selection;
    fields(s, 'operating bank_id channel_id', '$.selection');
    choice(s.operating, ['vfo', 'memory'], '$.selection.operating');
    for (const [key, items] of [
        ['bank_id', banks],
        ['channel_id', channels]
    ]) {
        if (s[key] !== null) {
            integer(s[key], 1, MAX_ID, '$.selection.' + key);
            if (!items.has(s[key])) {
                fail('$.selection.' + key, 'missing object reference');
            }
        }
    }
    if (s.operating === 'memory' && s.channel_id === null) {
        fail('$.selection.channel_id', 'memory operation requires a selected channel');
    }
    if (
        s.bank_id !== null &&
        s.channel_id !== null &&
        !banks.get(s.bank_id).channel_ids.includes(s.channel_id)
    ) {
        fail('$.selection.channel_id', 'selected channel is outside the selected bank');
    }
    return document;
}
export function parse(text) {
    return validate(parseJson(text, MAX_BYTES));
}
export function parseBytes(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length > MAX_BYTES) {
        fail('JSON', 'expected at most 512 KiB of UTF-8 bytes');
    }
    // Preserve a BOM for the strict JSON parser to reject, matching the CLI.
    return parse(new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes));
}
export function canonical(document) {
    const ordered = { ...validate(document) };
    ordered.channels = [...ordered.channels].sort((a, b) => a.number - b.number);
    ordered.banks = [...ordered.banks].sort((a, b) => a.id - b.id);
    const sort = value =>
        Array.isArray(value)
            ? value.map(sort)
            : value !== null && typeof value === 'object'
              ? Object.fromEntries(
                    Object.keys(value)
                        .sort()
                        .map(key => [key, sort(value[key])])
                )
              : value;
    return JSON.stringify(sort(ordered)) + '\n';
}
export function fresh() {
    return {
        format: FORMAT,
        schema_version: SCHEMA_VERSION,
        allocation: { channel_id_high_water: 0, bank_id_high_water: 0 },
        global: {
            local_callsign: '',
            gain: 0,
            transmit_limit_s: 180,
            vfo_step_hz: 12500,
            ui: {
                theme: 'midnight',
                contrast: 'normal',
                animations: true,
                backlight: { brightness_percent: 100, idle_s: 15, dim_percent: 10 }
            }
        },
        vfo: {
            mode: 'fm',
            rx_frequency_hz: 433500000,
            tx_frequency_hz: 433500000,
            tx_inhibit: false,
            power_mw: 1000,
            bandwidth: 'wide',
            squelch: 4,
            fm: { rx_tone: { kind: 'none' }, tx_tone: { kind: 'none' } }
        },
        selection: { operating: 'vfo', bank_id: null, channel_id: null },
        channels: [],
        banks: []
    };
}
// Each edit works on a copy and publishes only after full validation. No ID is
// consumed on failure; deletes retain counters and preserve membership order.
export function putRecord(document, kind, draft) {
    const result = structuredClone(validate(document));
    const list = result[kind];
    const highWater = kind === 'channels' ? 'channel_id_high_water' : 'bank_id_high_water';
    if (!['channels', 'banks'].includes(kind)) {
        fail('$', 'unknown record kind');
    }
    const record = structuredClone(draft);
    const index = list.findIndex(item => item.id === record.id);
    if (record.id !== 0 && index < 0) {
        fail(kind, 'record no longer exists');
    }
    if (!record.id) {
        if (list.length === (kind === 'channels' ? MAX_CHANNELS : MAX_BANKS)) {
            fail(kind, 'capacity reached');
        }
        if (result.allocation[highWater] === MAX_ID) {
            fail(kind, 'ID allocation exhausted');
        }
        record.id = ++result.allocation[highWater];
        list.push(record);
    } else {
        list[index] = record;
    }
    if (
        kind === 'banks' &&
        result.selection.bank_id === record.id &&
        result.selection.channel_id !== null &&
        !record.channel_ids.includes(result.selection.channel_id)
    ) {
        result.selection.bank_id = null;
    }
    return { document: validate(result), id: record.id };
}
export function removeRecord(document, kind, id) {
    const result = structuredClone(validate(document));
    if (!['channels', 'banks'].includes(kind)) {
        fail('$', 'unknown record kind');
    }
    if (!result[kind].some(item => item.id === id)) {
        fail(kind, 'record no longer exists');
    }
    result[kind] = result[kind].filter(item => item.id !== id);
    if (kind === 'channels') {
        for (const bank of result.banks) {
            bank.channel_ids = bank.channel_ids.filter(member => member !== id);
        }
        if (result.selection.channel_id === id) {
            result.selection.channel_id = null;
            result.selection.operating = 'vfo';
        }
    } else if (result.selection.bank_id === id) {
        result.selection.bank_id = null;
    }
    return validate(result);
}
export function targetErrors(document) {
    validate(document);
    return document.global.gain === 0
        ? []
        : ['Current C62 and emulator radios require global gain 0.'];
}
