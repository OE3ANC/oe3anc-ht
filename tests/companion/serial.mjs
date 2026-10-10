// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { webcrypto } from 'node:crypto';
import { C, encode, Decoder } from '../../companion/src/protocol.mjs';
import { CompanionConnection } from '../../companion/src/serial.mjs';
globalThis.crypto ??= webcrypto;
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

class Port {
    constructor({
        dropFirst = false,
        mismatch = false,
        versionMismatch = false,
        legacyMinor = null,
        shortError = false,
        silent = false
    } = {}) {
        this.writes = [];
        this.closed = 0;
        this.opened = 0;
        this.readable = new ReadableStream({
            start: controller => {
                this.rx = controller;
            }
        });
        const decoder = new Decoder();
        this.writable = new WritableStream({
            write: bytes => {
                this.writes.push(bytes.slice());
                if (silent || (dropFirst && this.writes.length === 1)) {
                    return;
                }
                for (const byte of bytes) {
                    const request = decoder.feed(byte);
                    if (!request) {
                        continue;
                    }
                    let major = request.major;
                    let minor = request.minor;
                    let session = request.session;
                    let payload = Uint8Array.of(C.STATUS_OK);
                    if (request.type === C.MSG_HELLO) {
                        const size = request.payload[0];
                        session = new DataView(request.payload.buffer).getBigUint64(1 + size, true);
                        const release = new TextEncoder().encode(
                            mismatch ? 'v1.0.0@' + 'a'.repeat(40) : 'dev-test'
                        );
                        payload = new Uint8Array(1 + 1 + release.length + 1 + 3 + 8);
                        payload[0] = mismatch ? C.STATUS_MISMATCH : C.STATUS_OK;
                        payload[1] = release.length;
                        payload.set(release, 2);
                        const target = 2 + release.length;
                        payload[target] = 3;
                        payload.set(new TextEncoder().encode('c62'), target + 1);
                        const view = new DataView(payload.buffer);
                        const tail = target + 4;
                        view.setUint16(tail + 4, C.MAX_PAYLOAD, true);
                        view.setUint16(tail + 6, C.LEASE_MS, true);
                        if (mismatch) {
                            session = 0n;
                        }
                        if (versionMismatch) {
                            major = C.MAJOR + 1;
                            session = 0n;
                            payload = Uint8Array.of(C.STATUS_MISMATCH);
                        }
                        if (legacyMinor !== null) {
                            minor = legacyMinor;
                            session = 0n;
                            if (request.minor !== legacyMinor) {
                                payload = Uint8Array.of(C.STATUS_MISMATCH);
                            } else {
                                assert.equal(
                                    new TextDecoder().decode(request.payload.slice(1, 1 + size)),
                                    'companion-identify'
                                );
                                payload[0] = C.STATUS_MISMATCH;
                            }
                        }
                        if (shortError) {
                            session = 0n;
                            payload = Uint8Array.of(C.STATUS_INVALID);
                        }
                    }
                    // Noise and an uncorrelated response must never resolve a request.
                    this.rx.enqueue(Uint8Array.of(1, 1, 0));
                    this.rx.enqueue(
                        encode({
                            ...request,
                            flags: C.FLAG_RESPONSE,
                            request: request.request + 10,
                            session,
                            payload
                        })
                    );
                    this.rx.enqueue(
                        encode({ ...request, flags: C.FLAG_RESPONSE, major, minor, session, payload })
                    );
                }
            }
        });
    }

    async open(options) {
        this.opened++;
        assert.equal(options.baudRate, 115200);
    }

    async close() {
        assert.equal(this.readable.locked, false);
        assert.equal(this.writable.locked, false);
        this.closed++;
    }
}
const states = [];
const connection = new CompanionConnection({ identity: 'dev-test' }, value => states.push(value));
const port = new Port({ dropFirst: true });
assert.equal((await connection.connect(port)).target, 'c62');
assert.equal(states.at(-1).connected, true);
assert.equal(port.writes.length, 2);
assert.deepEqual(port.writes[0], port.writes[1]);
assert.equal((await connection.request(C.MSG_PING)).payload[0], C.STATUS_OK);
await Promise.all([connection.close(), connection.close()]);
assert.equal(port.closed, 1);
assert.equal(connection.session, 0n);
assert.equal(connection.port, null);
// Reconnect, unexpected unplug/error, lock release and another reconnect.
const unplug = new Port();
await connection.connect(unplug);
unplug.rx.error(new Error('Unplugged'));
for (let i = 0; i < 50 && !unplug.closed; i++) {
    await pause(5);
}
assert.equal(unplug.closed, 1);
assert.equal(states.at(-1).error.message, 'Unplugged');
const again = new Port();
await connection.connect(again);
await connection.close();
assert.equal(again.closed, 1);
const mismatch = new Port({ mismatch: true });
await assert.rejects(
    connection.connect(mismatch),
    error => error.requiredRelease === 'v1.0.0@' + 'a'.repeat(40)
);
assert.equal(mismatch.closed, 1);
assert.equal(connection.session, 0n);
const version = new Port({ versionMismatch: true });
await assert.rejects(connection.connect(version), error =>
    error.message.includes(`Radio protocol ${C.MAJOR + 1}.${C.MINOR} requires a matching companion`)
);
assert.equal(version.writes.length, 1);
assert.equal(version.closed, 1);
for (const legacyMinor of [0, 1]) {
    const legacy = new Port({ legacyMinor, mismatch: true });
    await assert.rejects(
        connection.connect(legacy),
        error => error.requiredRelease === 'v1.0.0@' + 'a'.repeat(40) &&
            error.message.includes(`protocol 1.${legacyMinor}`)
    );
    assert.equal(legacy.writes.length, 2, 'Only HELLO and bounded identity discovery are sent');
    assert.equal(connection.session, 0n);
    assert.equal(legacy.closed, 1);
}
const shortError = new Port({ shortError: true });
await assert.rejects(connection.connect(shortError), /rejected the handshake/);
assert.equal(shortError.closed, 1);
const silent = new Port({ silent: true });
await assert.rejects(connection.connect(silent), /timed out/);
assert.equal(silent.writes.length, 2);
assert.equal(silent.closed, 1);
console.log('Web Serial retry, mismatch, disconnect and reconnect checks passed');
