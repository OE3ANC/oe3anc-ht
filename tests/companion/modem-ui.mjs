// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { ModemUi } from '../../companion/src/modem-ui.mjs';
const controls = new Map();
const root = {
    querySelector(selector) {
        if (!controls.has(selector)) {
            controls.set(selector, { addEventListener() {}, disabled: false, textContent: '' });
        }
        return controls.get(selector);
    }
};
let rejectPicker;
let requests = 0;
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {
    serial: { requestPort() {
        requests++;
        return new Promise((resolve, reject) => { rejectPicker = reject; });
    } }
} });
const companion = { session: 0n, port: null };
let flasherOwnsPort = false;
const ownership = [];
const panel = new ModemUi(root, companion, {
    available: () => !companion.port && !flasherOwnsPort,
    changed: () => ownership.push(panel.ownsPort)
});
assert.equal(controls.get('#modem-position').disabled, true);
assert.equal(controls.get('#modem-enter').disabled, true);
flasherOwnsPort = true;
await panel.run('connect');
assert.equal(requests, 0);
flasherOwnsPort = false;
companion.port = {};
await panel.run('connect');
assert.equal(requests, 0);
companion.port = null;
const picking = panel.run('connect');
assert.equal(panel.ownsPort, true, 'Reserve ownership before awaiting browser chooser');
await panel.run('connect');
assert.equal(requests, 1, 'No concurrent chooser/open');
rejectPicker(new Error('Picker cancelled'));
await picking;
assert.equal(panel.ownsPort, false);
assert.deepEqual(ownership, [true, false]);
assert.match(panel.status.textContent, /Picker cancelled/);
companion.session = 1n;
companion.bulk = true;
companion.enterKiss = () => { throw new Error('Must not switch during CPS'); };
await panel.run('enter');
assert.equal(panel.ownsPort, false);
console.log('Modem UI exclusive reservation, chooser cancellation and CPS exclusion passed');
