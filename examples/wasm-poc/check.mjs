// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const module = await WebAssembly.compile(await readFile(process.argv[2]));
assert.deepEqual(WebAssembly.Module.imports(module), []);
const { exports: api } = await WebAssembly.instantiate(module);
assert.equal(api.probe(), 42);
assert.equal(api.main(), 0);
assert.equal(api.memory.buffer.byteLength, 65536);
assert.throws(() => api.memory.grow(1), RangeError);
const pcm = new Int16Array(api.memory.buffer, api.buffer(), 480);
const fixture = [-32768, -32767, -3, -1, 0, 1, 3, 32767];
for (const count of [0, 80, 320, 480]) {
    for (const gain of [0, 16384, 32768]) {
        pcm.set(Array.from({ length: 480 }, (_, i) => fixture[i % fixture.length]));
        const before = Array.from(pcm);
        assert.equal(api.process(count, gain), count);
        assert.deepEqual(
            Array.from(pcm),
            before.map((sample, i) =>
                i < count ? Math.trunc((sample * gain) / 32768) || 0 : sample
            )
        );
    }
}
for (const [count, gain] of [
    [-1, 32768],
    [481, 32768],
    [480, -1],
    [480, 32769]
]) {
    const before = Array.from(pcm);
    assert.equal(api.process(count, gain), -1);
    assert.deepEqual(Array.from(pcm), before);
}
console.log('PASS: probe, main, no imports, fixed memory, PCM gain, bounds and rejection');
