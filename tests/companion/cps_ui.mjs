// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal DOM doubles exercise the real editor's event handlers without a browser dependency.
import assert from 'node:assert/strict';
import { CpsEditor } from '../../companion/src/cps.mjs';
import { canonical, validate } from '../../companion/src/codeplug.mjs';

class Node {
    constructor(tag) {
        this.classList = { toggle() {} };
        this.tag = tag;
        this.children = [];
        this.listeners = {};
        this.attributes = {};
        this.value = '';
    }

    setAttribute(key, value) {
        this.attributes[key] = value;
        if (key === 'value') {
            this.value = value;
        }
    }

    append(...nodes) {
        this.children.push(...nodes);
    }

    replaceChildren(...nodes) {
        this.children = [...nodes];
    }

    addEventListener(type, fn) {
        (this.listeners[type] ??= []).push(fn);
    }

    change(value) {
        this.value = value;
        for (const fn of this.listeners.change ?? []) {
            fn({ target: this });
        }
    }
}
globalThis.document = { createElement: tag => new Node(tag) };
const ids = Object.fromEntries(
    [
        'cps-content',
        'cps-status',
        'cps-confirm',
        'cps-summary',
        'cps-json',
        'cps-new',
        'cps-import',
        'cps-export'
    ].map(id => [id, new Node('div')])
);
const root = { querySelector: selector => ids[selector.slice(1)], querySelectorAll: () => [] };
const editor = new CpsEditor(root);
editor.switchPanel('vfo');

function field(label) {
    function find(node) {
        if (node.tag === 'label' && node.children[0]?.textContent === label) {
            return node.children[1];
        }
        for (const child of node.children) {
            const found = find(child);
            if (found) {
                return found;
            }
        }
    }
    const found = find(ids['cps-content']);
    assert.ok(found, label);
    return found;
}
const baseline = canonical(editor.document);
const power = field('Requested power (W)');
assert.equal(power.value, '1');
for (const [watts, milliwatts] of [['0.001', 1], ['1.234', 1234], ['2.5', 2500], ['5', 5000]]) {
    power.value = watts;
    assert.equal(editor.readEditor().document.vfo.power_mw, milliwatts);
}
power.value = '1.0001';
assert.throws(() => editor.readEditor(), /Power/);
power.value = '1';
field('Mode').change('m17');
field('M17 destination').change('station');
field('Destination callsign').value = 'OE1TEST';
field('CAN (0–15)').value = '17';
field('Check RX CAN').checked = true;
field('Mode').change('fm');
field('Mode').change('m17');
assert.equal(field('Destination callsign').value, 'OE1TEST');
assert.equal(field('CAN (0–15)').value, '17', 'invalid numeric value retained');
assert.equal(field('Check RX CAN').checked, true);
assert.throws(() => validate(editor.readEditor().document), /can/);
assert.equal(canonical(editor.document), baseline, 'draft changes do not mutate codeplug');
field('CAN (0–15)').value = '7';
assert.equal(editor.readEditor().document.vfo.m17.can, 7);
field('Mode').change('fm');
field('Tone').change('ctcss');
field('CTCSS frequency (Hz)').value = 'not a frequency';
field('Tone').change('dcs');
field('DCS code (octal)').value = '731';
field('Polarity').value = 'inverted';
field('Tone').change('ctcss');
assert.equal(field('CTCSS frequency (Hz)').value, 'not a frequency', 'invalid raw text retained');
assert.throws(() => editor.readEditor(), /decimal places/);
field('CTCSS frequency (Hz)').value = '88.5';
field('Mode').change('m17');
field('Mode').change('fm');
assert.equal(field('CTCSS frequency (Hz)').value, '88.5');
assert.equal(editor.readEditor().document.vfo.fm.rx_tone.tenths_hz, 885);
field('Tone').change('dcs');
assert.equal(field('DCS code (octal)').value, '731');
assert.equal(field('Polarity').value, 'inverted');
assert.deepEqual(editor.readEditor().document.vfo.fm.rx_tone, {
    kind: 'dcs',
    code: '731',
    polarity: 'inverted'
});
assert.equal(canonical(editor.document), baseline);
console.log('Actual CPS handlers preserve mode/tone controls, including invalid raw drafts');
