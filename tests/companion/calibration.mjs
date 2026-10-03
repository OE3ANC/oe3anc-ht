// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { decodeCalibration } from '../../companion/src/calibration.mjs';

// Synthetic journals exercise the pinned OpenRTX format. These are not radio dumps.

function journal(length, count, fill = 255) {
    const bytes = new Uint8Array(0x50000).fill(fill);
    for (let i = 0; i < count; i++) {
        const offset = i * length;
        bytes.fill(0, offset, offset + length);
        bytes.set([0x5a, 0, length & 255, length >> 8], offset);
    }
    return bytes;
}

function checksum(bytes, offset, length) {
    bytes[offset + 1] =
        bytes.subarray(offset + 4, offset + length).reduce((sum, byte) => sum + byte, 0) & 255;
}

function record(bytes, entry, slot, frequency = 440125000, duties = [31, 74, 65, 100]) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    const offset = entry + 0x450 + slot * 36;
    view.setUint32(offset, frequency, true);
    duties.forEach((duty, i) => view.setUint16(offset + 4 + i * 2, duty, true));
    bytes[offset + 12] = 0x81;
    bytes[offset + 35] = 0xfe;
}
for (const [length, count, expectedEntry, expectedTable] of [
    [0xd48, 13, 0x3b9f60, 0x3ba3b0],
    [0xbdc, 6, 0x3b3b4c, 0x3b3f9c],
    [0xbdc, 11, 0x3b7698, 0x3b7ae8]
]) {
    const bytes = journal(length, count);
    const final = (count - 1) * length;
    record(bytes, final, 2);
    checksum(bytes, final, length);
    const result = decodeCalibration(bytes);
    assert.equal(result.latest, expectedEntry);
    assert.equal(result.warning, '');
    assert.equal(result.entries.length, count);
    const entry = result.entries.at(-1);
    const row = entry.records[0];
    assert.equal(entry.slots, length === 0xbdc ? 10 : 20);
    assert.equal(row.offset, expectedTable + 2 * 36);
    assert.equal(row.frequency, 440125000);
    assert.deepEqual(row.duties, [31, 74, 65, 100]);
    assert.equal(row.band, 'UHF');
    assert.deepEqual(row.flags, []);
    assert.equal(row.raw.split(' ').length, 36);
    assert.equal(row.unknown.split(' ').length, 24);
    assert.match(row.unknown, /^81 .* fe$/);
    // A subview must respect its byteOffset and never use unrelated buffer bytes.
    const backing = new Uint8Array(bytes.length + 11);
    backing.set(bytes, 7);
    assert.deepEqual(decodeCalibration(backing.subarray(7, 7 + bytes.length)), result);
}
const bytes = journal(0xd48, 3);
record(bytes, 0, 0, 136125000);
checksum(bytes, 0, 0xd48);
record(bytes, 0xd48, 1);
checksum(bytes, 0xd48, 0xd48);
bytes[0xd48 + 1] ^= 1;
record(bytes, 2 * 0xd48, 0, 123, [101, 65535, 0, 100]);
checksum(bytes, 2 * 0xd48, 0xd48);
let result = decodeCalibration(bytes);
assert.equal(result.entries[0].records[0].band, 'VHF');
assert.equal(result.entries[1].valid, false);
assert.deepEqual(result.entries[1].records, []);
assert.equal(result.latest, 0x3b0000 + 2 * 0xd48); // Suspicious newest table stays selected.
assert.deepEqual(result.entries[2].records[0].flags, [
    'Unknown reference frequency',
    'PWM duty exceeds 100%'
]);
// Entirely zero and erased records are skipped, but nonzero unknown fields are retained.
const emptyRecords = journal(0xbdc, 1);
emptyRecords.fill(255, 0x450 + 36, 0x450 + 72);
emptyRecords[0x450 + 72 + 12] = 1;
// The bytes after ten slots belong to other settings, not an eleventh RF slot.
record(emptyRecords, 0, 10);
checksum(emptyRecords, 0, 0xbdc);
result = decodeCalibration(emptyRecords);
assert.equal(result.entries[0].records.length, 1);
assert.equal(result.entries[0].records[0].slot, 2);
assert.equal(result.entries[0].records[0].frequency, 0);
for (const fill of [0, 255]) {
    result = decodeCalibration(new Uint8Array(0x10000).fill(fill));
    assert.equal(result.latest, null);
    assert.equal(result.warning, '');
    assert.deepEqual(result.entries, []);
}
const invalid = journal(0xbdc, 1);
invalid[1] = 1;
assert.equal(decodeCalibration(invalid).latest, null);
// Recognized checksum failures still establish the layout and advance by its length.
const changed = journal(0xbdc, 1);
changed[1] = 1;
changed.set([0x5a, 0, 0x48, 0x0d], 0xbdc);
result = decodeCalibration(changed);
assert.equal(result.entries.length, 1);
assert.equal(result.latest, null);
assert.match(result.warning, /layout changes/);
for (const header of [
    [0x42, 0, 0xdc, 0x0b],
    [0x5a, 0, 0xff, 0xff]
]) {
    const malformed = journal(0xbdc, 3);
    malformed.set(header, 0xbdc);
    result = decodeCalibration(malformed);
    assert.equal(result.entries.length, 1);
    assert.equal(result.latest, 0x3b0000);
    assert.equal(result.stoppedAt, 0x3b0000 + 0xbdc);
    assert.match(result.warning, /framing/);
}
for (const length of [0xbdc, 0xd48]) {
    const count = Math.floor(0x10000 / length);
    const truncated = journal(length, count);
    truncated.set([0x5a, 0, length & 255, length >> 8], count * length);
    result = decodeCalibration(truncated);
    assert.equal(result.entries.length, count);
    assert.match(result.warning, /Truncated entry/);
    assert.equal(result.stoppedAt, 0x3b0000 + count * length);
}
assert.throws(() => decodeCalibration(new Uint8Array(0xffff)), /complete 64 KiB/);
assert.throws(() => decodeCalibration(new ArrayBuffer(0x10000)), /complete 64 KiB/);
console.log(
    'PASS: pinned calibration examples, 10/20-slot journals, all selectors/unknown bytes, checksum history, malformed/bounded scans and suspicious newest selection (synthetic fixtures)'
);
