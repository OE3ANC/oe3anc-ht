// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { C } from '../../companion/src/protocol.mjs';
import { VirtualKeypad } from '../../companion/src/keys.mjs';
import { CompanionConnection } from '../../companion/src/serial.mjs';

// Even a radio advertising PTT must receive only ordinary keypad messages.
const connection = new CompanionConnection({ identity: 'test' });
connection.session = 42n;
connection.info = { capabilities: C.CAP_PTT | C.CAP_UI_KEYS };
const sent = [];
let unblock;
connection.request = async (type, payload = new Uint8Array()) => {
    sent.push({ type, payload: [...payload] });
    if (type === C.MSG_UI_KEY) {
        await new Promise(resolve => {
            unblock = resolve;
        });
    }
    return { payload: Uint8Array.of(C.STATUS_OK) };
};

const listeners = {};
const status = { textContent: '' };
const root = {
    querySelector(selector) {
        assert.equal(selector, '[role="status"]');
        return status;
    },
    querySelectorAll(selector) {
        assert.equal(selector, '[data-key]');
        return [];
    },
    addEventListener(name, listener) {
        listeners[name] = listener;
    }
};
globalThis.window = { addEventListener() {} };
globalThis.document = { hidden: false, hasFocus: () => true, addEventListener() {} };
const keypad = new VirtualKeypad(root, connection, () => true, () => {});
try {
    assert.equal(keypad.shortcut(' '), undefined);
    for (const name of ['keydown', 'keyup']) {
        listeners[name]({
            key: ' ',
            code: 'Space',
            preventDefault() {
                assert.fail('Space must not be captured as a PTT shortcut');
            }
        });
    }
    assert.equal(sent.length, 0);
    assert(!status.textContent.includes('PTT'));
    const html = readFileSync(new URL('../../companion/index.html', import.meta.url), 'utf8');
    assert(!html.includes('data-ptt'));

    listeners.keydown({ key: 'ArrowUp', code: 'ArrowUp', preventDefault() {} });
    let foregroundStarted = false;
    const foreground = connection.transfer(async () => {
        foregroundStarted = true;
    });
    assert.equal(foregroundStarted, false);
    unblock();
    await foreground;
    assert.deepEqual(sent.splice(0), [
        { type: C.MSG_UI_KEY, payload: [C.UI_KEY_UP, 1] },
        { type: C.MSG_UI_KEYS_CLEAR, payload: [] },
        { type: C.MSG_UI_KEYS_CLEAR, payload: [] }
    ]);

    // The app's explicit disconnect first reserves a foreground cleanup slot.
    await connection.transfer(async () => {});
    await connection.close();
    assert.deepEqual(sent, [{ type: C.MSG_UI_KEYS_CLEAR, payload: [] }]);
    assert.equal(connection.session, 0n);
} finally {
    clearInterval(keypad.watch);
    clearTimeout(keypad.keys.timer);
    delete globalThis.window;
    delete globalThis.document;
}
console.log('Web keypad, Space, CPS and disconnect send no remote PTT requests');
