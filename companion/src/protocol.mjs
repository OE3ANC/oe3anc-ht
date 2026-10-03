// SPDX-License-Identifier: GPL-3.0-or-later
import * as C from './contract.mjs';
export { C };
export function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
            crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
        }
    }
    return ~crc >>> 0;
}

function integer(value, maximum, name) {
    if (!Number.isInteger(value) || value < 0 || value > maximum) {
        throw new Error(`Invalid ${name}`);
    }
}
export function encode(frame) {
    const payload = frame.payload ?? new Uint8Array();
    if (!(payload instanceof Uint8Array) || payload.length > C.MAX_PAYLOAD) {
        throw new Error('Invalid payload');
    }
    const major = frame.major ?? C.MAJOR;
    const minor = frame.minor ?? C.MINOR;
    integer(major, 255, 'major');
    integer(minor, 255, 'minor');
    integer(frame.type, 255, 'message');
    integer(frame.flags, 1, 'flags');
    integer(frame.request, 0xffffffff, 'request');
    if (
        typeof frame.session !== 'bigint' ||
        frame.session < 0n ||
        frame.session > 0xffffffffffffffffn
    ) {
        throw new Error('Invalid session');
    }
    const raw = new Uint8Array(C.HEADER_SIZE + payload.length + 4);
    const view = new DataView(raw.buffer);
    raw.set([72, 84, major, minor, frame.type, frame.flags]);
    view.setUint16(6, payload.length, true);
    view.setUint32(8, frame.request, true);
    view.setBigUint64(12, frame.session, true);
    raw.set(payload, C.HEADER_SIZE);
    view.setUint32(raw.length - 4, crc32(raw.subarray(0, -4)), true);
    const output = [0, 0];
    let codePosition = 1;
    let code = 1;
    for (const byte of raw) {
        if (byte === 0) {
            output[codePosition] = code;
            codePosition = output.length;
            output.push(0);
            code = 1;
        } else {
            output.push(byte);
            code++;
        }
    }
    output[codePosition] = code;
    output.push(0);
    return Uint8Array.from(output);
}
export function decode(encoded) {
    if (
        !(encoded instanceof Uint8Array) ||
        !encoded.length ||
        encoded.length > C.HEADER_SIZE + C.MAX_PAYLOAD + 5
    ) {
        return null;
    }
    const raw = [];
    for (let i = 0; i < encoded.length; ) {
        const code = encoded[i++];
        if (!code || i + code - 1 > encoded.length) {
            return null;
        }
        for (let n = 1; n < code; n++) {
            if (!encoded[i] || raw.length >= C.HEADER_SIZE + C.MAX_PAYLOAD + 4) {
                return null;
            }
            raw.push(encoded[i++]);
        }
        if (code < 255 && i < encoded.length) {
            if (raw.length >= C.HEADER_SIZE + C.MAX_PAYLOAD + 4) {
                return null;
            }
            raw.push(0);
        }
    }
    if (raw.length < C.HEADER_SIZE + 4 || raw[0] !== 72 || raw[1] !== 84) {
        return null;
    }
    const bytes = Uint8Array.from(raw);
    const view = new DataView(bytes.buffer);
    const length = view.getUint16(6, true);
    if (
        length > C.MAX_PAYLOAD ||
        raw.length !== C.HEADER_SIZE + length + 4 ||
        raw[5] > 1 ||
        view.getUint32(bytes.length - 4, true) !== crc32(bytes.subarray(0, -4))
    ) {
        return null;
    }
    return {
        major: raw[2],
        minor: raw[3],
        type: raw[4],
        flags: raw[5],
        request: view.getUint32(8, true),
        session: view.getBigUint64(12, true),
        payload: bytes.slice(C.HEADER_SIZE, -4)
    };
}
export class Decoder {
    constructor() {
        this.reset();
    }

    reset() {
        this.bytes = [];
        this.started = 0;
        this.discard = false;
    }

    feed(byte, now = performance.now()) {
        integer(byte, 255, 'byte');
        if (this.bytes.length && now - this.started >= C.FRAME_TIMEOUT_MS) {
            this.bytes = [];
            this.discard = true;
        }
        if (byte === 0) {
            const result =
                !this.discard && this.bytes.length ? decode(Uint8Array.from(this.bytes)) : null;
            this.reset();
            return result;
        }
        if (this.discard) {
            return null;
        }
        if (!this.bytes.length) {
            this.started = now;
        }
        if (this.bytes.length >= C.MAX_ENCODED) {
            this.bytes = [];
            this.discard = true;
            return null;
        }
        this.bytes.push(byte);
        return null;
    }
}
export function helloPayload(release, nonce) {
    if (
        !/^[\x21-\x7e]+$/.test(release) ||
        release.length > C.MAX_RELEASE ||
        typeof nonce !== 'bigint' ||
        nonce <= 0n ||
        nonce > 0xffffffffffffffffn
    ) {
        throw new Error('Invalid hello');
    }
    const bytes = new Uint8Array(1 + release.length + 8);
    bytes[0] = release.length;
    bytes.set(new TextEncoder().encode(release), 1);
    new DataView(bytes.buffer).setBigUint64(1 + release.length, nonce, true);
    return bytes;
}
export function helloReply(frame) {
    const p = frame.payload;
    if (frame.type !== C.MSG_HELLO || p.length < 11) {
        throw new Error('Invalid hello reply');
    }
    const releaseSize = p[1];
    const targetOffset = 2 + releaseSize;
    if (!releaseSize || releaseSize > C.MAX_RELEASE || targetOffset >= p.length) {
        throw new Error('Invalid release identity');
    }
    const targetSize = p[targetOffset];
    const tail = targetOffset + 1 + targetSize;
    if (!targetSize || targetSize > 8 || tail + 8 !== p.length) {
        throw new Error('Invalid hello limits');
    }
    const releaseBytes = p.slice(2, targetOffset);
    const targetBytes = p.slice(targetOffset + 1, tail);
    if ([...releaseBytes, ...targetBytes].some(b => b < 33 || b > 126)) {
        throw new Error('Invalid hello text');
    }
    const v = new DataView(p.buffer, p.byteOffset);
    return {
        status: p[0],
        release: new TextDecoder().decode(releaseBytes),
        target: new TextDecoder().decode(targetBytes),
        capabilities: v.getUint32(tail, true),
        maxPayload: v.getUint16(tail + 4, true),
        leaseMs: v.getUint16(tail + 6, true)
    };
}
