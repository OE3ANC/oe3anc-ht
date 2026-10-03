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
        this.writeButton.disabled =
            !connected ||
            this.transfer.busy ||
            !this.transfer.baseline ||
            this.transfer.baseline.protected;
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
                    ? 'Replacement accepted; waiting for the radio.'
                    : result.state === C.CPS_STATE_DURABLE
                      ? `Codeplug applied and durably saved (revision ${result.revision}, generation ${result.generation}).`
                      : result.state === C.CPS_STATE_FAILED
                        ? `Replacement rejected: ${statusText(result.error)}. Your local draft is retained.`
                        : `Codeplug applied (revision ${result.revision}); ${result.saveError ? `save failed: ${statusText(result.saveError)}. The radio will retry.` : 'durable save is pending.'}`,
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
                `Radio read complete: revision ${baseline.revision}, generation ${baseline.generation}${baseline.pending ? '; unsaved radio changes' : ''}${baseline.protected ? '; protected settings' : ''}.`
            );
            this.editor.confirm(
                'Load the complete radio codeplug into this editor? Export first to keep your current draft. Cancel keeps your local draft and still permits a reviewed write using this fresh radio read.',
                () => this.editor.replace(baseline.document, 'radio-codeplug.json')
            );
        } catch (error) {
            this.message(error.message + ' Your local draft is retained.', true);
        } finally {
            this.update();
        }
    }

    review() {
        try {
            if (this.editor.dirtyForm && !this.editor.apply()) {
                return;
            }
            const baseline = this.transfer.baseline;
            if (!baseline || baseline.session !== this.connection.session) {
                throw new Error('Read the radio before writing.');
            }
            const replacement = structuredClone(validate(this.editor.document));
            const errors = targetErrors(replacement, this.connection.info.target);
            if (errors.length) {
                throw new Error(errors.join(' '));
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
                `Replace the complete radio codeplug read at revision ${baseline.revision}? ${changes.length ? changes.join('; ') : 'No setting differences.'} Calibration is outside this codeplug. Continue writes these reviewed values; Cancel keeps your local draft.`,
                () => {
                    if (this.transfer.baseline !== baseline) {
                        this.message('The radio read changed; review again.', true);
                        return;
                    }
                    this.write(replacement);
                },
                true
            );
        } catch (error) {
            this.message(error.message + ' Your local draft is retained.', true);
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
