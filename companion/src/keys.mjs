// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
export class RemoteKeys {
    constructor(connection, available, changed = () => {}) {
        this.connection = connection;
        this.available = available;
        this.changed = changed;
        this.held = 0;
        this.queue = [];
        // Renew only acknowledged presses; browser events can still be queued.
        this.acceptedHeld = 0;
        this.session = 0n;
    }

    set(key, pressed) {
        if (!Number.isInteger(key) || key < 0 || key > C.UI_KEY_DIGIT_9) {
            return;
        }
        const bit = 1 << key;
        if (pressed && (!this.available() || this.held & bit)) {
            return;
        }
        if (!pressed && !(this.held & bit)) {
            return;
        }
        if (this.queue.length >= 16) {
            this.cancel();
            return;
        }
        this.held = pressed ? this.held | bit : this.held & ~bit;
        this.queue.push({ type: C.MSG_UI_KEY, payload: Uint8Array.of(key, +pressed) });
        this.changed();
        this.run();
        this.renew();
    }

    async cleanup() {
        const c = this.connection;
        const reply = await c.request(C.MSG_UI_KEYS_CLEAR, new Uint8Array());
        if (reply.payload.length !== 1 || reply.payload[0] !== C.STATUS_OK) {
            throw new Error('Radio keypad cleanup failed');
        }
        this.acceptedHeld = 0;
    }

    cancel(send = true) {
        clearTimeout(this.timer);
        this.held = 0;
        this.queue = [];
        // CLEAR cancels accepted but unapplied remote keys too. It is sent when
        // the request slot becomes free; the independent radio lease bounds loss.
        if (this.connection.session) {
            this.clear = true;
        }
        this.changed();
        if (send) {
            this.run();
        }
    }

    async clearWithinSlot() {
        if (!this.clear) {
            return;
        }
        this.clear = false;
        await this.cleanup();
    }

    renew() {
        clearTimeout(this.timer);
        if (!this.held) {
            return;
        }
        this.timer = setTimeout(() => {
            if (!this.available()) {
                this.cancel();
                return;
            }
            // One queued KEEP is enough; use the current mask when it is sent.
            if (this.held && !this.queue.some(q => q.type === C.MSG_UI_KEYS_KEEP)) {
                this.queue.push({ type: C.MSG_UI_KEYS_KEEP });
            }
            this.run();
            this.renew();
        }, C.UI_KEYS_KEEP_MS);
    }

    async run() {
        if (this.running || (!this.queue.length && !this.clear)) {
            return;
        }
        const c = this.connection;
        const session = c.session;
        if (!session || c.closing) {
            this.queue = [];
            this.clear = false;
            return;
        }
        // CPS owns its complete transfer. Do not inject a key after it settles.
        if (c.bulk && !c.backgroundWork && !c.inputWork) {
            this.queue = [];
            return;
        }

        if (session !== this.session) {
            this.acceptedHeld = 0;
            this.session = session;
        }

        this.running = true;
        let failed;
        try {
            await c.transfer(
                async () => {
                    while (session === c.session && (this.clear || this.queue.length)) {
                        if (!this.clear && !this.available()) {
                            this.cancel(false);
                        }
                        if (this.clear) {
                            this.clear = false;
                            await this.cleanup();
                            continue;
                        }
                        const command = this.queue.shift();
                        if (command.type === C.MSG_UI_KEYS_KEEP) {
                            if (!this.acceptedHeld) {
                                continue;
                            }
                            command.payload = new Uint8Array(4);
                            new DataView(command.payload.buffer).setUint32(
                                0,
                                this.acceptedHeld,
                                true
                            );
                        }
                        const reply = await c.request(command.type, command.payload);
                        if (reply.payload.length !== 1 || reply.payload[0] !== C.STATUS_OK) {
                            throw new Error(
                                `Radio keypad rejected input (status ${reply.payload[0]})`
                            );
                        }
                        if (command.type === C.MSG_UI_KEYS_CLEAR) {
                            this.acceptedHeld = 0;
                        } else if (command.type === C.MSG_UI_KEY) {
                            const bit = 1 << command.payload[0];
                            this.acceptedHeld = command.payload[1]
                                ? this.acceptedHeld | bit
                                : this.acceptedHeld & ~bit;
                        }
                    }
                },
                { input: true }
            );
        } catch (error) {
            failed = error;
            clearTimeout(this.timer);
            this.held = 0;
            this.queue = [];
            this.clear = true;
        } finally {
            this.running = false;
            this.changed(failed);
        }
    }
}
const labels = ['Up', 'Down', 'P1', 'P2', 'OK', 'Back', '★', '#', ...'0123456789'];
export class VirtualKeypad {
    constructor(root, connection, fresh, refresh) {
        this.root = root;
        this.connection = connection;
        this.fresh = fresh;
        this.refresh = refresh;
        this.status = root.querySelector('[role="status"]');
        this.buttons = [...root.querySelectorAll('[data-key]')];
        this.keys = new RemoteKeys(
            connection,
            () => this.available(),
            error => {
                this.update();
                if (error) {
                    this.status.textContent = error.message;
                }
                this.refresh();
            }
        );
        this.pointers = new Map();
        this.keyboard = new Map();
        connection.cancelKeys = () => this.cancel(false);
        connection.clearKeys = () => this.keys.clearWithinSlot();
        for (const button of this.buttons) {
            const key = Number(button.dataset.key);
            button.setAttribute('aria-label', labels[key]);
            button.addEventListener('pointerdown', event => {
                if (event.button !== 0 || !this.available()) {
                    return;
                }
                event.preventDefault();
                button.focus();
                button.setPointerCapture(event.pointerId);
                this.pointers.set(event.pointerId, key);
                this.keys.set(key, true);
            });
            button.addEventListener('pointerup', event => this.releasePointer(event));
            button.addEventListener('pointercancel', () => this.cancel());
            button.addEventListener('lostpointercapture', event => this.releasePointer(event));
            // Assistive activation produces a click without pointerdown/up.
            button.addEventListener('click', event => {
                if (event.detail === 0 && !this.keyboard.size) {
                    this.keys.set(key, true);
                    this.keys.set(key, false);
                }
            });
        }
        root.addEventListener('keydown', event => {
            if (event.repeat) {
                event.preventDefault();
                return;
            }
            if (event.ctrlKey || event.altKey || event.metaKey) {
                return;
            }
            const key = this.shortcut(event.key);
            if (key === undefined || !this.available()) {
                return;
            }
            event.preventDefault();
            this.keyboard.set(event.code, key);
            this.keys.set(key, true);
        });
        root.addEventListener('keyup', event => {
            const key = this.keyboard.get(event.code);
            if (key === undefined) {
                return;
            }
            event.preventDefault();
            this.keyboard.delete(event.code);
            this.release(key);
        });
        root.addEventListener('focusout', event => {
            if (!root.contains(event.relatedTarget)) {
                this.cancel();
            }
        });
        window.addEventListener('blur', () => this.cancel());
        document.addEventListener('visibilitychange', () => {
            if (document.hidden) {
                this.cancel();
            }
        });
        this.watch = setInterval(() => {
            this.update();
            if (this.available() && this.keys.clear && !this.keys.running) {
                this.keys.run();
            }
        }, 100);
        this.update();
    }

    shortcut(key) {
        const values = {
            ArrowUp: 0,
            ArrowDown: 1,
            ArrowLeft: 2,
            ArrowRight: 3,
            Enter: 4,
            Escape: 5,
            Backspace: 5,
            '*': 6,
            '#': 7
        };
        if (/^[0-9]$/.test(key)) {
            return C.UI_KEY_DIGIT_0 + Number(key);
        }
        return values[key];
    }

    available() {
        const c = this.connection;
        return (
            !c.foregroundWaiting &&
            !!c.session &&
            !!(c.info?.capabilities & C.CAP_UI_KEYS) &&
            this.fresh() &&
            !document.hidden &&
            document.hasFocus() &&
            (!c.bulk || !!c.backgroundWork || !!c.inputWork)
        );
    }

    releasePointer(event) {
        const key = this.pointers.get(event.pointerId);
        if (key === undefined) {
            return;
        }
        this.pointers.delete(event.pointerId);
        this.release(key);
    }

    release(key) {
        if (![...this.pointers.values(), ...this.keyboard.values()].includes(key)) {
            this.keys.set(key, false);
        }
    }

    cancel(send = true) {
        this.pointers.clear();
        this.keyboard.clear();
        this.keys.cancel(send);
    }

    update() {
        const available = this.available();
        const lost = this.wasAvailable && !available;
        this.wasAvailable = available;
        if (lost) {
            this.cancel();
        }
        for (const button of this.buttons) {
            button.disabled = !available;
            button.setAttribute(
                'aria-pressed',
                String(!!(this.keys.held & (1 << Number(button.dataset.key))))
            );
        }
        this.status.textContent = available
            ? 'Radio keypad ready. Focus here for arrow keys, Enter, Escape, digits, * and #. Hold ★ for keypad lock. Keys do not auto-repeat.'
            : 'Keypad needs a fresh live display, an active tab and a matching session. The unplug/Done confirmation is local only.';
    }
}
