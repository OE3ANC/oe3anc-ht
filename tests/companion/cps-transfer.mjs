// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { C, crc32 } from '../../companion/src/protocol.mjs';
import { CpsTransfer } from '../../companion/src/cps-transfer.mjs';
import { ConnectedCps } from '../../companion/src/connected-cps.mjs';
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
        this.reads = 0;
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
            this.reads++;
            await this.onRead?.();
            this.bytes = encodeCodeplug(this.document);
            this.token = id;
            this.readRevision = this.revision;
            this.readOffset = 0;
            const reply = new Uint8Array(31);
            put(reply, 1, id);
            put(reply, 5, this.revision);
            put(reply, 17, this.bytes.length);
            put(reply, 21, crc32(this.bytes));
            reply[29] = this.protected ? 2 : 0;
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

function connectedEditor(radio = new Radio()) {
    const nodes = new Map();
    const root = {
        querySelector(selector) {
            if (!nodes.has(selector)) {
                nodes.set(selector, {
                    addEventListener() {},
                    classList: { toggle() {} }
                });
            }
            return nodes.get(selector);
        }
    };
    const editor = {
        document: structuredClone(radio.document),
        importSequence: 0,
        dialog: root,
        confirmations: [],
        confirm(text, accept, showReview) {
            assert.equal(showReview, true);
            this.confirmations.push({ text, accept });
        }
    };
    editor.document.global.ui.theme = 'nord';
    return { radio, editor, cps: new ConnectedCps(root, editor, radio) };
}

const connected = connectedEditor();
const imported = canonical(connected.editor.document);
assert.equal(connected.cps.writeButton.disabled, false);
const reviewing = connected.cps.review();
assert.equal(connected.cps.writeButton.disabled, true);
await connected.cps.review(); // A second click cannot start another read.
await reviewing;
assert.equal(connected.radio.reads, 1);
assert.equal(connected.radio.commits, 0);
assert.equal(canonical(connected.editor.document), imported);
assert.equal(
    connected.editor.dialog.querySelector('#cps-before').value,
    canonical(connected.radio.document)
);
assert.equal(connected.editor.dialog.querySelector('#cps-after').value, imported);
assert.equal(connected.editor.confirmations.length, 1);
assert.equal(connected.cps.writeButton.disabled, false);
await connected.cps.review(); // Cancelling a review permits reuse of its completed baseline.
assert.equal(connected.radio.reads, 1);
await connected.editor.confirmations[1].accept();
assert.equal(connected.radio.commits, 1);
assert.equal(canonical(connected.radio.document), imported);
assert.equal(canonical(connected.editor.document), imported);

for (const failure of ['crc', 'cancel', 'protected', 'session', 'draft', 'dialog', 'import']) {
    const item = connectedEditor();
    const savedDraft = canonical(item.editor.document);
    if (failure === 'crc') {
        item.radio.badRead = true;
    } else if (failure === 'protected') {
        item.radio.protected = true;
    } else {
        item.radio.onRead = () => {
            if (failure === 'cancel') {
                item.cps.transfer.cancel();
            } else if (failure === 'session') {
                item.radio.session++;
            } else if (failure === 'dialog') {
                item.editor.dialog.open = true;
            } else if (failure === 'import') {
                item.editor.importSequence++;
            } else {
                item.editor.dirtyForm = true;
            }
        };
    }
    await item.cps.review();
    assert.equal(item.editor.confirmations.length, 0, failure);
    assert.equal(item.radio.commits, 0, failure);
    assert.equal(canonical(item.editor.document), savedDraft, failure);
    assert.equal(item.cps.writeButton.disabled, false, failure);
    assert.match(item.cps.status.textContent, /Your local draft is retained/, failure);
}

connected.radio.session = 0n;
connected.cps.update();
assert.equal(connected.cps.writeButton.disabled, true);
connected.radio.session = 43n;
connected.radio.info.capabilities = 0;
connected.cps.update();
assert.equal(connected.cps.writeButton.disabled, true);
console.log(
    'CPS transfer and connected review: automatic read, retained imports, cancellation and stale guards passed'
);
