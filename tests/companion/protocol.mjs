// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
    C,
    crc32,
    encode,
    decode,
    Decoder,
    helloPayload,
    helloReply
} from '../../companion/src/protocol.mjs';
const fixtures = JSON.parse(
    readFileSync(new URL('../../protocol/companion/fixtures.json', import.meta.url))
);
assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
for (const fixture of fixtures) {
    const frame = {
        ...fixture.frame,
        session: BigInt(fixture.frame.session),
        payload: Uint8Array.from(Buffer.from(fixture.frame.payload, 'hex'))
    };
    const expected = Uint8Array.from(Buffer.from(fixture.wire, 'hex'));
    assert.deepEqual(encode(frame), expected, fixture.name);
    assert.deepEqual(decode(expected.slice(1, -1)), frame, fixture.name);
    const decoder = new Decoder();
    for (let i = 0; i < expected.length; i++) {
        assert.deepEqual(decoder.feed(expected[i], i), i === expected.length - 1 ? frame : null);
    }
    const bad = expected.slice();
    bad[bad.length - 2] ^= 1;
    assert.equal(decode(bad.slice(1, -1)), null);
    for (const byte of bad) {
        assert.equal(decoder.feed(byte, 300), null);
    }
    for (let i = 0; i < C.MAX_ENCODED + 20; i++) {
        assert.equal(decoder.feed(1, 400), null);
    }
    assert.deepEqual([...expected].map(b => decoder.feed(b, 410)).filter(Boolean), [frame]);
    decoder.feed(1, 0);
    decoder.feed(2, C.FRAME_TIMEOUT_MS);
    assert.deepEqual([...expected].map(b => decoder.feed(b, 600)).filter(Boolean), [frame]);
    if (fixture.name === 'hello-reply') {
        assert.deepEqual(helloReply(frame), {
            status: C.STATUS_OK,
            release: 'dev-test',
            target: 'c62',
            capabilities: 0,
            maxPayload: 192,
            leaseMs: 1000
        });
    }
    if (fixture.name === 'hello') {
        assert.deepEqual(frame.payload, helloPayload('dev-test', frame.session || 42n));
    }
}
for (const nonce of [0n, -1n, 1n << 64n, 42]) {
    assert.throws(() => helloPayload('dev-test', nonce));
}
assert.throws(() => helloPayload('invalid space', 1n));
assert.throws(() => helloReply({ type: C.MSG_HELLO, payload: new Uint8Array(11) }));
assert.equal(decode(Uint8Array.of(255)), null);
console.log('JavaScript companion conformance passed');
