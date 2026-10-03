// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import {
    parseFirmwareBundle,
    sha256,
    MAX_BUNDLE_BYTES
} from '../../companion/src/firmware-bundle.mjs';

const text = await readFile(process.argv[2], 'utf8');
const original = JSON.parse(text);
const parsed = await parseFirmwareBundle(text);
assert.equal(parsed.release.identity, original.release.identity);
assert.deepEqual(
    parsed.images.map(image => image.offset),
    [0, 0x100000]
);
for (const image of parsed.images) {
    assert.equal(image.bytes.length, image.size);
    assert.equal(await sha256(image.bytes), image.sha256);
}
// The largest allowed application and development identities remain valid.
const development = structuredClone(original);
development.release.label = 'development';
development.release.identity = 'dev-' + development.release.source_sha256.slice(0, 32);
const fullApp = new Uint8Array(0x100000);
fullApp.set(new TextEncoder().encode(development.release.identity + '\0'));
development.images[0].size = fullApp.length;
development.images[0].data_base64 = Buffer.from(fullApp).toString('base64');
development.images[0].sha256 = await sha256(fullApp);
assert.equal((await parseFirmwareBundle(JSON.stringify(development))).images[0].size, 0x100000);
let rejected = 0;

async function reject(text) {
    await assert.rejects(() => parseFirmwareBundle(text));
    rejected++;
}
for (const mutate of [
    d => (d.format = 'another-format'),
    d => (d.schema_version = 2),
    d => (d.target = 'other'),
    d => (d.flash_size *= 2),
    d => (d.erase_size = 8192),
    d => (d.extra = true),
    d => (d.release.commit = 'f'.repeat(40) + '\n'),
    d => (d.release.source_sha256 = 'no'),
    d => (d.release.identity += 'wrong'),
    d => (d.release.label += '\n'),
    d => d.images.pop(),
    d => d.images.reverse(),
    d => (d.images[0].offset = 4096),
    d => (d.images[1].offset = 0x300000),
    d => (d.images[0].size = 0),
    d => (d.images[0].size = true),
    d => (d.images[0].size = 0x100001),
    d => d.images[0].size++,
    d => (d.images[0].sha256 = 'f'.repeat(64)),
    d => (d.images[0].data_base64 = '!' + d.images[0].data_base64.slice(1)),
    d => (d.images[1].data_base64 = d.images[0].data_base64)
]) {
    const document = structuredClone(original);
    mutate(document);
    await reject(JSON.stringify(document));
}
for (const bad of [
    text.slice(0, -10),
    text + 'null',
    '\ufeff' + text,
    text.replace('"schema_version":1', '"schema_version":1,"schema_version":1'),
    text.replace('"schema_version":1', '"schema_version":1e0'),
    ' '.repeat(MAX_BUNDLE_BYTES + 1)
]) {
    await reject(bad);
}
// Recomputed integrity alone cannot admit an unrelated DSP or application.
for (const index of [0, 1]) {
    const document = structuredClone(original);
    const image = document.images[index];
    const bytes = Uint8Array.of(1, 2, 3, 4);
    image.size = bytes.length;
    image.data_base64 = 'AQIDBA==';
    image.sha256 = await sha256(bytes);
    await reject(JSON.stringify(document));
}
// Noncanonical padding bits decode to identical bytes but must be rejected.
const document = structuredClone(original);
const image = document.images[0];
image.size = 1;
image.data_base64 = 'Af==';
image.sha256 = await sha256(Uint8Array.of(1));
await reject(JSON.stringify(document));
console.log(`PASS: firmware bundle pair and ${rejected} invalid-package checks`);
