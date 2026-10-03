// SPDX-License-Identifier: GPL-3.0-or-later
import { C, Decoder, encode, helloPayload, helloReply } from './protocol.mjs';

export class CompanionConnection {
    constructor(release, onState = () => {}) {
        this.release = release;
        this.onState = onState;
        this.session = 0n;
        this.sequence = 0;
        this.pending = null;
        this.closing = false;
        this.closePromise = null;
    }

    async connect(port) {
        if (this.port) {
            throw new Error('Already connected');
        }
        this.port = port;
        this.closing = false;
        this.closePromise = null;
        this.sequence = 0;
        try {
            await port.open({
                baudRate: 115200,
                dataBits: 8,
                stopBits: 1,
                parity: 'none',
                flowControl: 'none',
                bufferSize: 1024
            });
            this.writer = port.writable.getWriter();
            this.reader = port.readable.getReader();
            const nonceBytes = crypto.getRandomValues(new Uint8Array(8));
            let nonce = new DataView(nonceBytes.buffer).getBigUint64(0, true);
            if (!nonce) {
                nonce = 1n;
            }
            this.loop = this.readLoop();
            const frame = await this.request(
                C.MSG_HELLO,
                helloPayload(this.release.identity, nonce)
            );
            if (frame.major !== C.MAJOR || frame.minor !== C.MINOR) {
                throw new Error(
                    `Radio protocol ${frame.major}.${frame.minor} requires a matching companion (this companion uses ${C.MAJOR}.${C.MINOR}).`
                );
            }
            if (frame.payload.length === 1) {
                throw new Error(`The radio rejected the handshake (status ${frame.payload[0]}).`);
            }
            const info = helloReply(frame);
            if (info.status === C.STATUS_MISMATCH || info.release !== this.release.identity) {
                const error = new Error(`This radio requires companion ${info.release}.`);
                error.requiredRelease = info.release;
                throw error;
            }
            if (
                info.status !== C.STATUS_OK ||
                frame.session !== nonce ||
                info.maxPayload !== C.MAX_PAYLOAD ||
                info.leaseMs !== C.LEASE_MS
            ) {
                throw new Error('The radio rejected this session or reported incompatible limits.');
            }
            this.session = nonce;
            this.info = info;
            this.onState({ connected: true, info });
            this.scheduleHeartbeat();
            return info;
        } catch (error) {
            await this.close();
            throw error;
        }
    }

    request(type, payload = new Uint8Array()) {
        if (!this.writer || this.pending) {
            return Promise.reject(new Error('Connection busy or closed'));
        }
        if (this.sequence >= 0xffffffff) {
            return Promise.reject(new Error('Reconnect to start a new request sequence'));
        }

        const request = ++this.sequence;
        const bytes = encode({
            type,
            flags: C.FLAG_REQUEST,
            request,
            session: this.session,
            payload
        });
        const promise = new Promise((resolve, reject) => {
            let attempts = 0;
            const send = () => {
                if (this.pending?.request !== request) {
                    return;
                }
                if (++attempts > 2) {
                    this.pending = null;
                    reject(new Error('Radio response timed out'));
                    return;
                }
                // Retry the identical frame once, within the radio's lease.
                this.pending.timeout = setTimeout(send, 250);
                this.writer.write(bytes).catch(error => {
                    if (this.pending?.request === request) {
                        clearTimeout(this.pending.timeout);
                        this.pending = null;
                        reject(error);
                    }
                });
            };
            this.pending = { request, type, resolve, reject, timeout: null };
            send();
        });
        if (this.pending) {
            this.pending.promise = promise;
        }
        return promise;
    }

    async readLoop() {
        const decoder = new Decoder();
        try {
            while (!this.closing) {
                const { value, done } = await this.reader.read();
                if (done) {
                    break;
                }
                for (const byte of value) {
                    const frame = decoder.feed(byte);
                    const pending = this.pending;
                    if (
                        !frame ||
                        !pending ||
                        frame.flags !== C.FLAG_RESPONSE ||
                        frame.request !== pending.request ||
                        frame.type !== pending.type
                    ) {
                        continue;
                    }
                    // HELLO may report another protocol's bounded rejection.
                    // Established sessions still require exact versions/nonce.
                    if (
                        pending.type !== C.MSG_HELLO &&
                        (frame.major !== C.MAJOR ||
                            frame.minor !== C.MINOR ||
                            frame.session !== this.session)
                    ) {
                        continue;
                    }
                    clearTimeout(pending.timeout);
                    this.pending = null;
                    pending.resolve(frame);
                }
            }
            if (!this.closing) {
                throw new Error('Serial connection ended');
            }
        } catch (error) {
            if (!this.closing) {
                if (this.pending) {
                    clearTimeout(this.pending.timeout);
                    this.pending.reject(error);
                    this.pending = null;
                }
                this.session = 0n;
                clearTimeout(this.heartbeat);
                // Finish this read loop before close() awaits it.
                queueMicrotask(async () => {
                    await this.close();
                    this.onState({ connected: false, error });
                });
            }
        }
    }

    scheduleHeartbeat() {
        this.heartbeat = setTimeout(async () => {
            if (this.closing || !this.session) {
                return;
            }
            try {
                if (!this.pending && !this.bulk) {
                    const reply = await this.request(C.MSG_PING);
                    if (reply.payload.length !== 1 || reply.payload[0] !== C.STATUS_OK) {
                        throw new Error('Radio session expired');
                    }
                }
                this.scheduleHeartbeat();
            } catch (error) {
                await this.close();
                this.onState({ connected: false, error });
            }
        }, 250);
    }

    async transfer(work, { background = false, input = false } = {}) {
        if (
            this.closing ||
            !this.session ||
            this.foregroundWaiting ||
            (this.bulk && (background || input || !(this.backgroundWork || this.inputWork)))
        ) {
            throw new Error('Connection busy or closed');
        }
        // Awaited work may outlive a disconnect. Never continue it in a new session.
        const session = this.session;
        if (!background && !input) {
            this.cancelKeys?.();
        }
        const active = this.backgroundWork || this.inputWork;
        if (active) {
            // A CPS action cancels front keys and reserves the next slot before
            // awaiting the current key batch or snapshot. Further background polls cannot overtake it.
            this.foregroundWaiting = true;
            try {
                await active.catch(() => {});
            } finally {
                this.foregroundWaiting = false;
            }
            if (this.closing || this.session !== session) {
                throw new Error('Connection closed');
            }
        }

        this.bulk = true;
        let complete;
        if (background || input) {
            const promise = new Promise(resolve => {
                complete = resolve;
            });
            if (background) {
                this.backgroundWork = promise;
            } else {
                this.inputWork = promise;
            }
        }

        try {
            // An idle heartbeat may already be in flight when a user clicks.
            if (this.pending) {
                await this.pending.promise;
            }
            if (this.closing || this.session !== session) {
                throw new Error('Connection closed');
            }
            if (!background && !input) {
                await this.clearKeys?.();
            }
            return await work();
        } finally {
            this.bulk = false;
            if (background) {
                this.backgroundWork = null;
                complete();
            } else if (input) {
                this.inputWork = null;
                complete();
            }
        }
    }

    close() {
        if (!this.closePromise) {
            this.closePromise = this.closePort();
        }
        return this.closePromise;
    }

    async closePort() {
        this.closing = true;
        clearTimeout(this.heartbeat);
        if (this.pending) {
            clearTimeout(this.pending.timeout);
            this.pending.reject(new Error('Connection closed'));
            this.pending = null;
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
        this.session = 0n;
    }
}
