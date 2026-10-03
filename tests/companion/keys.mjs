// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { LiveUi } from '../../companion/src/live-ui.mjs';
import { C } from '../../companion/src/protocol.mjs';
import { CompanionConnection } from '../../companion/src/serial.mjs';
import { RemoteKeys } from '../../companion/src/keys.mjs';
const tick = () => new Promise(resolve => setTimeout(resolve, 0));
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
const sent = [];
let ready = true;
let pending;
let delay = false;
const c = {
    session: 42n,
    info: { capabilities: C.CAP_PTT | C.CAP_UI_KEYS },
    async transfer(work, options) {
        assert.equal(options.input, true);
        return work();
    },
    async request(type, payload) {
        sent.push({ type, payload: [...payload] });
        if (delay) {
            await new Promise(resolve => {
                pending = resolve;
            });
        }
        return { payload: Uint8Array.of(C.STATUS_OK) };
    }
};
const keys = new RemoteKeys(c, () => ready);
keys.set(C.UI_KEY_DIGIT_2, true);
keys.set(C.UI_KEY_DIGIT_2, true);
keys.set(C.UI_KEY_DIGIT_2, false);
await tick();
assert.deepEqual(sent.splice(0), [
    { type: C.MSG_UI_KEY, payload: [10, 1] },
    { type: C.MSG_UI_KEY, payload: [10, 0] }
]);
ready = false;
keys.set(C.UI_KEY_UP, true);
await tick();
assert.equal(sent.length, 0);
ready = true;
// Blur/hidden/stale cancellation drops unsent presses behind a pending ACK.
delay = true;
keys.set(C.UI_KEY_STAR, true);
keys.set(C.UI_KEY_DOWN, true);
keys.cancel();
assert.equal(sent.length, 1);
delay = false;
pending();
await tick();
assert.deepEqual(sent.splice(0), [
    { type: C.MSG_UI_KEY, payload: [6, 1] },
    { type: C.MSG_UI_KEYS_CLEAR, payload: [] }
]);
assert.equal(keys.held, 0);
assert.equal(keys.acceptedHeld, 0);
// KEEP uses the radio-acknowledged mask; a later local release cannot change
// an earlier queued KEEP's meaning while its preceding press ACK is delayed.
delay = true;
keys.set(C.UI_KEY_STAR, true);
await pause(C.UI_KEYS_KEEP_MS + 20);
keys.set(C.UI_KEY_STAR, false);
delay = false;
pending();
await tick();
assert.deepEqual(sent.splice(0), [
    { type: C.MSG_UI_KEY, payload: [6, 1] },
    { type: C.MSG_UI_KEYS_KEEP, payload: [64, 0, 0, 0] },
    { type: C.MSG_UI_KEY, payload: [6, 0] }
]);
keys.set(C.UI_KEY_STAR, true);
await tick();
ready = false;
await pause(C.UI_KEYS_KEEP_MS + 20);
assert.equal(keys.held, 0);
assert.equal(sent.at(-1).type, C.MSG_UI_KEYS_CLEAR);
sent.length = 0;
// A bulk transfer cannot replay queued keys after completing; expiry is radio-owned.
ready = true;
c.bulk = true;
keys.set(C.UI_KEY_UP, true);
keys.cancel();
await tick();
assert.equal(sent.length, 0);
c.bulk = false;
await keys.run();
assert.deepEqual(sent.splice(0), [{ type: C.MSG_UI_KEYS_CLEAR, payload: [] }]);
// Fully released taps remain queued: stale gating must discard them too.
delay = true;
keys.set(C.UI_KEY_UP, true);
keys.set(C.UI_KEY_UP, false);
keys.set(C.UI_KEY_DOWN, true);
keys.set(C.UI_KEY_DOWN, false);
ready = false;
delay = false;
pending();
await tick();
assert.deepEqual(sent.splice(0), [
    { type: C.MSG_UI_KEY, payload: [0, 1] },
    { type: C.MSG_UI_KEYS_CLEAR, payload: [] }
]);
ready = true;
c.session = 43n;
keys.set(C.UI_KEY_UP, true);
keys.set(C.UI_KEY_UP, false);
await tick();
assert.equal(keys.acceptedHeld, 0);
keys.cancel();
await tick();
console.log(
    'Virtual-key ordering, no-repeat, held renewal, cancellation and bulk exclusion checks passed'
);

// CPS cancels and waits for an input batch; a delayed CLEAR must not lose its
// slot or consume the CPS write/read baseline before the foreground action.
const real = new CompanionConnection({ identity: 'test' });
real.session = 1n;
let unblock;
let cleared = false;
let ran = false;
const input = real.transfer(
    async () => {
        await new Promise(resolve => {
            unblock = resolve;
        });
        cleared = true;
    },
    { input: true }
);
real.cancelKeys = () => {};
real.clearKeys = async () => {
    assert(cleared);
};
const foreground = real.transfer(async () => {
    ran = true;
    return 42;
});
assert.equal(ran, false);
await assert.rejects(
    real.transfer(async () => {}, { background: true }),
    /busy/
);
unblock();
await input;
assert.equal(await foreground, 42);
assert.equal(real.bulk, false);

const expiredView = {
    active: true,
    freshAt: 0,
    previous: 0,
    stale() {
        this.active = false;
    },
    renderer: {
        tick() {
            assert.fail('stale frame must not animate');
        }
    }
};
LiveUi.prototype.animate.call(expiredView, C.UI_STALE_MS);
assert.equal(expiredView.active, false);
assert.equal(expiredView.animation, null);
