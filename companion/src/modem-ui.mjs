// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
import { KissConnection } from './kiss-serial.mjs';
import { ax25, decodeAx25, position, message, hex, safeText } from './kiss.mjs';

export class ModemUi {
    constructor(root, companion, { available, changed }) {
        this.root = root;
        this.companion = companion;
        this.available = available;
        this.changed = changed;
        this.status = root.querySelector('[role="status"]');
        this.log = root.querySelector('#modem-log');
        this.kiss = new KissConnection(frame => this.packet('Received', frame), error => {
            if (error) {
                this.status.textContent = error.message;
            }
            this.update();
            this.changed();
        });
        for (const action of ['connect', 'enter', 'exit', 'close', 'settings', 'position', 'message']) {
            root.querySelector(`#modem-${action}`).addEventListener('click', () => this.run(action));
        }
        root.querySelector('#modem-clear').addEventListener('click', () => {
            this.log.textContent = '';
        });
        this.update();
    }

    get ownsPort() {
        return this.busy || !!this.kiss.port;
    }

    update() {
        const disabled = this.busy || this.kiss.busy;
        this.root.querySelector('#modem-connect').disabled = disabled || !this.available() ||
            !!this.kiss.port || !('serial' in navigator);
        this.root.querySelector('#modem-enter').disabled = disabled || !!this.kiss.port ||
            !this.companion.session || this.companion.bulk ||
            !(this.companion.info?.capabilities & C.CAP_KISS);
        for (const action of ['exit', 'settings', 'position', 'message']) {
            this.root.querySelector(`#modem-${action}`).disabled = disabled || !this.kiss.ready;
        }
        this.root.querySelector('#modem-close').disabled = disabled || !this.kiss.port;
    }

    value(name) {
        return this.root.querySelector(`#modem-${name}`).value;
    }

    packet(label, frame) {
        const row = document.createElement('pre');
        let decoded = '';
        if (frame.command === 0) {
            try {
                const item = decodeAx25(frame.payload);
                decoded = `${item.source} > ${item.destination}` +
                    (item.path.length ? ` via ${item.path.join(', ')}` : '') +
                    ` · control ${item.control.toString(16)} · PID ${item.pid?.toString(16) ?? '—'}` +
                    `\n${safeText(item.payload)}\n`;
            } catch (error) {
                decoded = `${error.message}\n`;
            }
        }
        row.textContent = `${label} · KISS ${frame.command.toString(16)}\n${decoded}${hex(frame.payload)}`;
        this.log.prepend(row);
        while (this.log.children.length > 100) {
            this.log.lastChild.remove();
        }
    }

    async run(action) {
        if (this.busy || this.kiss.busy) {
            return;
        }
        if ((action === 'connect' && (!this.available() || this.kiss.port)) ||
            (action === 'enter' && (!this.companion.session || this.companion.bulk || this.kiss.port))) {
            return;
        }
        this.busy = true;
        this.update();
        this.changed();
        try {
            if (action === 'connect') {
                await this.kiss.connect(await navigator.serial.requestPort());
                this.status.textContent = 'Port open. KISS mode assumed from your radio menu selection; no handshake is available.';
            } else if (action === 'enter') {
                const transport = await this.companion.enterKiss();
                await this.kiss.connect(transport.port, transport);
                this.status.textContent = 'Radio accepted KISS entry. Physical PTT or radio changes can exit it; reconnect explicitly afterward.';
            } else if (action === 'exit') {
                await this.kiss.exit();
                this.status.textContent = 'Return command sent and port closed. Use Connect radio to confirm companion mode.';
            } else if (action === 'close') {
                await this.kiss.close();
                this.status.textContent = 'Port closed. Radio mode is unchanged; use its Modem menu if needed.';
            } else if (action === 'settings') {
                const values = ['delay', 'persistence', 'slot', 'tail'].map(name => {
                    const value = Number(this.value(name));
                    if (!this.value(name).trim() || !Number.isInteger(value) || value < 0 || value > 255) {
                        throw new Error('KISS timing/persistence values must be integers from 0 to 255');
                    }
                    return value;
                });
                values.push(this.root.querySelector('#modem-duplex').checked ? 1 : 0);
                for (const [index, value] of values.entries()) {
                    await this.kiss.send(Uint8Array.of(value), index + 1);
                }
                this.status.textContent = 'Settings sent to modem (no acknowledgement).';
            } else {
                const payload = action === 'position' ? position({
                    latitude: this.value('latitude'), longitude: this.value('longitude'),
                    table: this.value('table'), symbol: this.value('symbol'), comment: this.value('comment')
                }) : message({ addressee: this.value('addressee'), text: this.value('text') });
                const bytes = ax25({ source: this.value('source'), destination: this.value('destination'),
                    path: this.value('path'), payload });
                await this.kiss.send(bytes);
                this.packet('Sent to modem', { command: 0, payload: bytes });
                this.status.textContent = 'Sent to modem. KISS does not confirm RF transmission or delivery.';
            }
        } catch (error) {
            this.status.textContent = error.message;
        } finally {
            this.busy = false;
            this.update();
            this.changed();
        }
    }
}
