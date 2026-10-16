// SPDX-License-Identifier: GPL-3.0-or-later
export const MAX_FRAME = 330;

export function kissFrame(payload, command = 0) {
    if (!(payload instanceof Uint8Array) || payload.length > MAX_FRAME ||
        !Number.isInteger(command) || command < 0 || command > 255) {
        throw new Error('Invalid KISS frame (maximum 330 bytes)');
    }
    const bytes = [0xc0];
    for (const byte of [command, ...payload]) {
        if (byte === 0xc0) {
            bytes.push(0xdb, 0xdc);
        } else if (byte === 0xdb) {
            bytes.push(0xdb, 0xdd);
        } else {
            bytes.push(byte);
        }
    }
    return Uint8Array.from([...bytes, 0xc0]);
}

export class KissDecoder {
    constructor() {
        this.reset();
    }

    reset() {
        this.bytes = [];
        this.started = false;
        this.escape = false;
        this.invalid = false;
    }

    feed(byte) {
        if (byte === 0xc0) {
            const frame = this.started && !this.invalid && !this.escape && this.bytes.length
                ? { command: this.bytes[0], payload: Uint8Array.from(this.bytes.slice(1)) }
                : null;
            this.reset();
            this.started = true;
            return frame;
        }
        if (!this.started || this.invalid) {
            return null;
        }
        if (this.escape) {
            this.escape = false;
            if (byte !== 0xdc && byte !== 0xdd) {
                this.invalid = true;
                return null;
            }
            byte = byte === 0xdc ? 0xc0 : 0xdb;
        } else if (byte === 0xdb) {
            this.escape = true;
            return null;
        }
        if (this.bytes.length >= MAX_FRAME + 1) {
            this.invalid = true;
        } else {
            this.bytes.push(byte);
        }
        return null;
    }
}

function address(value) {
    const match = /^([A-Z0-9]{1,6})(?:-(\d{1,2}))?$/.exec(value);
    if (!match || Number(match[2] || 0) > 15) {
        throw new Error('Use uppercase callsigns of 1–6 letters/digits with optional SSID 0–15');
    }
    return { call: match[1], ssid: Number(match[2] || 0) };
}

export function ax25({ source, destination, path = '', payload }) {
    const digis = path ? path.split(',').map(value => value.trim()) : [];
    if (digis.length > 8) {
        throw new Error('Path allows at most eight digipeaters');
    }
    const addresses = [destination, source, ...digis].map(address);
    const bytes = [];
    for (const [index, item] of addresses.entries()) {
        bytes.push(...[...item.call.padEnd(6)].map(char => char.charCodeAt(0) << 1));
        // UI command: destination C=1, source C=0; unused digipeater H=0.
        bytes.push(0x60 | (item.ssid << 1) | (index === 0 ? 0x80 : 0) |
            (index === addresses.length - 1 ? 1 : 0));
    }
    bytes.push(3, 0xf0, ...new TextEncoder().encode(payload));
    if (bytes.length > MAX_FRAME) {
        throw new Error('AX.25 packet exceeds 330 bytes');
    }
    return Uint8Array.from(bytes);
}

export function decodeAx25(bytes) {
    const addresses = [];
    let offset = 0;
    let last = false;
    while (!last && addresses.length < 10 && offset + 7 <= bytes.length) {
        const chars = bytes.slice(offset, offset + 6);
        const field = bytes[offset + 6];
        if (chars.some(byte => byte & 1) || (field & 0x60) !== 0x60) {
            throw new Error('Malformed AX.25 address');
        }
        const call = String.fromCharCode(...chars.map(byte => byte >> 1)).trimEnd();
        if (!/^[A-Z0-9]{1,6}$/.test(call)) {
            throw new Error('Malformed AX.25 callsign');
        }
        const ssid = (field >> 1) & 15;
        addresses.push(call + (ssid ? `-${ssid}` : '') +
            (addresses.length >= 2 && (field & 0x80) ? '*' : ''));
        last = !!(field & 1);
        offset += 7;
    }
    if (!last || addresses.length < 2 || offset >= bytes.length) {
        throw new Error('Truncated AX.25 address/control');
    }
    const control = bytes[offset++];
    // Only UI frames have the one-byte control/PID layout used by APRS.
    const pid = (control & 0xef) === 3 && offset < bytes.length ? bytes[offset++] : null;
    if ((control & 0xef) === 3 && pid === null) {
        throw new Error('Truncated AX.25 PID');
    }
    return { destination: addresses[0], source: addresses[1], path: addresses.slice(2),
        control, pid, payload: bytes.slice(offset) };
}

function printable(value, limit, label, forbidden = /[^\x20-\x7e]/) {
    if (typeof value !== 'string' || value.length > limit || forbidden.test(value)) {
        throw new Error(`${label}: maximum ${limit} printable ASCII characters`);
    }
    return value;
}

function coordinate(value, latitude) {
    const limit = latitude ? 90 : 180;
    if ((typeof value !== 'number' && typeof value !== 'string') ||
        (typeof value === 'string' && !/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/.test(value))) {
        throw new Error('Coordinates must be decimal degrees');
    }
    value = Number(value);
    if (!Number.isFinite(value) || Math.abs(value) > limit) {
        throw new Error(`Coordinate must be between -${limit} and ${limit}`);
    }
    const hundredths = Math.round(Math.abs(value) * 6000);
    const degrees = Math.floor(hundredths / 6000);
    const minutes = hundredths % 6000;
    return String(degrees).padStart(latitude ? 2 : 3, '0') +
        String(Math.floor(minutes / 100)).padStart(2, '0') + '.' +
        String(minutes % 100).padStart(2, '0') +
        (latitude ? (value < 0 ? 'S' : 'N') : (value < 0 ? 'W' : 'E'));
}

export function position({ latitude, longitude, table = '/', symbol = '>', comment = '' }) {
    if (!/^[\/\\]$/.test(table) || !/^[\x21-\x7e]$/.test(symbol)) {
        throw new Error('Select / or \\ symbol table and one printable symbol character');
    }
    return '!' + coordinate(latitude, true) + table + coordinate(longitude, false) + symbol +
        printable(comment, 43, 'Position comment');
}

export function message({ addressee, text }) {
    if (!/^[A-Z0-9][A-Z0-9 -]{0,8}$/.test(addressee) || addressee.endsWith(' ')) {
        throw new Error('Message addressee: 1–9 uppercase letters, digits, spaces or hyphens');
    }
    if (!text) {
        throw new Error('Message text is required');
    }
    return ':' + addressee.padEnd(9) + ':' +
        printable(text, 67, 'Message text', /[^\x20-\x7e]|[|~{]/);
}

export function hex(bytes) {
    return [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join(' ');
}

export function safeText(bytes) {
    return [...bytes].map(byte => byte >= 32 && byte <= 126 ? String.fromCharCode(byte) : '.').join('');
}
