// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { C, crc32 } from '../../companion/src/protocol.mjs';
import { CompanionConnection } from '../../companion/src/serial.mjs';
import { UiTransfer } from '../../companion/src/ui-transfer.mjs';
const fixture = JSON.parse(
    fs.readFileSync(new URL('../../protocol/companion/ui-fixtures.json', import.meta.url))
).valid.find(v => v.name === 'maximum-list');
const bytes = Uint8Array.from(Buffer.from(fixture.wire, 'hex'));
const connection = new CompanionConnection({ identity: 'test' });
connection.session = 1n;
connection.info = { capabilities: C.CAP_UI_SNAPSHOT };
let corrupt = false;
let stale = false;
let delayed = false;
let gate;
let release;
connection.request = async (type, payload) => {
    if (gate) {
        const waiting = gate;
        gate = null;
        await waiting;
    }
    if (type === C.MSG_UI_POLL) {
        if (stale) {
            return { payload: Uint8Array.of(C.STATUS_STALE) };
        }
        if (delayed) {
            await new Promise(resolve => setTimeout(resolve, C.UI_STALE_MS + 1));
        }
        const unchanged = new DataView(payload.buffer).getUint32(0, true) === 1;
        const p = new Uint8Array(unchanged ? 6 : 16);
        const d = new DataView(p.buffer);
        p[1] = unchanged ? 0 : 1;
        d.setUint32(2, 1, true);
        if (!unchanged) {
            d.setUint32(6, 12, true);
            d.setUint16(10, bytes.length, true);
            d.setUint32(12, crc32(bytes), true);
        }
        return { payload: p };
    }
    const q = new DataView(payload.buffer);
    const offset = q.getUint16(4, true);
    const count = payload[6];
    const p = new Uint8Array(7 + count);
    const d = new DataView(p.buffer);
    d.setUint32(1, 12, true);
    d.setUint16(5, offset, true);
    p.set(bytes.subarray(offset, offset + count), 7);
    if (corrupt) {
        p[7] ^= 1;
    }
    return { payload: p };
};
const ui = new UiTransfer(connection);
assert.deepEqual((await ui.poll()).bytes, bytes);
assert.equal((await ui.poll()).changed, false);
ui.reset();
corrupt = true;
await assert.rejects(ui.poll(), /CRC/);
assert.equal(ui.revision, 0);
corrupt = false;
stale = true;
await assert.rejects(ui.poll(), /status 5/);
stale = false;
delayed = true;
await assert.rejects(ui.poll(), /stale/);
delayed = false;
// A foreground action waits for the current snapshot and reserves the next
// serial slot. No subsequent poll may overtake it or consume its baseline.
gate = new Promise(resolve => {
    release = resolve;
});
const poll = ui.poll();
let ran = false;
const foreground = connection.transfer(async () => {
    ran = true;
    assert(connection.bulk);
    await assert.rejects(ui.poll(), /busy/);
    return 42;
});
assert.equal(ran, false);
await assert.rejects(ui.poll(), /busy/);
release();
await poll;
assert.equal(await foreground, 42);
assert.equal(connection.bulk, false);
gate = new Promise(resolve => {
    release = resolve;
});
const old = ui.poll();
connection.session = 2n;
release();
await assert.rejects(old, /stale/);
assert.equal(connection.bulk, false);
console.log(
    'UI chunk coherence, CRC/freshness/session rejection and foreground transfer priority passed'
);
