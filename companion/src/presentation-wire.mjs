// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
const S = name => C['UI_SCREEN_' + name];
export function presentationKind(v) {
    if (v.flags & 4) {
        return 'system';
    }
    if (v.flags & 8) {
        return 'home';
    }
    const s = v.screen;
    if (s === S('MENU')) {
        return 'menu';
    }
    if ([S('STATUS'), S('COMPANION_EXIT')].includes(s)) {
        return 'status';
    }
    if (s === S('APPEARANCE')) {
        return 'appearance';
    }
    if (s === S('DIAGNOSTICS')) {
        return v.flags & 64 ? 'hex' : 'list';
    }
    if (
        [
            S('FREQUENCY'),
            S('CALLSIGN'),
            S('CHANNEL_NUMBER'),
            S('CHANNEL_FIELD'),
            S('BANK_NAME')
        ].includes(s)
    ) {
        return 'text';
    }
    if (
        [S('CHANNELS'), S('BANKS'), S('CHANNEL_BANK'), S('BANK_MEMBERS'), S('BANK_ADD')].includes(s)
    ) {
        return 'list';
    }
    if (
        (s >= S('CHANNEL_EDITOR') && s <= S('CHANNEL_SAVED')) ||
        (s >= S('BANK_EDITOR') && s <= S('BANK_SAVED')) ||
        [S('VFO_STEP'), S('QUICK_CONTROLS'), S('BACKLIGHT'), S('TRANSMIT_LIMIT')].includes(s)
    ) {
        return 'form';
    }
    return 'none';
}

function fields(v, text) {
    v.actions = Array.from({ length: 4 }, (_, i) => text(v.actions?.[i], 32));
    const kind = presentationKind(v);
    if (['system', 'status'].includes(kind)) {
        v.status ??= {};
        v.status.title = text(v.status.title, 25);
        v.status.detail = text(v.status.detail, 32);
        v.status.rows = Array.from({ length: 4 }, (_, i) => text(v.status.rows?.[i], 32));
    } else if (kind === 'home') {
        v.home ??= {};
        for (const [key, limit] of [
            ['identity', 16],
            ['name', 25],
            ['context', 32],
            ['frequency', 16],
            ['settings', 32],
            ['activity', 32],
            ['battery', 12],
            ['mode', 4]
        ]) {
            v.home[key] = text(v.home[key], limit);
        }
    } else if (['menu', 'list', 'hex'].includes(kind)) {
        v.list ??= {};
        v.list.title = text(v.list.title, 25);
        v.list.detail = text(v.list.detail, 32);
        v.list.rows = Array.from({ length: 4 }, (_, i) => {
            const row = v.list.rows?.[i] ?? {};
            return {
                name: text(row.name, 25),
                prefix: text(row.prefix, 5),
                suffix: text(row.suffix, 5)
            };
        });
        if (kind === 'hex') {
            v.diagnosticText = text(v.diagnosticText, 5);
        } else {
            v.lines ??= {};
            v.lines[6] = text(v.lines[6], 32);
        }
    } else if (['appearance', 'text', 'form'].includes(kind)) {
        v.lines ??= {};
        v.lines[1] = text(v.lines[1], 32);
        v.lines[6] = text(v.lines[6], 32);
        if (kind === 'text') {
            v.lines[0] = text(v.lines[0], 32);
            v.lines[4] = text(v.lines[4], 32);
            v.text ??= {};
            v.text.value = text(v.text.value, 25);
        } else if (kind === 'form') {
            v.lines[0] = text(v.lines[0], 32);
            for (let i = 2; i < 6; i++) {
                v.lines[i] = text(v.lines[i], 32);
            }
        }
    }
}

function scalars(v, number) {
    const kind = presentationKind(v);
    if (['system', 'status'].includes(kind)) {
        v.status.color = number(v.status.color, 1);
    }
    if (kind === 'home') {
        for (const key of ['status', 'contextColor', 'batteryColor', 'bars']) {
            v.home[key] = number(v.home[key], 1);
        }
    }
    if (['menu', 'list', 'hex'].includes(kind)) {
        v.list.cursor = number(v.list.cursor, 2);
        v.list.count = number(v.list.count, 2);
        v.list.ready = number(v.list.ready, 1);
    }
    if (kind === 'text') {
        v.text.kind = number(v.text.kind, 1);
        v.text.cursor = number(v.text.cursor, 1);
    }
}

function valid(v) {
    const kind = presentationKind(v);
    if (
        v.screen > S('COMPANION_EXIT') ||
        v.listReturn > S('COMPANION_EXIT') ||
        v.theme > 3 ||
        v.contrast > 2 ||
        v.flags > 1023 ||
        (v.flags & 12) === 12 ||
        v.formCursor > 3 ||
        (v.flags & 1 && !(v.flags & 512))
    ) {
        throw new Error('Invalid presentation header');
    }
    if (['system', 'status'].includes(kind) && v.status.color > 3) {
        throw new Error('Invalid status color');
    }
    if (
        kind === 'home' &&
        (v.home.bars > 5 ||
            ['status', 'contextColor', 'batteryColor'].some(key => v.home[key] > 3) ||
            !['FM', 'M17'].includes(v.home.mode))
    ) {
        throw new Error('Invalid home presentation');
    }
    if (
        ['menu', 'list', 'hex'].includes(kind) &&
        (v.list.count > 256 ||
            (v.list.count ? v.list.cursor >= v.list.count : v.list.cursor !== 0) ||
            v.list.ready > 1)
    ) {
        throw new Error('Invalid visible list');
    }
    if (
        kind === 'text' &&
        (v.text.kind > 3 ||
            v.text.value.length > [24, 9, 11, 3][v.text.kind] ||
            v.text.cursor > v.text.value.length ||
            (v.flags & 256 && !v.text.cursor))
    ) {
        throw new Error('Invalid text cursor');
    }
    if (
        (v.flags & 48 && kind !== 'home') ||
        (v.flags & 64 && v.screen !== S('DIAGNOSTICS')) ||
        (v.flags & 128 && v.screen !== S('QUICK_CONTROLS')) ||
        (v.flags & 256 && kind !== 'text')
    ) {
        throw new Error('Invalid presentation flags');
    }
}
const header = [
    ['screen', 1],
    ['theme', 1],
    ['contrast', 1],
    ['flags', 2],
    ['formCursor', 1],
    ['listReturn', 1],
    ['interruptions', 4],
    ['pttSequence', 4],
    ['monitorSequence', 4]
];
export function decodePresentation(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 24 || bytes.length > C.UI_MAX_BYTES) {
        throw new Error('Invalid presentation size');
    }
    let offset = 0;
    const number = (_, count) => {
        if (offset + count > bytes.length) {
            throw new Error('Truncated presentation');
        }
        let value = 0;
        for (let i = 0; i < count; i++) {
            value += bytes[offset++] * 2 ** (i * 8);
        }
        return value;
    };
    const text = (_, limit) => {
        const size = number(0, 1);
        if (size >= limit || offset + size > bytes.length) {
            throw new Error('Invalid presentation text size');
        }
        const value = bytes.subarray(offset, offset + size);
        if ([...value].some(b => b < 32 || b > 126)) {
            throw new Error('Invalid presentation text');
        }
        offset += size;
        return String.fromCharCode(...value);
    };
    if (number(0, 1) !== 1) {
        throw new Error('Unsupported presentation schema');
    }
    const v = {};
    for (const [key, count] of header) {
        v[key] = number(0, count);
    }
    fields(v, text);
    scalars(v, number);
    valid(v);
    if (offset !== bytes.length) {
        throw new Error('Trailing presentation bytes');
    }
    return v;
}
export function encodePresentation(document) {
    const v = structuredClone(document);
    const bytes = [1];
    const number = (value, count) => {
        if (!Number.isInteger(value) || value < 0 || value >= 2 ** (count * 8)) {
            throw new Error('Invalid presentation integer');
        }
        for (let i = 0; i < count; i++) {
            bytes.push(Math.floor(value / 2 ** (i * 8)) & 255);
        }
        return value;
    };
    const text = (value, limit) => {
        if (typeof value !== 'string' || value.length >= limit || !/^[\x20-\x7e]*$/.test(value)) {
            throw new Error('Invalid presentation text');
        }
        number(value.length, 1);
        for (const c of value) {
            bytes.push(c.charCodeAt(0));
        }
        return value;
    };
    for (const [key, count] of header) {
        number(v[key], count);
    }
    fields(v, text);
    scalars(v, number);
    valid(v);
    const result = Uint8Array.from(bytes);
    decodePresentation(result);
    return result;
}
