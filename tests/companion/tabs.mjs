// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setupToolTabs } from '../../companion/src/tabs.mjs';
import { LiveUi } from '../../companion/src/live-ui.mjs';
import { C } from '../../companion/src/protocol.mjs';

const html = readFileSync(new URL('../../companion/index.html', import.meta.url), 'utf8');
const panels = new Map();
let focused;
const tabs = [...html.matchAll(/<button[^>]+role="tab"[^>]+aria-controls="([^"]+)"[^>]*>/g)].map(
    ([, id]) => {
        assert.match(html, new RegExp(`<section id="${id}"[^>]+role="tabpanel"`));
        panels.set(`#${id}`, { hidden: false, draft: { name: 'Unsaved edit' } });
        return {
            id,
            attributes: {},
            events: {},
            getAttribute: () => id,
            setAttribute(key, value) {
                this.attributes[key] = value;
            },
            addEventListener(type, handler) {
                this.events[type] = handler;
            },
            focus() {
                focused = this;
            }
        };
    }
);
assert.equal(tabs.length, 3);
globalThis.location = { hash: '#firmware-tools' };
globalThis.history = {
    replaceState(_state, _title, hash) {
        location.hash = hash;
    }
};
let hashchange;
globalThis.window = {
    addEventListener(type, handler) {
        assert.equal(type, 'hashchange');
        hashchange = handler;
    }
};
let updates = 0;
setupToolTabs(
    { querySelectorAll: () => tabs, querySelector: selector => panels.get(selector) },
    () => updates++
);

function selected(index) {
    tabs.forEach((tab, i) => {
        assert.equal(tab.attributes['aria-selected'], String(i === index));
        assert.equal(tab.tabIndex, i === index ? 0 : -1);
        assert.equal(panels.get(`#${tab.id}`).hidden, i !== index);
    });
}
function key(index, key) {
    let prevented = false;
    tabs[index].events.keydown({ key, preventDefault: () => (prevented = true) });
    return prevented;
}
selected(2);
const draft = panels.get('#cps').draft;
tabs[0].events.click();
selected(0);
assert.equal(location.hash, '#cps');
assert.equal(focused, tabs[0]);
assert(key(0, 'ArrowLeft'));
selected(2);
assert(key(2, 'ArrowRight'));
selected(0);
assert(key(0, 'End'));
selected(2);
assert(key(2, 'Home'));
selected(0);
assert(!key(0, 'Tab'));
location.hash = '#live-ui';
hashchange();
selected(1);
location.hash = '#unknown';
hashchange();
selected(0);
assert.equal(panels.get('#cps').draft, draft, 'switching must preserve the editor DOM');
assert.equal(updates, 8);

// Hiding an active Radio panel invalidates in-flight snapshots and immediately
// updates key availability. Returning must start a fresh snapshot generation.
let keyUpdates = 0;
let polled;
const view = {
    root: { hidden: true, classList: { add() {} } },
    connection: { session: 1n, info: { capabilities: C.CAP_UI_SNAPSHOT } },
    session: 1n,
    generation: 3,
    active: true,
    pause: {},
    status: {},
    keypad: { update: () => keyUpdates++ },
    stale: LiveUi.prototype.stale,
    loading: Promise.resolve(),
    poll: generation => (polled = generation)
};
LiveUi.prototype.update.call(view);
assert.equal(view.active, false);
assert.equal(view.session, 0n);
assert.equal(view.generation, 4);
assert.equal(keyUpdates, 1);
await LiveUi.prototype.poll.call(view, 3);
assert.equal(view.polling, undefined, 'late snapshot generation must not run');
view.root.hidden = false;
LiveUi.prototype.update.call(view);
await view.loading;
assert.equal(polled, 5);
view.paused = true;
view.root.hidden = true;
LiveUi.prototype.update.call(view);
view.root.hidden = false;
LiveUi.prototype.update.call(view);
assert.equal(view.session, 0n, 'user pause must survive switching tabs');
assert.equal(polled, 5);
console.log('Tool tabs: selection, keyboard, deep links, draft retention and live-view pause passed');
