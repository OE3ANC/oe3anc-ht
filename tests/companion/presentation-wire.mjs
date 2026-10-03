// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { decodePresentation, encodePresentation } from '../../companion/src/presentation-wire.mjs';
const fixtures = JSON.parse(
    fs.readFileSync(new URL('../../protocol/companion/ui-fixtures.json', import.meta.url))
);
for (const f of fixtures.valid) {
    const bytes = Uint8Array.from(Buffer.from(f.wire, 'hex'));
    assert.deepEqual(decodePresentation(bytes), f.document, f.name);
    assert.deepEqual(encodePresentation(f.document), bytes, f.name);
}
for (const f of fixtures.invalid) {
    assert.throws(
        () => decodePresentation(Uint8Array.from(Buffer.from(f.wire, 'hex'))),
        undefined,
        f.name
    );
}
console.log(
    `${fixtures.valid.length} valid and ${fixtures.invalid.length} rejected paired UI vectors passed`
);
