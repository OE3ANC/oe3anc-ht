// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { encodeCodeplug, decodeCodeplug } from '../../companion/src/codeplug-wire.mjs';
import { canonical } from '../../companion/src/codeplug.mjs';
const fixtures = JSON.parse(
    fs.readFileSync(new URL('../../protocol/companion/cps-fixtures.json', import.meta.url))
);
for (const item of fixtures.valid) {
    const wire = Uint8Array.from(Buffer.from(item.wire, 'hex'));
    assert.deepEqual(encodeCodeplug(item.document), wire, item.name);
    assert.equal(canonical(decodeCodeplug(wire)), canonical(item.document), item.name);
}
for (const item of fixtures.invalid) {
    assert.throws(
        () => decodeCodeplug(Uint8Array.from(Buffer.from(item.wire, 'hex'))),
        undefined,
        item.name
    );
}
console.log(
    `${fixtures.valid.length} valid and ${fixtures.invalid.length} rejected paired CPS binary vectors passed`
);
