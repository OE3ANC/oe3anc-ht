// SPDX-License-Identifier: GPL-3.0-or-later
import { KissDecoder, kissFrame } from './kiss.mjs';

export class KissConnection {
    constructor(received, changed = () => {}) {
        this.received = received;
        this.changed = changed;
    }

    async connect(port, transport = null) {
        if (this.port) {
            throw new Error('KISS already connected');
        }
        this.port = port;
        this.closing = false;
        this.ready = false;
        this.closePromise = null;
        try {
            if (transport) {
                this.reader = transport.reader;
                this.writer = transport.writer;
            } else {
                await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1,
                    parity: 'none', flowControl: 'none', bufferSize: 1024 });
                this.reader = port.readable.getReader();
                this.writer = port.writable.getWriter();
            }
            this.loop = this.readLoop();
            await new Promise(resolve => setTimeout(resolve, 50));
            if (this.closing) {
                throw new Error('KISS connection ended');
            }
            this.ready = true;
            this.changed();
        } catch (error) {
            await this.close();
            throw error;
        }
    }

    async readLoop() {
        const decoder = new KissDecoder();
        try {
            while (!this.closing) {
                const { value, done } = await this.reader.read();
                if (done) {
                    throw new Error('KISS serial connection ended');
                }
                for (const byte of value) {
                    const frame = decoder.feed(byte);
                    if (frame && this.ready) {
                        this.received(frame);
                    }
                }
            }
        } catch (error) {
            if (!this.closing) {
                this.ready = false;
                queueMicrotask(async () => {
                    await this.close();
                    this.changed(error);
                });
            }
        }
    }

    async send(payload, command = 0) {
        if (!this.ready || this.busy || this.closing) {
            throw new Error('KISS connection busy or closed');
        }
        const bytes = kissFrame(payload, command);
        this.busy = true;
        this.changed();
        try {
            await this.writer.write(bytes);
        } catch (error) {
            await this.close();
            throw error;
        } finally {
            this.busy = false;
            this.changed();
        }
    }

    async exit() {
        try {
            await this.send(new Uint8Array(), 0xff);
        } finally {
            await this.close();
        }
    }

    close() {
        this.closePromise ??= this.closePort();
        return this.closePromise;
    }

    async closePort() {
        this.closing = true;
        this.ready = false;
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
        this.changed();
    }
}
