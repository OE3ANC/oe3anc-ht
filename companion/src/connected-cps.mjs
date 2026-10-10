// SPDX-License-Identifier: GPL-3.0-or-later
import { CpsTransfer, statusText } from './cps-transfer.mjs';
import { C } from './protocol.mjs';
import { canonical, validate, targetErrors } from './codeplug.mjs';

export class ConnectedCps {
    constructor(root, editor, connection) {
        this.editor = editor;
        this.connection = connection;
        this.readButton = root.querySelector('#cps-read');
        this.writeButton = root.querySelector('#cps-write');
        this.cancelButton = root.querySelector('#cps-cancel');
        this.status = root.querySelector('#cps-radio-status');
        this.transfer = new CpsTransfer(connection, progress => this.progress(progress));
        this.readButton.addEventListener('click', () => this.read());
        this.writeButton.addEventListener('click', () => this.review());
        this.cancelButton.addEventListener('click', () => {
            this.transfer.cancel();
            this.cancelButton.disabled = true;
        });
        this.update();
    }

    update() {
        const connected =
            !!this.connection.session && !!(this.connection.info?.capabilities & C.CAP_CPS);
        if (!connected) {
            this.transfer.invalidate();
        }
        this.readButton.disabled = !connected || this.transfer.busy;
        this.writeButton.disabled = !connected || this.transfer.busy;
        this.cancelButton.disabled = !this.transfer.busy;
    }

    message(text, error = false) {
        this.status.textContent = text;
        this.status.classList.toggle('error', error);
    }

    progress(progress) {
        this.update();
        if (progress.operation !== 'save') {
            this.message(
                `${progress.operation === 'read' ? 'Reading' : 'Uploading'} codeplug: ${progress.done}/${progress.total} bytes`
            );
        } else {
            const result = progress.result;
            this.message(
                result.state === C.CPS_STATE_ACCEPTED
                    ? 'Waiting for the radio…'
                    : result.state === C.CPS_STATE_DURABLE
                      ? 'Codeplug saved on the radio.'
                      : result.state === C.CPS_STATE_FAILED
                        ? `Replacement rejected: ${statusText(result.error)}. Your local draft is retained.`
                        : `Codeplug applied; ${result.saveError ? `save failed: ${statusText(result.saveError)}. The radio will retry.` : 'saving…'}`,
                !!result.error || !!result.saveError
            );
        }
    }

    async read() {
        try {
            const pending = this.transfer.read();
            this.update();
            const baseline = await pending;
            this.update();
            this.message(
                `Radio read complete${baseline.pending ? '; unsaved radio changes' : ''}${baseline.protected ? '; protected settings' : ''}.`
            );
            this.editor.confirm(
                'Replace your local draft with the radio codeplug? Export first to keep it. Cancel keeps your draft.',
                () => this.editor.replace(baseline.document, 'radio-codeplug.json')
            );
        } catch (error) {
            this.message(error.message + ' Your local draft is retained.', true);
        } finally {
            this.update();
        }
    }

    async review() {
        if (this.transfer.busy) {
            return;
        }
        try {
            if (this.editor.dirtyForm && !this.editor.apply()) {
                return;
            }
            const draft = this.editor.document;
            const importSequence = this.editor.importSequence;
            const replacement = structuredClone(validate(this.editor.document));
            const errors = targetErrors(replacement, this.connection.info.target);
            if (errors.length) {
                throw new Error(errors.join(' '));
            }
            let baseline = this.transfer.baseline;
            if (!baseline || baseline.session !== this.connection.session) {
                this.message('Reading radio for comparison · Draft kept.');
                const pending = this.transfer.read();
                this.update();
                baseline = await pending;
            }
            if (!this.connection.session || baseline.session !== this.connection.session) {
                throw new Error('The radio connection changed; review again.');
            }
            if (this.editor.document !== draft || this.editor.dirtyForm) {
                throw new Error('Your local draft changed during the read; review again.');
            }
            if (this.editor.dialog.open || this.editor.importSequence !== importSequence) {
                throw new Error('Another codeplug action started; finish it and review again.');
            }
            if (baseline.protected) {
                throw new Error('Radio settings are protected and cannot be replaced.');
            }
            for (const key of Object.keys(replacement.allocation)) {
                replacement.allocation[key] = Math.max(
                    replacement.allocation[key],
                    baseline.document.allocation[key]
                );
            }
            const old = JSON.parse(canonical(baseline.document));
            const ordered = JSON.parse(canonical(replacement));
            const changes = [];
            for (const key of ['global', 'vfo', 'selection', 'allocation']) {
                if (JSON.stringify(old[key]) !== JSON.stringify(ordered[key])) {
                    changes.push(`${key} settings`);
                }
            }
            for (const key of ['channels', 'banks']) {
                const before = new Map(old[key].map(item => [item.id, item]));
                const after = new Map(ordered[key].map(item => [item.id, item]));
                const added = [...after.keys()].filter(id => !before.has(id)).length;
                const removed = [...before.keys()].filter(id => !after.has(id)).length;
                const changed = [...after].filter(
                    ([id, value]) =>
                        before.has(id) && JSON.stringify(before.get(id)) !== JSON.stringify(value)
                ).length;
                if (added || removed || changed) {
                    changes.push(`${key}: ${added} added, ${removed} removed, ${changed} changed`);
                }
            }
            // Full before/after JSON remains reviewable before the explicit write.
            const dialog = this.editor.dialog;
            dialog.querySelector('#cps-before').value = canonical(old);
            dialog.querySelector('#cps-after').value = canonical(replacement);
            this.editor.confirm(
                `Replace all radio channels and settings? ${changes.length ? changes.join('; ') : 'No setting differences.'} Calibration is preserved. Continue writes these changes.`,
                () => {
                    if (this.transfer.baseline !== baseline) {
                        this.message('The radio read changed; review again.', true);
                        return;
                    }
                    return this.write(replacement);
                },
                true
            );
        } catch (error) {
            this.message(error.message + ' Your local draft is retained.', true);
        } finally {
            this.update();
        }
    }

    async write(replacement) {
        try {
            const pending = this.transfer.write(replacement);
            this.update();
            const result = await pending;
            this.progress({ operation: 'save', result });
            if (result.state !== C.CPS_STATE_DURABLE && result.state !== C.CPS_STATE_FAILED) {
                this.message(
                    this.status.textContent + ' Read the radio again to check its current state.'
                );
            }
        } catch (error) {
            this.message(
                error.message +
                    ' Your local draft is retained; read the radio again before retrying.',
                true
            );
        } finally {
            this.update();
        }
    }
}
