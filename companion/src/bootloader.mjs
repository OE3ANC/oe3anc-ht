// SPDX-License-Identifier: GPL-3.0-or-later
// CSK6 wire sequence follows LISTENAI/cskburn df83dc2067a5f16c8b825a6e3ad4d2d6511de303.
import {
    sha256,
    FLASH_SIZE,
    PARTITION_SIZE,
    ERASE_SIZE,
    parseFirmwareBundle
} from './firmware-bundle.mjs';
import { md5 } from './md5.mjs';

export const HELPER_SHA256 = 'f3aad327667b3e419bf4c1c90c1a0206a6d608a2d3633dbf6b54af8dc1796e65';
export const HELPER_SIZE = 38724;
export const FACTORY_OFFSET = 0x3b0000;
// Match the C62 NVS extent, not the entire storage partition (which includes factory data).
export const SETTINGS_OFFSET = 0x308000;
export const SETTINGS_SIZE = 0x40000;
const SYNC = 8;
const MAX_RESPONSE = 74;
export class BootloaderStatusError extends Error {}
export function words(...values) {
    const bytes = new Uint8Array(values.length * 4);
    const view = new DataView(bytes.buffer);
    values.forEach((value, index) => view.setUint32(index * 4, value, true));
    return bytes;
}
export function slipEncode(bytes) {
    const wire = [0xc0];
    for (const byte of bytes) {
        if (byte === 0xc0) {
            wire.push(0xdb, 0xdc);
        } else if (byte === 0xdb) {
            wire.push(0xdb, 0xdd);
        } else {
            wire.push(byte);
        }
    }
    wire.push(0xc0);
    return Uint8Array.from(wire);
}
export class SlipDecoder {
    constructor() {
        this.bytes = new Uint8Array(MAX_RESPONSE);
        this.reset();
    }

    reset() {
        this.active = false;
        this.length = 0;
        this.escape = false;
    }

    feed(byte) {
        if (byte === 0xc0) {
            const frame =
                this.active && !this.escape && this.length
                    ? this.bytes.slice(0, this.length)
                    : null;
            this.active = true;
            this.length = 0;
            this.escape = false;
            return frame;
        }
        if (!this.active) {
            return null;
        }
        if (this.escape) {
            if (byte !== 0xdc && byte !== 0xdd) {
                this.reset();
                return null;
            }
            byte = byte === 0xdc ? 0xc0 : 0xdb;
            this.escape = false;
        } else if (byte === 0xdb) {
            this.escape = true;
            return null;
        }
        if (this.length === this.bytes.length) {
            this.reset();
            return null;
        }
        this.bytes[this.length++] = byte;
        return null;
    }
}

function aborted(signal) {
    if (signal?.aborted) {
        throw signal.reason ?? new DOMException('Cancelled', 'AbortError');
    }
}

function delay(ms, signal) {
    aborted(signal);
    return new Promise((resolve, reject) => {
        const finish = () => {
            signal?.removeEventListener('abort', cancel);
            resolve();
        };
        const timer = setTimeout(finish, ms);
        const cancel = () => {
            clearTimeout(timer);
            signal.removeEventListener('abort', cancel);
            reject(signal.reason);
        };
        signal?.addEventListener('abort', cancel, { once: true });
    });
}
export class CskBootloader {
    constructor(onClosed = () => {}) {
        this.onClosed = onClosed;
        this.decoder = new SlipDecoder();
    }

    async connect(port, helper, { signal, progress = () => {} } = {}) {
        if (this.port || this.busy) {
            throw new Error('Bootloader connection busy');
        }
        this.busy = true;
        this.closePromise = null;
        this.closing = false;
        this.ready = false;
        try {
            if (
                !(helper instanceof Uint8Array) ||
                helper.length !== HELPER_SIZE ||
                (await sha256(helper)) !== HELPER_SHA256
            ) {
                throw new Error(
                    'CSK6 helper asset is corrupt or does not match its pinned version'
                );
            }
            aborted(signal);
            if (this.closing) {
                throw new Error('Bootloader connection closed');
            }
            this.port = port;
            this.opening = port.open({
                baudRate: 115200,
                dataBits: 8,
                stopBits: 1,
                parity: 'none',
                flowControl: 'none',
                bufferSize: 8192
            });
            await this.opening;
            this.opening = null;
            aborted(signal);
            if (this.closing) {
                throw new Error('Bootloader connection closed');
            }
            this.reader = port.readable.getReader();
            this.writer = port.writable.getWriter();
            this.decoder.reset();
            this.loop = this.#readLoop();
            await this.#sync(signal);
            await this.#command(5, words(helper.length, Math.ceil(helper.length / 2048), 2048, 0), {
                signal
            });
            for (let offset = 0; offset < helper.length; offset += 2048) {
                const data = helper.slice(offset, offset + 2048);
                const payload = new Uint8Array(16 + data.length);
                payload.set(words(data.length, offset / 2048, 0, 0));
                payload.set(data, 16);
                const checksum = data.reduce((value, byte) => value ^ byte, 0xef);
                await this.#command(7, payload, { checksum, timeout: 500, signal });
                progress({
                    stage: 'helper',
                    completed: offset + data.length,
                    total: helper.length
                });
            }
            await this.#command(6, words(0, 0), { signal });
            await delay(500, signal);
            await this.#sync(signal);
            await this.#command(0x0f, words(115200, 115200), { timeout: 1000, signal });
            await delay(200, signal);
            await this.#sync(signal);
            const flash = await this.#command(0xf3, new Uint8Array(), { signal });
            const capacity = (flash.value >>> 16) & 0xff;
            const jedec = flash.value & 0xffffff;
            if (!jedec || jedec === 0xffffff || capacity < 1 || capacity > 31) {
                throw new Error('Flash is absent or its capacity is invalid');
            }
            const size = 2 ** capacity;
            if (size !== FLASH_SIZE) {
                throw new Error(
                    `Unsupported C62 flash capacity: ${size} bytes (expected ${FLASH_SIZE}); no truncated backup will be produced`
                );
            }
            let chipId = null;
            try {
                const first = await this.#command(0xf4, new Uint8Array(), { signal });
                const second = await this.#command(0xf4, new Uint8Array(), { signal });
                if (
                    first.data.length === 8 &&
                    second.data.length === 8 &&
                    first.data.some(byte => byte !== 0) &&
                    first.data.some(byte => byte !== 0xff) &&
                    first.data.every((byte, index) => byte === second.data[index])
                ) {
                    chipId = Array.from(first.data, byte =>
                        byte.toString(16).padStart(2, '0')
                    ).join('');
                }
            } catch (error) {
                if (!(error instanceof BootloaderStatusError)) {
                    throw error;
                }
            }
            aborted(signal);
            this.info = Object.freeze({ target: 'c62', flashSize: size, flashId: jedec, chipId });
            this.ready = true;
            return this.info;
        } catch (error) {
            await this.close(error);
            throw error;
        } finally {
            this.busy = false;
        }
    }

    async #sync(signal) {
        const payload = new Uint8Array(36).fill(0x55);
        payload.set([7, 7, 0x12, 0x20]);
        const end = performance.now() + 2000;
        do {
            try {
                await this.#command(SYNC, payload, { timeout: 100, signal });
                return;
            } catch (error) {
                if (!error.syncTimeout) {
                    throw error;
                }
            }
        } while (performance.now() < end);
        throw new Error(
            'No CSK6 bootloader response. Check the cable and enter bootloader mode again.'
        );
    }

    #command(op, payload, { checksum = 0, timeout = 200, signal } = {}) {
        aborted(signal);
        if (!this.writer || this.closing || this.pending) {
            return Promise.reject(new Error('Bootloader connection busy or closed'));
        }
        const raw = new Uint8Array(8 + payload.length);
        const view = new DataView(raw.buffer);
        raw[1] = op;
        view.setUint16(2, payload.length, true);
        view.setUint32(4, checksum, true);
        raw.set(payload, 8);
        this.decoder.reset();
        return new Promise((resolve, reject) => {
            const finish = (error, reply) => {
                if (this.pending?.finish !== finish) {
                    return;
                }
                clearTimeout(timer);
                signal?.removeEventListener('abort', cancel);
                this.pending = null;
                if (error) {
                    reject(error);
                } else {
                    resolve(reply);
                }
            };
            const cancel = () => {
                finish(signal.reason ?? new Error('Cancelled'));
                void this.close();
            };
            const timer = setTimeout(() => {
                const error = new Error('Bootloader response timed out; reconnect before retrying');
                const syncRetry = op === SYNC && this.pending?.writeDone;
                if (syncRetry) {
                    error.syncTimeout = true;
                }
                finish(error);
                if (!syncRetry) {
                    void this.close(error);
                }
            }, timeout);
            const pending = {
                op,
                finish,
                writeDone: false,
                received: false,
                receive: (error, reply) => {
                    if (pending.received) {
                        return;
                    }
                    pending.received = true;
                    pending.error = error;
                    pending.reply = reply;
                    if (pending.writeDone) {
                        finish(error, reply);
                    }
                }
            };
            this.pending = pending;
            signal?.addEventListener('abort', cancel, { once: true });
            this.writer
                .write(slipEncode(raw))
                .then(() => {
                    pending.writeDone = true;
                    if (pending.received) {
                        finish(pending.error, pending.reply);
                    }
                })
                .catch(error => {
                    finish(error);
                    void this.close(error);
                });
        });
    }

    async #readLoop() {
        try {
            while (!this.closing) {
                const { value, done } = await this.reader.read();
                if (done) {
                    break;
                }
                for (const byte of value) {
                    const frame = this.decoder.feed(byte);
                    const pending = this.pending;
                    if (
                        !frame ||
                        !pending ||
                        frame.length < 8 ||
                        frame[0] !== 1 ||
                        frame[1] !== pending.op
                    ) {
                        continue;
                    }
                    const view = new DataView(frame.buffer);
                    if (view.getUint16(2, true) !== frame.length - 8) {
                        throw new Error('Malformed bootloader response length');
                    }
                    const payload = frame.slice(8);
                    if (pending.op !== SYNC && payload.length < 2) {
                        throw new Error('Truncated bootloader status');
                    }
                    if (payload.length >= 2 && payload[0]) {
                        pending.receive(
                            new BootloaderStatusError(
                                `Bootloader command 0x${pending.op.toString(16)} failed (code 0x${payload[1].toString(16)})`
                            )
                        );
                    } else {
                        pending.receive(null, {
                            value: view.getUint32(4, true),
                            data: pending.op === SYNC ? payload : payload.slice(2)
                        });
                    }
                }
            }
            if (!this.closing) {
                throw new Error('Bootloader serial connection ended');
            }
        } catch (error) {
            if (!this.closing) {
                this.pending?.finish(error);
                this.ready = false;
                queueMicrotask(() => {
                    void this.close(error);
                });
            }
        }
    }

    async readRange(offset, size, { signal, progress = () => {} } = {}) {
        if (
            !this.ready ||
            this.busy ||
            !Number.isSafeInteger(offset) ||
            !Number.isSafeInteger(size) ||
            offset < 0 ||
            size <= 0 ||
            offset + size > this.info.flashSize
        ) {
            throw new Error('Read exceeds detected flash bounds, or bootloader is busy/closed');
        }
        this.busy = true;
        try {
            return await this.#readRange(offset, size, signal, progress);
        } catch (error) {
            await this.close(error);
            throw error;
        } finally {
            this.busy = false;
        }
    }

    async #readRange(offset, size, signal, progress) {
        const bytes = new Uint8Array(size);
        for (let read = 0; read < size; ) {
            const want = Math.min(64, size - read);
            const reply = await this.#command(0x0e, words(offset + read, want), {
                timeout: 1000,
                signal
            });
            if (!reply.data.length || reply.data.length > want) {
                throw new Error('Flash read returned an invalid byte count');
            }
            bytes.set(reply.data, read);
            read += reply.data.length;
            progress({ stage: 'read', completed: read, total: size });
        }
        progress({ stage: 'verify', completed: size, total: size });
        const digest = await this.#digest(offset, size, signal);
        const expected = md5(bytes);
        if (!digest.every((byte, index) => byte === expected[index])) {
            throw new Error('Readback integrity mismatch; no successful backup was produced');
        }
        aborted(signal);
        return bytes;
    }

    async #digest(offset, size, signal) {
        const reply = await this.#command(0x13, words(offset, size, 0, 0), {
            timeout: Math.ceil(size / 0x100000) * 1000,
            signal
        });
        if (reply.data.length !== 16) {
            throw new Error('Invalid flash digest response');
        }
        return reply.data;
    }

    async updateFirmware(text, options = {}) {
        return this.#write(text, false, options);
    }

    async restoreBackup(text, options = {}) {
        return this.#write(text, true, options);
    }

    async resetSettings(options = {}) {
        return this.#write(null, false, options, true);
    }

    async #freshDevice(signal, restoring) {
        const expected = this.info;
        if (!this.ready || expected?.target !== 'c62' || expected.flashSize !== FLASH_SIZE) {
            throw new Error('Bootloader is not ready for a C62 write');
        }
        const flash = await this.#command(0xf3, new Uint8Array(), { signal });
        if ((flash.value & 0xffffff) !== expected.flashId) {
            throw new Error('Connected flash identity changed; reconnect and review again');
        }
        if (restoring && !expected.chipId) {
            throw new Error('Full restore unavailable: bootloader UUID could not be verified');
        }
        if (expected.chipId) {
            for (let repeat = 0; repeat < 2; repeat++) {
                const reply = await this.#command(0xf4, new Uint8Array(), { signal });
                const identity = Array.from(reply.data, byte =>
                    byte.toString(16).padStart(2, '0')
                ).join('');
                if (reply.data.length !== 8 || identity !== expected.chipId) {
                    throw new Error('Connected radio UUID changed; reconnect and review again');
                }
            }
        }
    }

    async #write(
        text,
        restoring,
        {
            acknowledged = false,
            verifyAfterWrite = true,
            applicationOnly = false,
            signal,
            progress = () => {}
        },
        resetting = false
    ) {
        if (acknowledged !== true) {
            throw new Error('Explicit risk acknowledgement is required before flash erase/write');
        }
        if (resetting && (!verifyAfterWrite || applicationOnly)) {
            throw new Error('Settings reset requires verification and cannot select firmware');
        }
        if (typeof verifyAfterWrite !== 'boolean') {
            throw new Error('Post-flash readback choice must be boolean');
        }
        if (typeof applicationOnly !== 'boolean' || (restoring && applicationOnly)) {
            throw new Error(
                'Application-only choice must be boolean and cannot limit a full restore'
            );
        }
        if (!this.ready || this.busy) {
            throw new Error('Bootloader busy or closed');
        }
        this.busy = true;
        let changed = false;
        try {
            let images;
            let result;
            if (resetting) {
                const bytes = new Uint8Array(SETTINGS_SIZE).fill(0xff);
                images = [
                    {
                        offset: SETTINGS_OFFSET,
                        bytes,
                        sha256: await sha256(bytes),
                        role: 'settings'
                    }
                ];
                result = 'Settings reset';
            } else if (restoring) {
                const { parseFlashBackup } = await import('./flash-backup.mjs');
                const backup = await parseFlashBackup(text);
                if (
                    !backup.device.chip_id ||
                    !this.info?.chipId ||
                    backup.device.chip_id !== this.info.chipId ||
                    backup.device.flash_id !== this.info.flashId
                ) {
                    throw new Error(
                        'Full restore requires a complete backup from this same verified radio'
                    );
                }
                images = [
                    { offset: 0, bytes: backup.bytes, sha256: backup.sha256, role: 'full backup' }
                ];
                result = backup.sha256;
            } else {
                const bundle = await parseFirmwareBundle(text);
                images = bundle.images;
                result = bundle.release.identity;
            }
            await this.#freshDevice(signal, restoring);
            if (applicationOnly) {
                const dsp = images.find(image => image.role === 'dsp');
                progress({ stage: 'check-dsp', completed: 0, total: dsp.bytes.length });
                const digest = await this.#digest(dsp.offset, dsp.bytes.length, signal);
                const expected = md5(dsp.bytes);
                if (!digest.every((byte, index) => byte === expected[index])) {
                    throw new Error(
                        'Existing DSP does not match this bundle; choose Application + DSP'
                    );
                }
                images = images.filter(image => image.role === 'application');
            }
            for (const image of images) {
                aborted(signal);
                const eraseEnd =
                    image.offset + Math.ceil(image.bytes.length / ERASE_SIZE) * ERASE_SIZE;
                if (
                    image.offset % ERASE_SIZE ||
                    eraseEnd > (restoring ? FLASH_SIZE : image.offset + PARTITION_SIZE)
                ) {
                    throw new Error('Image erase extent exceeds its permitted destination');
                }
                changed = true;
                await this.#program(image, signal, progress);
                if (verifyAfterWrite) {
                    const readback = await this.#readRange(
                        image.offset,
                        image.bytes.length,
                        signal,
                        value => progress({ ...value, image: image.role })
                    );
                    if ((await sha256(readback)) !== image.sha256) {
                        throw new Error(
                            'Written image does not match its verified source: ' + image.role
                        );
                    }
                }
                aborted(signal);
            }
            return result;
        } catch (error) {
            const message =
                error instanceof Error
                    ? error.message
                    : typeof error === 'string'
                      ? error
                      : 'Bootloader operation failed or was cancelled';
            const failure = new Error(message, { cause: error });
            if (error instanceof Error) {
                failure.name = error.name;
            }
            failure.flashMayHaveChanged = changed;
            await this.close(failure);
            throw failure;
        } finally {
            this.busy = false;
        }
    }

    async #program(image, signal, progress) {
        const bytes = image.bytes;
        const blocks = Math.ceil(bytes.length / 4096);
        // CSK6 FLASH_BEGIN queues sector erasure for this image. Never issue
        // whole-chip erase, and never retry an ambiguous non-sequenced write.
        await this.#command(2, words(bytes.length, blocks, 4096, image.offset), { signal });
        for (let sequence = 0; sequence < blocks; sequence++) {
            const data = bytes.subarray(sequence * 4096, (sequence + 1) * 4096);
            const payload = new Uint8Array(16 + data.length);
            payload.set(words(data.length, sequence, 0, 0));
            payload.set(data, 16);
            await this.#command(3, payload, {
                checksum: data.reduce((value, byte) => value ^ byte, 0xef),
                timeout: 1000,
                signal
            });
            progress({
                stage: 'write',
                image: image.role,
                completed: Math.min(bytes.length, (sequence + 1) * 4096),
                total: bytes.length
            });
        }
        await this.#command(4, words(0xff), { timeout: 2000, signal });
    }

    close(error = null) {
        this.ready = false;
        this.closing = true;
        this.pending?.finish(error ?? new Error('Bootloader connection closed'));
        if (!this.closePromise) {
            this.closePromise = this.#closePort(error);
        }
        return this.closePromise;
    }

    async #closePort(error) {
        if (this.opening) {
            try {
                await this.opening;
            } catch {}
            this.opening = null;
        }
        if (this.reader) {
            try {
                await this.reader.cancel();
            } catch {}
            await this.loop;
            this.reader.releaseLock();
            this.reader = null;
        }
        if (this.writer) {
            try {
                await this.writer.abort();
            } catch {}
            this.writer.releaseLock();
            this.writer = null;
        }
        if (this.port) {
            try {
                await this.port.close();
            } catch {}
            this.port = null;
        }
        this.info = null;
        this.onClosed(error);
    }
}
