// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
import { KissConnection } from './kiss-serial.mjs';
import { ax25, position, message, describePacket } from './kiss.mjs';
import { ReceivedMap } from './received-map.mjs';

export class ModemUi {
    constructor(root, companion, { available, changed }) {
        this.root = root;
        this.companion = companion;
        this.available = available;
        this.changed = changed;
        this.status = root.querySelector('[role="status"]');
        this.counts = { Received: 0, Sent: 0 };
        this.receivedMap = new ReceivedMap(root);
        this.kiss = new KissConnection(frame => this.packet('Received', frame), error => {
            if (error) {
                this.status.textContent = error.message;
            }
            this.update();
            this.changed();
        });
        for (const action of ['connect', 'enter', 'exit', 'close', 'settings', 'position', 'message',
            'preview-position', 'preview-message']) {
            root.querySelector(`#modem-${action}`).addEventListener('click', () => this.run(action));
        }
        for (const name of ['source', 'destination', 'path', 'latitude', 'longitude', 'table',
            'symbol', 'comment', 'addressee', 'text']) {
            root.querySelector(`#modem-${name}`).addEventListener('input', () => {
                for (const kind of ['position', 'message']) {
                    const preview = root.querySelector(`#modem-${kind}-preview`);
                    if (preview.textContent) {
                        preview.textContent = 'Draft changed — preview again.';
                    }
                }
            });
        }
        root.querySelector('#modem-clear').addEventListener('click', () => {
            for (const kind of ['Received', 'Sent']) {
                this.root.querySelector(`#modem-${kind.toLowerCase()}-log`).textContent = '';
                this.counts[kind] = 0;
            }
            this.receivedMap.clear();
            this.update();
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
        for (const kind of ['Received', 'Sent']) {
            this.root.querySelector(`#modem-${kind.toLowerCase()}-count`).textContent =
                String(this.counts[kind]);
        }
        this.root.querySelector('#modem-port').textContent = this.kiss.port
            ? 'Serial port open · radio KISS mode assumed, not monitored'
            : 'Serial port closed · radio mode unknown';
        this.root.querySelector('#modem-close').disabled = disabled || !this.kiss.port;
    }

    value(name) {
        return this.root.querySelector(`#modem-${name}`).value;
    }

    compose(action) {
        const payload = action === 'position' ? position({
            latitude: this.value('latitude'), longitude: this.value('longitude'),
            table: this.value('table'), symbol: this.value('symbol'), comment: this.value('comment')
        }) : message({ addressee: this.value('addressee'), text: this.value('text') });
        return ax25({ source: this.value('source'), destination: this.value('destination'),
            path: this.value('path'), payload });
    }

    preview(action, bytes) {
        this.root.querySelector(`#modem-${action}-preview`).textContent =
            describePacket({ command: 0, payload: bytes });
    }

    packet(kind, frame) {
        const row = document.createElement('pre');
        row.textContent = `${kind === 'Sent' ? 'Sent to modem' : 'Received'} · ${describePacket(frame)}`;
        const station = kind === 'Received' ? this.receivedMap.receive(frame) : null;
        if (station) {
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'secondary packet-map';
            button.textContent = 'Map';
            button.setAttribute('aria-label', `Map received position from ${station.source}`);
            button.addEventListener('click', () => this.receivedMap.focus(station));
            row.append(button);
        }
        const log = this.root.querySelector(`#modem-${kind.toLowerCase()}-log`);
        log.prepend(row);
        while (log.children.length > 100) {
            log.lastChild.remove();
        }
        ++this.counts[kind];
        this.update();
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
            if (action.startsWith('preview-')) {
                const kind = action.slice(8);
                this.root.querySelector(`#modem-${kind}-preview`).textContent = '';
                this.preview(kind, this.compose(kind));
                this.status.textContent = 'Preview only. Review the bytes, then explicitly send once.';
            } else if (action === 'connect') {
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
                const bytes = this.compose(action);
                this.preview(action, bytes);
                await this.kiss.send(bytes);
                this.packet('Sent', { command: 0, payload: bytes });
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
