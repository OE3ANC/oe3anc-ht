// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';

const source = readFileSync(new URL('../../companion/src/theme.js', import.meta.url), 'utf8');
const key = 'oe3anc-ht-companion-theme';

function page(storage, dark = false) {
    const system = new EventTarget();
    system.matches = dark;
    const select = new EventTarget();
    const document = new EventTarget();
    document.documentElement = { dataset: {} };
    document.querySelector = selector => {
        assert.equal(selector, '#companion-theme');
        return select;
    };
    runInNewContext(source, {
        document,
        window: {
            matchMedia: () => system,
            get localStorage() {
                if (!storage) {
                    throw new Error('Storage blocked');
                }
                return storage;
            }
        }
    });
    document.dispatchEvent(new Event('DOMContentLoaded'));
    return {
        get theme() {
            return document.documentElement.dataset.theme;
        },
        select,
        choose(value) {
            select.value = value;
            select.dispatchEvent(new Event('change'));
        },
        system(dark) {
            system.matches = dark;
            system.dispatchEvent(new Event('change'));
        }
    };
}

const values = new Map();
const storage = {
    getItem: key => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, value)
};
const first = page(storage);
assert.equal(first.select.value, 'system');
assert.equal(first.theme, 'light');
first.system(true);
assert.equal(first.theme, 'dark');
first.choose('light');
first.system(true);
assert.equal(first.theme, 'light');
assert.equal(values.get(key), 'light');
assert.equal(page(storage, true).theme, 'light');
first.choose('dark');
first.system(false);
assert.equal(first.theme, 'dark');
assert.equal(page(storage).theme, 'dark');
first.choose('system');
assert.equal(first.theme, 'light');
assert.equal(page(storage, true).theme, 'dark');
assert.equal(page(storage).select.value, 'system');
values.set(key, 'invalid');
assert.equal(page(storage, true).select.value, 'system');
const blocked = page(null, true);
blocked.choose('light');
assert.equal(blocked.theme, 'light');
const writeBlocked = page({
    getItem: () => 'light',
    setItem: () => {
        throw new Error('Storage full');
    }
});
writeBlocked.choose('dark');
assert.equal(writeBlocked.theme, 'dark');
console.log('Companion themes: defaults, system changes, overrides, reload and blocked storage passed');
