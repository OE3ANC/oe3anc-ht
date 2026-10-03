// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash, webcrypto } from 'node:crypto';
import { createFlashBackup } from '../../companion/src/flash-backup.mjs';
import { FirmwareTools } from '../../companion/src/firmware-tools.mjs';

class Element {
    constructor(tag = '') {
        this.tag = tag;
        this.children = [];
        this.open = false;
        this.checked = false;
        this.listeners = new Map();
        this.classList = { toggle() {} };
        this.disabled = false;
    }

    append(...children) {
        this.children.push(...children);
    }

    replaceChildren(...children) {
        this.children = children;
    }

    remove() {}

    addEventListener(name, callback) {
        this.listeners.set(name, callback);
    }

    click() {
        if (!this.disabled) {
            this.listeners.get('click')?.();
        }
    }

    showModal() {
        this.open = true;
    }

    close(value) {
        this.open = false;
        this.returnValue = value;
        this.listeners.get('close')?.();
    }

    change() {
        this.listeners.get('change')?.({ target: this });
        this.onchange?.({ target: this });
    }
}
const names = [
    '[data-firmware-status]',
    'progress',
    '[data-boot-connect]',
    '[data-boot-close]',
    '[data-flash-backup]',
    '[data-factory-read]',
    '[data-boot-cancel]',
    '[data-flash-review]',
    '[data-flash-ack]',
    '[data-flash-verify]',
    '[data-flash-confirm]',
    '[data-flash-dismiss]',
    '[data-flash-preview]',
    '[data-flash-review-title]',
    '[data-bundle-file]',
    '[data-backup-file]',
    '[data-flash-update]',
    '[data-flash-restore]',
    '[data-settings-reset]',
    '[data-restore-status]',
    '[data-bundle-status]',
    '[data-backup-status]',
    '[data-factory-download]',
    '[data-calibration]',
    '[data-flash-target]',
    '[data-flash-selection]'
];
const elements = new Map(names.map(name => [name, new Element()]));
const root = {
    querySelector: name => {
        assert.ok(elements.has(name), name);
        return elements.get(name);
    }
};
let available = false;
let chooser = 0;
let changed = 0;
let downloads = 0;
Object.defineProperty(globalThis, 'navigator', {
    value: {
        serial: {
            requestPort: async () => {
                chooser++;
            }
        }
    }
});
const tools = new FirmwareTools(root, {
    available: () => available,
    changed: () => {
        changed++;
    }
});
const onClosed = tools.connection.onClosed;
assert.equal(elements.get('[data-boot-connect]').disabled, true);
await tools.connect();
assert.equal(chooser, 0);
available = true;
tools.update();
assert.equal(elements.get('[data-boot-connect]').disabled, false);
let choose;
navigator.serial.requestPort = () => new Promise(resolve => (choose = resolve));
const connecting = tools.connect();
assert.equal(tools.ownsPort, true);
assert.equal(elements.get('[data-boot-cancel]').disabled, false);
elements.get('[data-boot-cancel]').click();
choose({});
await connecting;
assert.equal(tools.ownsPort, false);
assert.match(tools.status.textContent, /cancelled/);

// Exercise cancellation after a completed radio read, while packaging/hashing
// awaits the platform crypto API. Neither path may leave a ready port or download.
for (const [factory, downloadRaw] of [
    [false, false],
    [true, false],
    [true, true]
]) {
    let hashed;
    let finishHash;
    let closed = 0;
    const hashing = new Promise(resolve => (hashed = resolve));
    Object.defineProperty(globalThis, 'crypto', {
        configurable: true,
        value: {
            subtle: {
                digest: () => {
                    if (finishHash) {
                        return Promise.resolve(new ArrayBuffer(32));
                    }
                    return new Promise(resolve => {
                        finishHash = resolve;
                        hashed();
                    });
                }
            }
        }
    });
    globalThis.document = {
        createElement: tag => {
            const element = new Element(tag);
            if (tag === 'a') {
                element.click = () => downloads++;
            }
            return element;
        },
        body: { append() {} }
    };
    const bytes = new Uint8Array(factory ? (downloadRaw ? 0x50000 : 0x10000) : 0x400000);
    tools.connection = {
        ready: true,
        port: {},
        info: { target: 'c62', flashSize: 0x400000, flashId: 0x1640ef, chipId: '0102030405060708' },
        readRange: async (offset, size) => {
            assert.equal(offset, factory ? 0x3b0000 : 0);
            assert.equal(size, bytes.length);
            return bytes;
        },
        close: async () => {
            closed++;
            tools.connection.ready = false;
            tools.connection.port = null;
        }
    };
    tools.update();
    const reading = tools.read(factory, downloadRaw);
    await hashing;
    assert.equal(elements.get('[data-boot-close]').disabled, true);
    assert.equal(elements.get('[data-flash-backup]').disabled, true);
    elements.get('[data-boot-cancel]').click();
    finishHash(new ArrayBuffer(32));
    await reading;
    assert.equal(closed, 1);
    assert.equal(downloads, 0);
    assert.equal(tools.ownsPort, false);
    assert.equal(elements.get('[data-flash-backup]').disabled, true);
    assert.equal(elements.get('[data-boot-connect]').disabled, false);
    assert.match(tools.status.textContent, /no successful backup or download/);
    assert.equal(tools.factoryBytes ?? null, null);
    assert.equal(elements.get('[data-calibration]').children.length, 0);
}
assert.ok(changed >= 6);
Object.defineProperty(globalThis, 'crypto', { configurable: true, value: webcrypto });
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const dsp = new Uint8Array(
    await readFile(new URL('../../backends/c62/resources/dsp_firmware.bin', import.meta.url))
);
for (const offset of [0x46150, 0x461b4, 0x461f4]) {
    dsp.set([0x0c, 0x02, 0x1d, 0xf0, 0x3d, 0xf0], offset + 3);
}
const identity = 'dev-' + 'a'.repeat(32);
const app = new Uint8Array(4101);
app.set(new TextEncoder().encode(identity + '\0'));
const bundleText = JSON.stringify({
    format: 'oe3anc-ht-firmware',
    schema_version: 1,
    target: 'c62',
    flash_size: 0x400000,
    erase_size: 4096,
    release: {
        identity,
        label: 'development',
        commit: 'b'.repeat(40),
        source_sha256: 'a'.repeat(64)
    },
    images: [app, dsp].map((bytes, index) => ({
        role: index ? 'dsp' : 'application',
        offset: index * 0x100000,
        size: bytes.length,
        sha256: hash(bytes),
        data_base64: Buffer.from(bytes).toString('base64')
    }))
});
const info = { target: 'c62', flashSize: 0x400000, flashId: 0x1640ef, chipId: '0102030405060708' };
const backupText = await createFlashBackup(info, new Uint8Array(0x400000));
const file = (name, text) => new File([text], name, { type: 'application/json' });
const get = name => elements.get('[' + name + ']');
let writes = 0;
let closes = 0;
const verificationChoices = [];
const imageChoices = [];

function connected(chipId = info.chipId) {
    tools.connection = {
        ready: true,
        port: {},
        info: { ...info, chipId },
        close: async () => {
            closes++;
            tools.connection.ready = false;
            tools.connection.port = null;
        },
        updateFirmware: async (text, options) => {
            assert.equal(text, bundleText);
            assert.equal(options.acknowledged, true);
            verificationChoices.push(options.verifyAfterWrite);
            imageChoices.push(options.applicationOnly);
            writes++;
            return identity;
        },
        restoreBackup: async (text, options) => {
            assert.equal(text, backupText);
            assert.equal(options.acknowledged, true);
            verificationChoices.push(options.verifyAfterWrite);
            imageChoices.push(options.applicationOnly);
            writes++;
            return hash(new Uint8Array(0x400000));
        }
    };
    tools.update();
}

async function waitForPreview() {
    const deadline = Date.now() + 3000;
    while (!tools.dialog.open && tools.busy && Date.now() < deadline) {
        await new Promise(resolve => setTimeout(resolve, 1));
    }
    assert.equal(tools.dialog.open, true, tools.status.textContent);
}
// File validation is offline and reserves controls while asynchronous reads run.
available = false;
tools.connection = { ready: false };
await tools.selectFile(file('pair.json', bundleText), false);
assert.equal(tools.bundle.text, bundleText);
assert.equal(writes, 0);
await tools.selectFile(file('backup.json', backupText), true);
assert.equal(get('data-flash-restore').disabled, true);
await tools.selectFile(
    {
        name: 'too-big',
        size: 3 * 1024 * 1024 + 1,
        arrayBuffer: () => assert.fail('oversized file was read')
    },
    false
);
assert.equal(tools.bundle, null);
await tools.selectFile(file('invalid.json', '{}'), false);
assert.equal(tools.bundle, null);
let finishFile;
const choosing = tools.selectFile(
    {
        name: 'pair.json',
        size: 10,
        arrayBuffer: () => new Promise(resolve => (finishFile = resolve))
    },
    false
);
assert.equal(get('data-bundle-file').disabled, true);
get('data-boot-cancel').click();
finishFile(new TextEncoder().encode(bundleText).buffer);
await choosing;
assert.equal(tools.bundle, null);
assert.equal(writes, 0);
for (const restoring of [false, true]) {
    const text = restoring ? backupText : bundleText;
    for (const prefix of [Uint8Array.of(0xef, 0xbb, 0xbf), Uint8Array.of(0xff)]) {
        await tools.selectFile(new File([prefix, text], 'bad-bytes.json'), restoring);
        assert.equal(restoring ? tools.backup : tools.bundle, null);
    }
}
await tools.selectFile(file('backup.json', backupText), true);
await tools.selectFile(file('pair.json', bundleText), false);
available = true;
connected();
for (const restoring of [false, true]) {
    let pending = tools.write(restoring);
    await waitForPreview();
    assert.equal(get('data-boot-close').disabled, true);
    assert.equal(get('data-bundle-file').disabled, true);
    assert.equal(tools.confirm.disabled, true);
    assert.equal(tools.acknowledgement.checked, false);
    assert.equal(tools.verifyAfterWrite.checked, true);
    tools.verifyAfterWrite.checked = false;
    tools.confirm.listeners.get('click')(); // Bypassing disabled cannot bypass acknowledgement.
    assert.equal(tools.dialog.open, true);
    assert.equal(writes, 0);
    const preview = get('data-flash-preview').textContent;
    assert.match(preview, /0102030405060708/);
    assert.match(
        preview,
        restoring ? /Replaces ALL flash.*factory calibration/ : /application: 0x000000/
    );
    if (!restoring) {
        assert.match(preview, /dsp: 0x100000/);
        assert.match(preview, /0x002000/);
    }
    tools.acknowledgement.checked = true;
    tools.acknowledgement.change();
    get('data-flash-dismiss').click();
    await pending;
    assert.equal(writes, 0);
    assert.equal(tools.connection.ready, true);
    assert.equal(tools.busy, false);
    // Escape/native close must not reuse the previous accepted checkbox state.
    pending = tools.write(restoring);
    await waitForPreview();
    assert.equal(tools.acknowledgement.checked, false);
    assert.equal(tools.confirm.disabled, true);
    assert.equal(tools.verifyAfterWrite.checked, true);
    tools.dialog.close('');
    await pending;
    assert.equal(writes, 0);
}
// Missing, cross-radio or absent-backup IDs must prevent even a review operation.
for (const chipId of [null, '1112131415161718']) {
    connected(chipId);
    assert.equal(get('data-flash-restore').disabled, true);
    await tools.write(true);
    assert.equal(tools.busy, false);
    assert.equal(writes, 0);
    assert.match(get('data-restore-status').textContent, /unavailable/);
}
const missingId = JSON.parse(backupText);
missingId.device.chip_id = null;
await tools.selectFile(file('no-id.json', JSON.stringify(missingId)), true);
connected();
await tools.write(true);
assert.equal(writes, 0);
await tools.selectFile(file('backup.json', backupText), true);
// Port loss while reviewing cancels the modal before any transport mutation.
connected();
let pending = tools.write(false);
await waitForPreview();
tools.connection.ready = false;
tools.connection.port = null;
onClosed(new Error('Serial disconnected'));
await pending;
assert.equal(writes, 0);
// A changed connection after confirmation is also rejected before writing.
connected();
pending = tools.write(false);
await waitForPreview();
tools.acknowledgement.checked = true;
tools.acknowledgement.change();
tools.connection.info = { ...info };
tools.confirm.click();
await pending;
assert.equal(writes, 0);
assert.equal(tools.connection.ready, false);
for (const [restoring, verifyAfterWrite, applicationOnly] of [
    [false, true, false],
    [true, true, false],
    [false, false, false],
    [true, false, false],
    [false, true, true],
    [false, false, true]
]) {
    connected();
    pending = tools.write(restoring);
    await waitForPreview();
    assert.equal(tools.verifyAfterWrite.checked, true);
    assert.equal(tools.flashTarget.value, 'pair');
    assert.equal(tools.flashSelection.hidden, restoring);
    if (applicationOnly) {
        tools.acknowledgement.checked = true;
        tools.acknowledgement.change();
        tools.flashTarget.value = 'application';
        tools.flashTarget.change();
        assert.equal(tools.acknowledgement.checked, false);
        assert.equal(tools.confirm.disabled, true);
        assert.match(get('data-flash-preview').textContent, /application: 0x000000/);
        assert.doesNotMatch(get('data-flash-preview').textContent, /dsp: 0x100000/);
        assert.match(get('data-flash-preview').textContent, /DSP is preserved/);
        assert.equal(tools.confirm.textContent, 'Flash application');
    }
    tools.verifyAfterWrite.checked = verifyAfterWrite;
    tools.acknowledgement.checked = true;
    tools.acknowledgement.change();
    tools.confirm.click();
    await pending;
    assert.equal(verificationChoices.at(-1), verifyAfterWrite);
    assert.equal(imageChoices.at(-1), applicationOnly);
    if (verifyAfterWrite) {
        assert.match(tools.status.textContent, /every written byte verified/);
    } else {
        assert.match(tools.status.textContent, /readback verification skipped/);
        assert.doesNotMatch(tools.status.textContent, /written byte verified/);
    }
    if (applicationOnly) {
        assert.match(tools.status.textContent, /Application-only update/);
    }
    assert.equal(tools.flashTarget.onchange, null);
}
assert.equal(writes, 6);
// Partial mutation must be described as incomplete, with manual recovery.
connected();
tools.connection.updateFirmware = async () => {
    throw Object.assign(new Error('Lost write response'), { flashMayHaveChanged: true });
};
pending = tools.write(false);
await waitForPreview();
tools.acknowledgement.checked = true;
tools.acknowledgement.change();
tools.confirm.click();
await pending;
assert.match(tools.status.textContent, /Flash may be incomplete/);
assert.match(tools.status.textContent, /repeat the complete paired update/);
assert.equal(tools.busy, false);
assert.equal(tools.connection.ready, false);
assert.ok(closes >= 2);
assert.equal(downloads, 0);
// Inspection reads only the complete journal; a separate raw download reads all factory bytes.
connected();
const factory = new Uint8Array(0x50000).fill(255);
factory.fill(0, 0, 0xd48);
factory.set([0x5a, 0, 0x48, 0x0d]);
const factoryView = new DataView(factory.buffer);
factoryView.setUint32(0x450, 440125000, true);
[31, 74, 65, 100].forEach((value, i) => factoryView.setUint16(0x454 + 2 * i, value, true));
factory[1] = factory.subarray(4, 0xd48).reduce((sum, byte) => sum + byte, 0) & 255;
const reads = [];
tools.connection.readRange = async (offset, size) => {
    assert.equal(offset, 0x3b0000);
    reads.push(size);
    return factory.slice(0, size);
};
await tools.read(true);
assert.deepEqual(reads, [0x10000]);
assert.equal(writes, 6);
assert.equal(downloads, 0);
assert.equal(tools.factoryBytes, null);
assert.equal(get('data-factory-download').disabled, false);
const allText = element => [element.textContent ?? '', ...element.children.map(allText)].join('\n');
const tables = get('data-calibration');
assert.match(allText(tables), /0102030405060708/);
assert.match(allText(tables), /Verified region \[0x3b0000, 0x3c0000\)/);
assert.match(allText(tables), new RegExp(hash(factory.subarray(0, 0x10000))));
assert.match(allText(tables), /Selector 3/);
assert.match(allText(tables), /440125000/);
assert.equal(tables.children.find(element => element.tag === 'details').open, true);
const createUrl = URL.createObjectURL;
let downloadedBlob;
URL.createObjectURL = value => {
    downloadedBlob = value;
    return createUrl(value);
};
// Exercise the actual button handler, including port reservation while reading/hashing.
get('data-factory-download').click();
assert.equal(tools.busy, true);
assert.equal(get('data-factory-download').disabled, true);
const deadline = Date.now() + 3000;
while (tools.busy && Date.now() < deadline) {
    await new Promise(resolve => setTimeout(resolve, 1));
}
assert.equal(tools.busy, false, tools.status.textContent);
assert.deepEqual(reads, [0x10000, 0x50000]);
assert.equal(downloads, 1);
assert.deepEqual(new Uint8Array(await downloadedBlob.arrayBuffer()), factory);
assert.deepEqual(tools.factoryBytes, factory);
assert.match(allText(tables), /Verified region \[0x3b0000, 0x400000\)/);
assert.match(allText(tables), new RegExp(hash(factory)));
// Keep the provenance-labelled snapshot and the complete cached download after disconnect.
await tools.connection.close();
tools.update();
get('data-factory-download').click();
URL.createObjectURL = createUrl;
assert.equal(downloads, 2);
assert.deepEqual(reads, [0x10000, 0x50000]);
assert.deepEqual(new Uint8Array(await downloadedBlob.arrayBuffer()), factory);
// Starting another read clears stale output. A failed read must not expose previous raw bytes.
connected();
tools.connection.readRange = async () => {
    throw new Error('Read failed');
};
await tools.read(true, true);
assert.equal(tools.factoryBytes, null);
assert.equal(tables.children.length, 0);
assert.equal(downloads, 2);
await tools.connection.close();
tools.update();
assert.equal(get('data-factory-download').disabled, true);
console.log(
    'PASS: offline firmware/backup validation, fresh acknowledged previews, cancellation/identity/loss guards, update/restore and partial-write recovery'
);
console.log(
    'PASS: firmware UI port reservation, chooser cancellation and post-read packaging/hash cancellation without downloads'
);
console.log(
    'PASS: bounded calibration inspection, exact region/hash provenance, separately verified full raw download, offline cache and failed reread clears stale data'
);

// Reset needs no firmware/backup file and must always verify, even if checkbox state is altered.
let resets = 0;
for (const action of ['cancel', 'confirm', 'changed', 'partial']) {
    connected();
    tools.bundle = null;
    tools.backup = null;
    tools.connection.resetSettings = async options => {
        assert.equal(options.acknowledged, true);
        assert.equal(options.verifyAfterWrite, true);
        assert.equal(options.applicationOnly, false);
        resets++;
        if (action === 'partial') {
            throw Object.assign(new Error('Lost reset response'), { flashMayHaveChanged: true });
        }
    };
    tools.update();
    assert.equal(get('data-settings-reset').disabled, false);
    const before = resets;
    const reset = tools.write(false, true);
    await waitForPreview();
    assert.equal(get('data-settings-reset').disabled, true);
    assert.equal(tools.flashSelection.hidden, true);
    assert.equal(tools.verifyAfterWrite.checked, true);
    assert.equal(tools.verifyAfterWrite.disabled, true);
    assert.equal(tools.confirm.disabled, true);
    assert.match(get('data-flash-preview').textContent, /Deletes all saved settings, channels and banks/);
    assert.match(get('data-flash-preview').textContent, /\[0x308000, 0x348000\)/);
    assert.match(get('data-flash-preview').textContent, /factory calibration.*preserved/);
    tools.confirm.listeners.get('click')();
    assert.equal(resets, before);
    if (action === 'cancel') {
        tools.dialog.close('cancel');
    } else {
        tools.verifyAfterWrite.checked = false;
        tools.acknowledgement.checked = true;
        tools.acknowledgement.change();
        if (action === 'changed') {
            tools.connection.info = { ...info };
        }
        tools.confirm.click();
    }
    await reset;
    assert.equal(resets, before + (['confirm', 'partial'].includes(action) ? 1 : 0));
    assert.equal(tools.verifyAfterWrite.disabled, false);
    assert.equal(tools.busy, false);
    if (action === 'confirm') {
        assert.match(tools.status.textContent, /reset completed and verified/);
        assert.match(tools.status.textContent, /reboot the radio/);
    }
    if (action === 'partial') {
        assert.match(tools.status.textContent, /Settings reset may be incomplete/);
        assert.match(tools.status.textContent, /repeat the settings reset/);
        assert.doesNotMatch(tools.status.textContent, /radio may not boot/);
    }
}
console.log('PASS: file-independent settings reset preview, required verification, acknowledgement, cancellation, identity and partial-reset recovery');
