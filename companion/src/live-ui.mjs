// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
import { UiTransfer } from './ui-transfer.mjs';
import { RadioRenderer } from './renderer.mjs';
import { VirtualKeypad } from './keys.mjs';
export class LiveUi {
    constructor(root, connection) {
        this.root = root;
        this.connection = connection;
        this.transfer = new UiTransfer(connection);
        this.status = root.querySelector('[role="status"]');
        this.canvas = root.querySelector('canvas');
        this.renderer = new RadioRenderer(this.canvas);
        this.session = 0n;
        this.generation = 0;
        this.pause = root.querySelector('button');
        this.paused = false;
        this.pause.addEventListener('click', () => {
            this.paused = !this.paused;
            this.pause.textContent = this.paused ? 'Resume live view' : 'Pause live view';
            this.update();
        });
        this.keypad = new VirtualKeypad(
            root.querySelector('#radio-keypad'),
            connection,
            () => this.active && performance.now() - this.freshAt < C.UI_STALE_MS,
            () => this.refresh()
        );
        this.update();
    }

    refresh() {
        if (!this.session || this.paused || this.polling === this.generation) {
            return;
        }
        clearTimeout(this.timer);
        this.timer = setTimeout(() => this.poll(this.generation), 0);
    }

    stale(message) {
        this.active = false;
        this.root.classList.add('stale');
        this.status.textContent = message;
    }

    update() {
        const c = this.connection;
        const available = !!c.session && !!(c.info?.capabilities & C.CAP_UI_SNAPSHOT);
        this.pause.disabled = !available;
        if (!available || this.paused) {
            clearTimeout(this.timer);
            this.generation++;
            this.session = 0n;
            this.transfer = new UiTransfer(c);
            this.stale(
                available
                    ? 'Live view paused.'
                    : 'Connect matching firmware for the live radio display.'
            );
            return;
        }
        if (this.session === c.session) {
            return;
        }
        this.transfer = new UiTransfer(c);
        this.session = c.session;
        const generation = ++this.generation;
        this.stale('Loading the shared radio display…');
        this.loading ??= this.renderer.start();
        this.loading
            .then(() => {
                if (generation === this.generation) {
                    this.poll(generation);
                }
            })
            .catch(error => {
                if (generation === this.generation) {
                    this.loading = null;
                    this.stale(error.message);
                }
            });
    }

    async poll(generation) {
        if (
            generation !== this.generation ||
            this.session !== this.connection.session ||
            this.polling === generation
        ) {
            return;
        }
        this.polling = generation;
        try {
            if (this.connection.bulk) {
                if (!this.connection.inputWork) {
                    this.stale('Live view paused during codeplug transfer.');
                }
            } else {
                const result = await this.transfer.poll();
                if (generation !== this.generation || this.session !== this.connection.session) {
                    return;
                }
                if (result.changed) {
                    this.renderer.apply(result.bytes);
                }
                this.root.classList.remove('stale');
                this.active = true;
                this.freshAt = performance.now();
                this.status.textContent = `Live radio display · revision ${result.revision}`;
                if (!this.animation) {
                    this.animate();
                }
            }
        } catch (error) {
            if (generation === this.generation) {
                this.transfer.reset();
                this.stale(error.message);
            }
        } finally {
            if (this.polling === generation) {
                this.polling = null;
            }
            if (generation === this.generation && this.session === this.connection.session) {
                this.timer = setTimeout(() => this.poll(generation), C.UI_POLL_MS);
            }
        }
    }

    animate(now = performance.now()) {
        if (this.active && now - this.freshAt >= C.UI_STALE_MS) {
            this.stale('Live display expired; waiting for a fresh snapshot.');
        }
        if (!this.active) {
            this.animation = null;
            this.previous = null;
            return;
        }
        if (this.previous !== null && this.previous !== undefined) {
            this.renderer.tick(now - this.previous);
        }
        this.previous = now;
        this.animation = requestAnimationFrame(t => this.animate(t));
    }
}
