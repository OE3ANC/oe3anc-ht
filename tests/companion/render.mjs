// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
const [native, wasm] = process.argv.slice(2);
if (!native || !wasm) {
    throw new Error('Usage: node render.mjs /path/to/native-renderer /path/to/ht-ui.mjs');
}
const factory = (await import(pathToFileURL(path.resolve(wasm)))).default;
const fixtures = JSON.parse(
    fs.readFileSync(new URL('../../protocol/companion/ui-fixtures.json', import.meta.url))
);
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ht-ui-parity-'));
try {
    for (const fixture of fixtures.valid) {
        const wire = Buffer.from(fixture.wire, 'hex');
        const input = path.join(directory, 'input');
        const output = path.join(directory, 'frame');
        fs.writeFileSync(input, wire);
        execFileSync(native, [input, output]);
        const module = await factory();
        assert.equal(module._ht_ui_start(), 0);
        module.HEAPU8.set(wire, module._ht_ui_buffer());
        assert.equal(module._ht_ui_apply(wire.length), 0);
        for (let i = 0; i < 10; i++) {
            module._ht_ui_tick(100);
        }
        const pixels = module.HEAPU8.subarray(
            module._ht_ui_pixels(),
            module._ht_ui_pixels() + 160 * 128 * 2
        );
        assert.deepEqual(Buffer.from(pixels), fs.readFileSync(output), fixture.name);
        const previous = Buffer.from(pixels);
        for (const bad of fixtures.invalid) {
            const bytes = Buffer.from(bad.wire, 'hex');
            module.HEAPU8.set(bytes, module._ht_ui_buffer());
            assert.notEqual(module._ht_ui_apply(bytes.length), 0, bad.name);
            assert.deepEqual(
                Buffer.from(pixels),
                previous,
                'Rejected snapshot changed the display'
            );
        }
    }
} finally {
    fs.rmSync(directory, { recursive: true, force: true });
}
console.log(
    `${fixtures.valid.length} native/WASM frames match; malformed snapshots preserve the prior frame`
);
