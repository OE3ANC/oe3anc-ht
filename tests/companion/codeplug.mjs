// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
    parse,
    parseBytes,
    canonical,
    validate,
    fresh,
    putRecord,
    removeRecord,
    MAX_ID,
    targetErrors
} from '../../companion/src/codeplug.mjs';
const cases = JSON.parse(readFileSync(process.argv[2]));
for (const item of cases) {
    if (item.expected === null) {
        assert.throws(() => parse(item.text), undefined, item.name);
    } else {
        const document = parse(item.text);
        const before = JSON.stringify(document);
        assert.equal(canonical(document), item.expected, item.name);
        assert.equal(JSON.stringify(document), before, 'validation/export cannot mutate caller');
        assert.equal(
            canonical(parse(canonical(document))),
            item.expected,
            item.name + ' round trip'
        );
    }
}
assert.throws(() => parseBytes(Uint8Array.of(0xff)), /encoded data/);
assert.throws(() => parseBytes(new TextEncoder().encode('\ufeff' + canonical(fresh()))));
assert.equal(
    canonical(parseBytes(new TextEncoder().encode(canonical(fresh())))),
    canonical(fresh())
);
let document = fresh();
assert.equal(
    canonical(document),
    canonical(
        JSON.parse(readFileSync(new URL('../../examples/codeplug/empty.json', import.meta.url)))
    )
);
const config = document.vfo;
const makeChannel = (number, id = 0) => ({
    id,
    number,
    name: 'Channel ' + number,
    configuration: config
});
let result = putRecord(document, 'channels', makeChannel(1));
document = result.document;
assert.equal(result.id, 1);
assert.equal(document.allocation.channel_id_high_water, 1);
const original = canonical(document);
assert.throws(() => putRecord(document, 'channels', makeChannel(1)), /duplicate/);
assert.equal(canonical(document), original, 'failed edit leaves input/counters unchanged');
result = putRecord(document, 'channels', makeChannel(2));
document = result.document;
result = putRecord(document, 'banks', { id: 0, name: 'Bank', channel_ids: [2, 1] });
document = result.document;
assert.equal(result.id, 1);
assert.deepEqual(document.banks[0].channel_ids, [2, 1]);
document.selection = { operating: 'memory', bank_id: 1, channel_id: 1 };
const rebanked = putRecord(document, 'banks', { id: 1, name: 'Bank', channel_ids: [2] }).document;
assert.equal(rebanked.selection.bank_id, null);
assert.equal(rebanked.selection.channel_id, 1);
assert.equal(document.selection.bank_id, 1, 'edit cannot mutate source');
document = removeRecord(document, 'channels', 1);
assert.deepEqual(document.banks[0].channel_ids, [2]);
assert.deepEqual(document.selection, { operating: 'vfo', bank_id: 1, channel_id: null });
assert.equal(document.allocation.channel_id_high_water, 2);
result = putRecord(document, 'channels', makeChannel(1));
document = result.document;
assert.equal(result.id, 3, 'deleted IDs never reused');
document = removeRecord(document, 'banks', 1);
assert.equal(document.allocation.bank_id_high_water, 1);
assert.equal(document.selection.bank_id, null);
assert.equal(putRecord(document, 'banks', { id: 0, name: 'Again', channel_ids: [] }).id, 2);
document.allocation.channel_id_high_water = MAX_ID;
const exhausted = canonical(document);
assert.throws(() => putRecord(document, 'channels', makeChannel(3)), /exhausted/);
assert.equal(canonical(document), exhausted);
const last = document.channels[0];
assert.equal(
    putRecord(document, 'channels', { ...last, name: 'Edited' }).id,
    last.id,
    'editing works at exhaustion'
);
assert.throws(() => removeRecord(document, 'channels', 99), /no longer exists/);
assert.deepEqual(targetErrors(document), []);
document.global.gain = 1;
assert.equal(targetErrors(document).length, 1);
validate(document);
console.log(
    `${cases.length} browser/Python JSON parity cases and draft identity/membership checks passed`
);
