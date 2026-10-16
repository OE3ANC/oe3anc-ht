// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { ModemUi } from '../../companion/src/modem-ui.mjs';
const controls = new Map();
const root = {
    querySelector(selector) {
        if (!controls.has(selector)) {
            const element = { disabled: false, textContent: '', value: '', children: [],
                addEventListener(event, callback) { this[event] = callback; },
                prepend(child) {
                    this.children.unshift(child);
                    child.remove = () => this.children.splice(this.children.indexOf(child), 1);
                },
                get lastChild() { return this.children.at(-1); }
            };
            controls.set(selector, element);
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

Object.defineProperty(globalThis, 'document', { configurable: true,
    value: { createElement: () => ({ textContent: '' }) } });
for (const [name, value] of Object.entries({ source: 'N0CALL-1', destination: 'APRS',
    path: '', latitude: '49.05833333', longitude: '-72.02916667', table: '/', symbol: '-',
    comment: 'Test', addressee: 'N0CALL', text: 'Hello' })) {
    root.querySelector(`#modem-${name}`).value = value;
}
const writes = [];
panel.kiss.send = async bytes => writes.push(bytes);
await panel.run('preview-position');
await panel.run('preview-message');
assert.equal(writes.length, 0, 'Offline previews must never write serial');
assert.match(root.querySelector('#modem-position-preview').textContent, /49.05833/);
root.querySelector('#modem-text').value = 'Changed after preview';
root.querySelector('#modem-text').input();
assert.match(root.querySelector('#modem-message-preview').textContent, /Draft changed/);
panel.kiss.ready = true;
panel.kiss.port = {};
await panel.run('message');
assert.equal(writes.length, 1);
assert.match(new TextDecoder().decode(writes[0]), /Changed after preview$/);
assert.equal(root.querySelector('#modem-sent-count').textContent, '1');
const frame = { command: 0, payload: writes[0] };
for (let i = 0; i < 103; ++i) {
    panel.packet('Received', frame);
}
assert.equal(root.querySelector('#modem-received-count').textContent, '103');
assert.equal(root.querySelector('#modem-received-log').children.length, 100);
assert.equal(root.querySelector('#modem-sent-log').children.length, 1);
panel.kiss.close = async () => { panel.kiss.port = null; panel.kiss.ready = false; };
await panel.run('close');
assert.equal(root.querySelector('#modem-text').value, 'Changed after preview');
navigator.serial.requestPort = async () => ({});
panel.kiss.connect = async port => { panel.kiss.port = port; panel.kiss.ready = true; };
await panel.run('connect');
assert.equal(root.querySelector('#modem-text').value, 'Changed after preview');
assert.equal(root.querySelector('#modem-latitude').value, '49.05833333');
root.querySelector('#modem-latitude').value = 'invalid';
await panel.run('preview-position');
assert.match(panel.status.textContent, /decimal degrees/);
assert.equal(writes.length, 1);
root.querySelector('#modem-clear').click();
assert.equal(root.querySelector('#modem-received-count').textContent, '0');
assert.equal(root.querySelector('#modem-sent-count').textContent, '0');
console.log('Offline preview, fresh send, counts, bounded history and reconnect drafts passed');
