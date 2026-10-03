// SPDX-License-Identifier: GPL-3.0-or-later
import { C, crc32 } from './protocol.mjs';
import { encodeCodeplug, decodeCodeplug } from './codeplug-wire.mjs';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const names = new Map([
    [C.STATUS_INVALID, 'invalid codeplug or transfer'],
    [C.STATUS_STALE, 'radio changed; read it again before retrying'],
    [C.STATUS_BUSY, 'radio busy; close local radio editors and wait for operations to finish'],
    [C.STATUS_UNSUPPORTED, 'unsupported radio feature'],
    [C.STATUS_PROTECTED, 'radio settings are protected'],
    [C.STATUS_CANCELLED, 'operation cancelled'],
    [C.STATUS_SESSION, 'radio session expired'],
    [C.STATUS_FAILED, 'radio operation failed']
]);
export function statusText(status) {
    return names.get(status) ?? `radio error ${status}`;
}

function values(...numbers) {
    const bytes = new Uint8Array(numbers.length * 4);
    const view = new DataView(bytes.buffer);
    numbers.forEach((number, index) => view.setUint32(index * 4, number, true));
    return bytes;
}

function u32(bytes, offset) {
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

function check(frame, length) {
    const bytes = frame.payload;
    if (!bytes.length) {
        throw new Error('Malformed radio reply');
    }
    if (bytes[0] !== C.STATUS_OK) {
        const error = new Error(statusText(bytes[0]));
        error.status = bytes[0];
        throw error;
    }
    if (bytes.length !== length) {
        throw new Error('Malformed radio reply');
    }
    return bytes;
}
export class CpsTransfer {
    constructor(connection, progress = () => {}) {
        this.connection = connection;
        this.progress = progress;
        this.baseline = null;
        this.busy = false;
    }

    invalidate() {
        this.baseline = null;
    }

    cancel() {
        this.cancelled = true;
    }

    async run(work) {
        if (this.busy) {
            throw new Error('Codeplug transfer already running');
        }
        if (!(this.connection.info?.capabilities & C.CAP_CPS)) {
            throw new Error('This radio does not support connected CPS');
        }
        this.busy = true;
        this.cancelled = false;
        this.activeToken = 0;
        this.committing = false;
        try {
            return await this.connection.transfer(async () => {
                try {
                    return await work();
                } catch (error) {
                    if (this.activeToken && this.connection.session) {
                        // Keep cleanup in the same serial slot as the failed upload.
                        // An accepted COMMIT responds BUSY and cannot be undone.
                        try {
                            await this.connection.request(
                                C.MSG_CPS_CANCEL,
                                values(this.activeToken)
                            );
                        } catch {}
                    }
                    throw error;
                }
            });
        } catch (error) {
            if (this.committing && error.status === undefined) {
                error.message += ' Commit may have completed; read the radio to check.';
            }
            throw error;
        } finally {
            this.busy = false;
            this.activeToken = 0;
        }
    }

    async cancellation(token) {
        if (!this.cancelled) {
            return;
        }
        check(await this.connection.request(C.MSG_CPS_CANCEL, values(token)), 1);
        throw new Error('Transfer cancelled; the radio codeplug was not replaced.');
    }

    async read() {
        this.invalidate();
        return this.run(async () => {
            const connection = this.connection;
            const session = connection.session;
            const metadata = check(await connection.request(C.MSG_CPS_READ), 31);
            const token = u32(metadata, 1);
            const length = u32(metadata, 17);
            if (
                !token ||
                !u32(metadata, 5) ||
                !length ||
                length > C.CPS_MAX_BYTES ||
                metadata[29] > 3 ||
                metadata[30] > C.STATUS_CANCELLED
            ) {
                throw new Error('Malformed codeplug metadata');
            }
            const bytes = new Uint8Array(length);
            this.activeToken = token;
            for (let offset = 0; offset < length; ) {
                await this.cancellation(token);
                const count = Math.min(C.CPS_CHUNK_BYTES, length - offset);
                const payload = new Uint8Array(10);
                payload.set(values(token, offset));
                new DataView(payload.buffer).setUint16(8, count, true);
                const reply = check(
                    await connection.request(C.MSG_CPS_READ_CHUNK, payload),
                    9 + count
                );
                if (u32(reply, 1) !== token || u32(reply, 5) !== offset) {
                    throw new Error('Codeplug chunk mismatch');
                }
                bytes.set(reply.subarray(9), offset);
                offset += count;
                this.progress({ operation: 'read', done: offset, total: length });
            }
            await this.cancellation(token);
            if (crc32(bytes) !== u32(metadata, 21)) {
                throw new Error('Complete codeplug CRC mismatch');
            }
            const document = decodeCodeplug(bytes);
            this.baseline = {
                token,
                session,
                revision: u32(metadata, 5),
                generation: u32(metadata, 25),
                pending: !!(metadata[29] & 1),
                protected: !!(metadata[29] & 2),
                saveError: metadata[30],
                document
            };
            return this.baseline;
        });
    }

    async write(document) {
        const baseline = this.baseline;
        const bytes = encodeCodeplug(document);
        if (!baseline || baseline.session !== this.connection.session) {
            throw new Error('Read the radio before writing. Your local draft is retained.');
        }
        if (baseline.protected) {
            throw new Error('Radio settings are protected. Your local draft is retained.');
        }
        this.invalidate(); // Each complete read authorizes only one write attempt.
        return this.run(async () => {
            const connection = this.connection;
            const reply = check(
                await connection.request(
                    C.MSG_CPS_WRITE,
                    values(baseline.token, bytes.length, crc32(bytes))
                ),
                5
            );
            const token = u32(reply, 1);
            if (!token) {
                throw new Error('Malformed write token');
            }
            this.activeToken = token;
            for (let offset = 0; offset < bytes.length; ) {
                await this.cancellation(token);
                const count = Math.min(C.CPS_CHUNK_BYTES, bytes.length - offset);
                const payload = new Uint8Array(8 + count);
                payload.set(values(token, offset));
                payload.set(bytes.subarray(offset, offset + count), 8);
                check(await connection.request(C.MSG_CPS_WRITE_CHUNK, payload), 1);
                offset += count;
                this.progress({ operation: 'write', done: offset, total: bytes.length });
            }
            await this.cancellation(token);
            this.committing = true;
            const accepted = check(await connection.request(C.MSG_CPS_COMMIT, values(token)), 5);
            if (u32(accepted, 1) !== token) {
                throw new Error('Write token mismatch');
            }
            // COMMIT acceptance is the cancellation boundary. Link loss afterward
            // can leave a completed write; never tell the user it was rolled back.
            const deadline = performance.now() + 30000;
            while (true) {
                const status = check(await connection.request(C.MSG_CPS_STATUS, values(token)), 16);
                const state = status[5];
                const result = {
                    state,
                    revision: u32(status, 6),
                    generation: u32(status, 10),
                    error: status[14],
                    saveError: status[15]
                };
                if (
                    u32(status, 1) !== token ||
                    state < C.CPS_STATE_ACCEPTED ||
                    state > C.CPS_STATE_FAILED ||
                    result.error > C.STATUS_CANCELLED ||
                    result.saveError > C.STATUS_CANCELLED
                ) {
                    throw new Error('Malformed write status');
                }
                if (
                    (state === C.CPS_STATE_ACCEPTED &&
                        (result.revision || result.error || result.saveError)) ||
                    (state === C.CPS_STATE_FAILED && (!result.error || result.revision)) ||
                    ([C.CPS_STATE_APPLIED, C.CPS_STATE_DURABLE].includes(state) &&
                        (!result.revision || result.error)) ||
                    (state === C.CPS_STATE_DURABLE && result.saveError)
                ) {
                    throw new Error('Inconsistent write status');
                }
                this.progress({ operation: 'save', result });
                if (
                    state === C.CPS_STATE_DURABLE ||
                    state === C.CPS_STATE_FAILED ||
                    performance.now() >= deadline ||
                    this.cancelled
                ) {
                    return result;
                }
                await sleep(50);
            }
        });
    }
}
