// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { C, crc32 } from '../../companion/src/protocol.mjs';
import { CpsTransfer } from '../../companion/src/cps-transfer.mjs';
import { encodeCodeplug, decodeCodeplug } from '../../companion/src/codeplug-wire.mjs';
import { canonical } from '../../companion/src/codeplug.mjs';
const fixtures = JSON.parse(
    fs.readFileSync(new URL('../../protocol/companion/cps-fixtures.json', import.meta.url))
);

function put(bytes, offset, value) {
    new DataView(bytes.buffer).setUint32(offset, value, true);
}

function get(bytes, offset) {
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

class Radio {
    constructor(document = fixtures.valid[0].document) {
        this.document = document;
        this.session = 42n;
        this.info = { capabilities: C.CAP_CPS };
        this.sequence = 0;
        this.commits = 0;
        this.cancelled = 0;
        this.revision = 1;
        this.failChunk = false;
    }

    async transfer(work) {
        return work();
    }

    async request(type, payload = new Uint8Array()) {
        const id = ++this.sequence;
        if (type === C.MSG_CPS_READ) {
            this.bytes = encodeCodeplug(this.document);
            this.token = id;
            this.readRevision = this.revision;
            this.readOffset = 0;
            const reply = new Uint8Array(31);
            put(reply, 1, id);
            put(reply, 5, this.revision);
            put(reply, 17, this.bytes.length);
            put(reply, 21, crc32(this.bytes));
            return { payload: reply };
        }
        if (get(payload, 0) !== this.token) {
            return { payload: Uint8Array.of(C.STATUS_STALE) };
        }
        if (type === C.MSG_CPS_READ_CHUNK) {
            const offset = get(payload, 4);
            const count = new DataView(payload.buffer).getUint16(8, true);
            assert.equal(offset, this.readOffset);
            this.readOffset += count;
            const reply = new Uint8Array(9 + count);
            put(reply, 1, this.token);
            put(reply, 5, offset);
            reply.set(this.bytes.subarray(offset, offset + count), 9);
            if (this.badRead) {
                reply[9] ^= 1;
            }
            return { payload: reply };
        }
        if (type === C.MSG_CPS_WRITE) {
            if (this.revision !== this.readRevision) {
                return { payload: Uint8Array.of(C.STATUS_STALE) };
            }
            this.upload = new Uint8Array(get(payload, 4));
            this.checksum = get(payload, 8);
            this.writeOffset = 0;
            this.token = id;
            const reply = new Uint8Array(5);
            put(reply, 1, id);
            return { payload: reply };
        }
        if (type === C.MSG_CPS_WRITE_CHUNK) {
            if (this.failChunk) {
                throw new Error('Unplugged');
            }
            assert.equal(get(payload, 4), this.writeOffset);
            this.upload.set(payload.subarray(8), this.writeOffset);
            this.writeOffset += payload.length - 8;
        }
        if (type === C.MSG_CPS_COMMIT) {
            assert.equal(this.writeOffset, this.upload.length);
            assert.equal(crc32(this.upload), this.checksum);
            this.document = decodeCodeplug(this.upload);
            this.commits++;
            this.revision++;
            this.accepted = true;
            if (this.lostCommit) {
                throw new Error('Radio response timed out');
            }
            const reply = new Uint8Array(5);
            put(reply, 1, this.token);
            return { payload: reply };
        }
        if (type === C.MSG_CPS_STATUS) {
            const reply = new Uint8Array(16);
            put(reply, 1, this.token);
            reply[5] = this.states?.shift() ?? C.CPS_STATE_DURABLE;
            put(reply, 6, reply[5] === C.CPS_STATE_ACCEPTED ? 0 : this.revision);
            put(reply, 10, 1);
            if (reply[5] === C.CPS_STATE_APPLIED) {
                reply[15] = C.STATUS_FAILED;
            }
            return { payload: reply };
        }
        if (type === C.MSG_CPS_CANCEL) {
            if (this.accepted) {
                return { payload: Uint8Array.of(C.STATUS_BUSY) };
            }
            this.cancelled++;
            this.token = 0;
        }
        return { payload: Uint8Array.of(C.STATUS_OK) };
    }
}
const radio = new Radio(fixtures.valid.find(item => item.name === 'maximum').document);
const progress = [];
const transfer = new CpsTransfer(radio, value => progress.push(value));
const baseline = await transfer.read();
assert.equal(canonical(baseline.document), canonical(radio.document));
const draft = structuredClone(baseline.document);
draft.global.ui.theme = 'nord';
const retained = canonical(draft);
radio.states = [C.CPS_STATE_ACCEPTED, C.CPS_STATE_APPLIED, C.CPS_STATE_DURABLE];
assert.equal((await transfer.write(draft)).state, C.CPS_STATE_DURABLE);
assert.equal(radio.commits, 1);
assert.equal(canonical(draft), retained);
assert.equal(canonical(radio.document), retained);
assert.equal(transfer.baseline, null);
assert(progress.some(value => value.result?.saveError === C.STATUS_FAILED));
await assert.rejects(transfer.write(draft), /Read the radio before writing/);
await transfer.read();
radio.revision++;
await assert.rejects(transfer.write(draft), /radio changed/);
assert.equal(transfer.baseline, null);
assert.equal(canonical(draft), retained);
const cancellingRadio = new Radio(),
    cancelling = new CpsTransfer(cancellingRadio, value => {
        if (value.operation === 'write') {
            cancelling.cancel();
        }
    });
await cancelling.read();
await assert.rejects(cancelling.write(draft), /cancelled/);
assert.equal(cancellingRadio.commits, 0);
assert(cancellingRadio.cancelled);
const failedRadio = new Radio();
const failed = new CpsTransfer(failedRadio);
await failed.read();
failedRadio.failChunk = true;
await assert.rejects(failed.write(draft), /Unplugged/);
assert(failedRadio.cancelled);
assert.equal(failedRadio.commits, 0);
failedRadio.failChunk = false;
await failed.read(); // Failure cleanup permits a fresh read.
const uncertainRadio = new Radio();
const uncertain = new CpsTransfer(uncertainRadio);
await uncertain.read();
uncertainRadio.lostCommit = true;
await assert.rejects(uncertain.write(draft), /Commit may have completed/);
assert.equal(uncertainRadio.commits, 1);
const corruptRadio = new Radio();
const corrupt = new CpsTransfer(corruptRadio);
corruptRadio.badRead = true;
await assert.rejects(corrupt.read(), /CRC mismatch/);
assert.equal(corrupt.baseline, null);
console.log(
    'CPS client full transfer, stale draft, cancellation, save retry and uncertain commit checks passed'
);
