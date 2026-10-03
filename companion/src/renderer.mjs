// SPDX-License-Identifier: GPL-3.0-or-later
import { C } from './protocol.mjs';
export class RadioRenderer {
    constructor(canvas) {
        this.canvas = canvas;
        this.context = canvas.getContext('2d');
        this.image = this.context.createImageData(160, 128);
    }

    async start() {
        const { default: createRenderer } = await import('../render/ht-ui.mjs');
        this.module = await createRenderer({
            locateFile: name => new URL('../render/' + name, import.meta.url).href
        });
        if (this.module._ht_ui_start()) {
            throw new Error('LVGL display could not start');
        }
    }

    apply(bytes) {
        if (!this.module || bytes.length > C.UI_MAX_BYTES) {
            throw new Error('Invalid UI snapshot');
        }
        this.module.HEAPU8.set(bytes, this.module._ht_ui_buffer());
        if (this.module._ht_ui_apply(bytes.length)) {
            throw new Error('LVGL rejected the UI snapshot');
        }
        this.draw();
    }

    tick(delta) {
        this.module._ht_ui_tick(Math.min(100, Math.max(0, Math.floor(delta))));
        this.draw();
    }

    draw() {
        const pixels = this.module.HEAPU16.subarray(
            this.module._ht_ui_pixels() / 2,
            this.module._ht_ui_pixels() / 2 + 160 * 128
        );
        const rgba = this.image.data;
        // LVGL exposes RGB565; the canvas needs full-range RGB channels and opaque alpha.
        for (let i = 0; i < pixels.length; i++) {
            const p = pixels[i];
            const offset = i * 4;
            rgba[offset] = Math.floor((((p >> 11) & 31) * 255) / 31);
            rgba[offset + 1] = Math.floor((((p >> 5) & 63) * 255) / 63);
            rgba[offset + 2] = Math.floor(((p & 31) * 255) / 31);
            rgba[offset + 3] = 255;
        }
        this.context.putImageData(this.image, 0, 0);
    }
}
