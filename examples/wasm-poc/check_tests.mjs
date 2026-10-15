// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';

const directory = process.argv[2];
const manifest = JSON.parse(await readFile(path.join(directory, 'manifest.json')));
for (const program of manifest) {
    const module = await WebAssembly.compile(await readFile(path.join(directory, program.file)));
    assert.deepEqual(WebAssembly.Module.imports(module), []);
    const { exports: api } = await WebAssembly.instantiate(module);
    assert.equal(api.get_output_count(), 0, 'must not pass before main executes');
    assert.equal(api.main(), 0);
    assert.equal(api.get_output_count(), program.expected);
    assert.equal(api.memory.buffer.byteLength, 65536);
    assert.throws(() => api.memory.grow(1), RangeError);
    console.log(`PASS: ${program.file}, result ${program.expected}, ${program.bytes} bytes`);
}
