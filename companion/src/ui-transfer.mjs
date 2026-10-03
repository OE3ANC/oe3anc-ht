// SPDX-License-Identifier: GPL-3.0-or-later
import { C, crc32 } from './protocol.mjs';
import { decodePresentation } from './presentation-wire.mjs';

function reply(frame) {
    const p = frame.payload;
    if (!p.length) {
        throw new Error('Malformed UI reply');
    }
    if (p[0] !== C.STATUS_OK) {
        const e = new Error(`UI snapshot unavailable (status ${p[0]})`);
        e.status = p[0];
        throw e;
    }
    return p;
}
export class UiTransfer {
    constructor(connection) {
        this.connection = connection;
        this.revision = 0;
    }

    reset() {
        this.revision = 0;
    }

    async poll() {
        const c = this.connection;
        const session = c.session;
        if (!(c.info?.capabilities & C.CAP_UI_SNAPSHOT)) {
            throw new Error('This radio does not provide UI snapshots');
        }
        return c.transfer(
            async () => {
                const started = performance.now();
                const query = new Uint8Array(4);
                new DataView(query.buffer).setUint32(0, this.revision, true);
                const meta = reply(await c.request(C.MSG_UI_POLL, query));
                const view = new DataView(meta.buffer, meta.byteOffset, meta.byteLength);
                if (meta.length < 6 || meta[1] > 1) {
                    throw new Error('Malformed UI metadata');
                }
                if (c.session !== session || performance.now() - started >= C.UI_STALE_MS) {
                    throw new Error('UI transfer became stale');
                }
                const revision = view.getUint32(2, true);
                if (!revision) {
                    throw new Error('Invalid UI revision');
                }
                if (!meta[1]) {
                    if (meta.length !== 6 || revision !== this.revision || !this.revision) {
                        throw new Error('Invalid unchanged UI reply');
                    }
                    return { revision, changed: false };
                }
                if (meta.length !== 16) {
                    throw new Error('Malformed UI metadata');
                }
                const token = view.getUint32(6, true);
                const length = view.getUint16(10, true);
                if (!token || !length || length > C.UI_MAX_BYTES) {
                    throw new Error('Invalid UI transfer');
                }

                // Every chunk belongs to the pinned snapshot token, even as the radio UI changes.
                const bytes = new Uint8Array(length);
                for (let offset = 0; offset < length; ) {
                    const count = Math.min(C.UI_CHUNK_BYTES, length - offset);
                    const request = new Uint8Array(7);
                    const q = new DataView(request.buffer);
                    q.setUint32(0, token, true);
                    q.setUint16(4, offset, true);
                    request[6] = count;
                    const chunk = reply(await c.request(C.MSG_UI_CHUNK, request));
                    const r = new DataView(chunk.buffer, chunk.byteOffset, chunk.byteLength);
                    if (
                        chunk.length !== 7 + count ||
                        r.getUint32(1, true) !== token ||
                        r.getUint16(5, true) !== offset
                    ) {
                        throw new Error('UI chunk mismatch');
                    }
                    bytes.set(chunk.subarray(7), offset);
                    offset += count;
                }

                if (c.session !== session || performance.now() - started >= C.UI_STALE_MS) {
                    throw new Error('UI transfer became stale');
                }
                if (crc32(bytes) !== view.getUint32(12, true)) {
                    throw new Error('UI snapshot CRC mismatch');
                }
                const presentation = decodePresentation(bytes);
                // Advance only after a complete, verified snapshot; failed polls must retry it.
                this.revision = revision;
                return { revision, changed: true, bytes, presentation };
            },
            { background: true }
        );
    }
}
