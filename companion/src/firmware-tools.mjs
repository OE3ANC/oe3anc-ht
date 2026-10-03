// SPDX-License-Identifier: GPL-3.0-or-later
import { CskBootloader, FACTORY_OFFSET } from './bootloader.mjs';
import { createFlashBackup, parseFlashBackup, MAX_BACKUP_BYTES } from './flash-backup.mjs';
import { sha256, parseFirmwareBundle, MAX_BUNDLE_BYTES, ERASE_SIZE } from './firmware-bundle.mjs';
import { decodeCalibration, showCalibration, JOURNAL_SIZE } from './calibration.mjs';

const address = value => '0x' + value.toString(16).padStart(6, '0');

function download(content, filename, type) {
    const url = URL.createObjectURL(new Blob([content], { type }));
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function restoreError(backup, info) {
    if (!info?.chipId) {
        return 'Full restore unavailable: this connection has no verified radio UUID.';
    }
    if (!backup) {
        return 'Select a complete flash backup to restore this radio.';
    }
    if (!backup.device.chip_id) {
        return 'Full restore unavailable: the backup has no verified radio UUID.';
    }
    if (backup.device.chip_id !== info.chipId || backup.device.flash_id !== info.flashId) {
        return 'Full restore unavailable: the backup belongs to a different radio or flash device.';
    }
    return '';
}

export class FirmwareTools {
    constructor(root, { available, changed }) {
        this.root = root;
        this.available = available;
        this.changed = changed;
        this.status = root.querySelector('[data-firmware-status]');
        this.progress = root.querySelector('progress');
        this.dialog = root.querySelector('[data-flash-review]');
        this.acknowledgement = root.querySelector('[data-flash-ack]');
        this.verifyAfterWrite = root.querySelector('[data-flash-verify]');
        this.flashTarget = root.querySelector('[data-flash-target]');
        this.flashSelection = root.querySelector('[data-flash-selection]');
        this.confirm = root.querySelector('[data-flash-confirm]');
        this.connection = new CskBootloader(error => {
            if (this.dialog.open) {
                this.dialog.close('cancel');
            }
            if (error && !this.abort?.signal.aborted) {
                this.message(error.message, true);
            }
            this.update();
            this.changed();
        });
        root.querySelector('[data-boot-connect]').addEventListener('click', () => {
            void this.connect();
        });
        root.querySelector('[data-boot-close]').addEventListener('click', async () => {
            await this.connection.close();
            this.message(
                'Bootloader disconnected. Unplug before rebooting; live connection requires a fresh handshake.'
            );
        });
        root.querySelector('[data-flash-backup]').addEventListener('click', () => {
            void this.read(false);
        });
        root.querySelector('[data-factory-read]').addEventListener('click', () => {
            void this.read(true);
        });
        root.querySelector('[data-factory-download]').addEventListener('click', () => {
            if (this.busy) {
                return;
            }
            if (this.factoryBytes) {
                download(
                    this.factoryBytes,
                    'c62-factory-region-0x3b0000.bin',
                    'application/octet-stream'
                );
            } else {
                void this.read(true, true);
            }
        });
        root.querySelector('[data-boot-cancel]').addEventListener('click', () => {
            this.abort?.abort(new DOMException('Cancelled', 'AbortError'));
            if (this.dialog.open) {
                this.dialog.close('cancel');
            }
        });
        for (const restoring of [false, true]) {
            root.querySelector(
                restoring ? '[data-backup-file]' : '[data-bundle-file]'
            ).addEventListener('change', event => {
                const file = event.target.files?.[0];
                event.target.value = '';
                if (file) {
                    void this.selectFile(file, restoring);
                }
            });
            root.querySelector(
                restoring ? '[data-flash-restore]' : '[data-flash-update]'
            ).addEventListener('click', () => {
                void this.write(restoring);
            });
        }
        this.acknowledgement.addEventListener('change', () => {
            this.confirm.disabled = !this.acknowledgement.checked;
        });
        this.confirm.addEventListener('click', () => {
            if (
                this.dialog.open &&
                this.acknowledgement.checked &&
                this.connection.ready &&
                !this.abort.signal.aborted
            ) {
                this.dialog.close('write');
            }
        });
        root.querySelector('[data-flash-dismiss]').addEventListener('click', () =>
            this.dialog.close('cancel')
        );
        this.dialog.addEventListener('close', () => {
            const proceed = this.dialog.returnValue === 'write' && this.acknowledgement.checked;
            this.acknowledgement.checked = false;
            this.confirm.disabled = true;
            this.resolveReview?.(proceed);
            this.resolveReview = null;
        });
        this.update();
    }

    get ownsPort() {
        return this.busy || !!this.connection.port;
    }

    update() {
        const supported = 'serial' in navigator;
        this.root.querySelector('[data-boot-connect]').disabled =
            !supported || this.ownsPort || !this.available();
        this.root.querySelector('[data-boot-close]').disabled = this.busy || !this.connection.port;
        for (const name of ['data-flash-backup', 'data-factory-read']) {
            this.root.querySelector(`[${name}]`).disabled = this.busy || !this.connection.ready;
        }
        this.root.querySelector('[data-factory-download]').disabled =
            this.busy || (!this.factoryBytes && !this.connection.ready);
        for (const name of ['data-bundle-file', 'data-backup-file']) {
            this.root.querySelector(`[${name}]`).disabled = !!this.busy;
        }
        this.root.querySelector('[data-flash-update]').disabled =
            this.busy || !this.connection.ready || !this.bundle;
        const reason = restoreError(this.backup, this.connection.info);
        this.root.querySelector('[data-restore-status]').textContent =
            reason ||
            'Verified backup identity matches this radio. Full restore replaces all flash bytes.';
        this.root.querySelector('[data-flash-restore]').disabled =
            this.busy || !this.connection.ready || !!reason;
        this.root.querySelector('[data-boot-cancel]').disabled = !this.busy;
    }

    message(text, error = false) {
        this.status.textContent = text;
        this.status.classList.toggle('error', error);
    }

    start() {
        this.busy = true;
        this.abort = new AbortController();
        this.progress.value = 0;
        this.progress.max = 1;
        this.update();
        this.changed();
    }

    finish() {
        this.busy = false;
        this.update();
        this.changed();
    }

    showProgress(value) {
        this.progress.max = value.total;
        this.progress.value = value.completed;
        const subject = value.image ? ` · ${value.image}` : '';
        this.message(
            value.stage === 'check-dsp'
                ? 'Checking the existing DSP matches this bundle…'
                : value.stage === 'verify'
                  ? `Read complete${subject}; verifying against the radio…`
                  : `${value.stage === 'helper' ? 'Loading RAM helper' : value.stage === 'write' ? 'Writing flash' : 'Reading flash'}${subject}: ${value.completed.toLocaleString()} / ${value.total.toLocaleString()} bytes`
        );
    }

    async connect() {
        if (this.ownsPort || !this.available() || !('serial' in navigator)) {
            return;
        }
        this.start();
        try {
            const port = await navigator.serial.requestPort();
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            const response = await fetch(
                new URL('../bootloader/burner_venus.bin', import.meta.url),
                { signal: this.abort.signal }
            );
            if (!response.ok) {
                throw new Error('Could not load the pinned bootloader helper');
            }
            const info = await this.connection.connect(
                port,
                new Uint8Array(await response.arrayBuffer()),
                { signal: this.abort.signal, progress: value => this.showProgress(value) }
            );
            this.message(
                `CSK6 connected · ${info.flashSize.toLocaleString()} bytes flash · UUID ${info.chipId ?? 'unavailable'}. Dump a complete backup before updating. ${info.chipId ? 'Select a matching backup for full restore.' : 'Full restore unavailable without a verified UUID.'}`
            );
        } catch (error) {
            await this.connection.close();
            this.message(
                error.name === 'AbortError' ? 'Connection cancelled.' : error.message,
                error.name !== 'AbortError'
            );
        } finally {
            this.finish();
        }
    }

    async read(factory, downloadRaw = false) {
        if (this.busy || !this.connection.ready) {
            return;
        }
        if (factory) {
            this.factoryBytes = null;
            this.root.querySelector('[data-calibration]').replaceChildren();
        }
        this.start();
        try {
            const info = this.connection.info;
            const offset = factory ? FACTORY_OFFSET : 0;
            const size = factory && !downloadRaw ? JOURNAL_SIZE : info.flashSize - offset;
            const bytes = await this.connection.readRange(offset, size, {
                signal: this.abort.signal,
                progress: value => this.showProgress(value)
            });
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            const content = factory ? bytes : await createFlashBackup(info, bytes);
            const hash = await sha256(bytes);
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            if (factory) {
                const result = decodeCalibration(bytes);
                showCalibration(
                    this.root.querySelector('[data-calibration]'),
                    result,
                    info,
                    hash,
                    bytes.length
                );
                if (downloadRaw) {
                    this.factoryBytes = bytes;
                    download(bytes, 'c62-factory-region-0x3b0000.bin', 'application/octet-stream');
                    this.message(
                        `Full factory region read verified and local download prepared. SHA-256 ${hash}.`
                    );
                } else {
                    this.message(
                        '64 KiB calibration journal read verified. Download raw factory region reads the remaining factory data too. PWM duties are not measured RF watts.'
                    );
                }
            } else {
                download(
                    content,
                    `c62-full-backup-${info.chipId ?? 'identity-unavailable'}.json`,
                    'application/json'
                );
                this.message(
                    `Complete flash backup verified and download prepared. SHA-256 ${hash}${info.chipId ? '.' : '. Device identity was unavailable; this backup cannot be restored with an identity check.'}`
                );
            }
        } catch (error) {
            if (error.name === 'AbortError') {
                await this.connection.close();
            }
            this.message(
                error.name === 'AbortError'
                    ? 'Read cancelled; no successful backup or download was produced. Reconnect the bootloader to retry.'
                    : error.message,
                error.name !== 'AbortError'
            );
        } finally {
            this.finish();
        }
    }

    async selectFile(file, restoring) {
        if (this.busy) {
            return;
        }
        const key = restoring ? 'backup' : 'bundle';
        const label = this.root.querySelector(
            restoring ? '[data-backup-status]' : '[data-bundle-status]'
        );
        this[key] = null;
        label.textContent = 'Validating ' + file.name + '…';
        this.start();
        try {
            if (file.size > (restoring ? MAX_BACKUP_BYTES : MAX_BUNDLE_BYTES)) {
                throw new Error('Selected file exceeds the supported size limit');
            }
            const text = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(
                await file.arrayBuffer()
            );
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            const parsed = await (restoring ? parseFlashBackup(text) : parseFirmwareBundle(text));
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            // Retain immutable text, not mutable decoded byte arrays. Preview and
            // transport each revalidate this same source before programming.
            this[key] = { text, name: file.name, ...(restoring ? { device: parsed.device } : {}) };
            label.textContent = `${file.name} · validated ${restoring ? 'complete 4 MiB backup · SHA-256 ' + parsed.sha256 : 'application/DSP pair · ' + parsed.release.identity}`;
            this.message(
                'File validated locally. Connect the bootloader, then review the destination before writing.'
            );
        } catch (error) {
            label.textContent = 'No valid file selected.';
            this.message(
                error.name === 'AbortError' ? 'File validation cancelled.' : error.message,
                error.name !== 'AbortError'
            );
        } finally {
            this.finish();
        }
    }

    async write(restoring) {
        const selected = restoring ? this.backup : this.bundle;
        if (
            this.busy ||
            !this.connection.ready ||
            !selected ||
            (restoring && restoreError(selected, this.connection.info))
        ) {
            return;
        }
        const info = this.connection.info;
        this.start();
        try {
            const parsed = await (restoring
                ? parseFlashBackup(selected.text)
                : parseFirmwareBundle(selected.text));
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            if (!this.connection.ready || this.connection.info !== info) {
                throw new Error('Bootloader changed; reconnect and review again');
            }
            if (restoring && restoreError(parsed, info)) {
                throw new Error(restoreError(parsed, info));
            }
            this.flashTarget.value = 'pair';
            this.flashSelection.hidden = restoring;
            const showPreview = () => {
                const applicationOnly = !restoring && this.flashTarget.value === 'application';
                const lines = [
                    `File: ${selected.name}`,
                    `Target: C62 · ${info.flashSize.toLocaleString()} bytes`,
                    `Flash JEDEC: ${address(info.flashId)} · radio UUID: ${info.chipId ?? 'unavailable'}`
                ];
                if (restoring) {
                    lines.push(
                        'Replaces ALL flash: firmware, DSP, settings, factory calibration and unknown bytes.',
                        `Write and erase: [0x000000, 0x400000) · 4,194,304 bytes`,
                        `Backup SHA-256: ${parsed.sha256}`
                    );
                } else {
                    lines.push(
                        `Release: ${parsed.release.identity}`,
                        `Commit: ${parsed.release.commit}`
                    );
                    for (const image of parsed.images.filter(
                        image => !applicationOnly || image.role === 'application'
                    )) {
                        lines.push(
                            `${image.role}: ${address(image.offset)} · ${image.size.toLocaleString()} bytes`,
                            `Sector erase: [${address(image.offset)}, ${address(image.offset + Math.ceil(image.size / ERASE_SIZE) * ERASE_SIZE)})`,
                            `SHA-256: ${image.sha256}`
                        );
                    }
                    if (applicationOnly) {
                        lines.push(
                            'DSP is preserved; its match to this bundle is checked before erasing the application.'
                        );
                    }
                    lines.push(
                        'Settings, factory calibration and bytes outside these erase ranges are preserved.'
                    );
                }
                lines.push(
                    'Keep power and cable connected until the operation finishes. Cancellation after writing begins may leave incomplete flash.'
                );
                this.root.querySelector('[data-flash-preview]').textContent = lines.join('\n');
                this.root.querySelector('[data-flash-review-title]').textContent = restoring
                    ? 'Restore complete same-radio backup'
                    : applicationOnly
                      ? 'Flash application only'
                      : 'Flash matching application/DSP pair';
                this.confirm.textContent = restoring
                    ? 'Restore all flash'
                    : applicationOnly
                      ? 'Flash application'
                      : 'Flash matched pair';
            };
            this.flashTarget.onchange = () => {
                this.acknowledgement.checked = false;
                this.confirm.disabled = true;
                showPreview();
            };
            showPreview();
            this.acknowledgement.checked = false;
            this.verifyAfterWrite.checked = true;
            this.confirm.disabled = true;
            this.dialog.returnValue = '';
            this.message('Review the final preview. No flash bytes have been changed.');
            const proceed = await new Promise(resolve => {
                this.resolveReview = resolve;
                this.dialog.showModal();
            });
            if (this.abort.signal.aborted) {
                throw this.abort.signal.reason;
            }
            if (!proceed) {
                this.message('Preview cancelled; no flash bytes were changed.');
                return;
            }
            if (!this.connection.ready || this.connection.info !== info) {
                throw new Error('Bootloader changed; reconnect and review again');
            }
            const verifyAfterWrite = this.verifyAfterWrite.checked;
            const applicationOnly = !restoring && this.flashTarget.value === 'application';
            const options = {
                acknowledged: true,
                verifyAfterWrite,
                applicationOnly,
                signal: this.abort.signal,
                progress: value => this.showProgress(value)
            };
            this.message(
                `Revalidating source and radio identity; keep power and cable connected through programming${verifyAfterWrite ? ' and verification' : ''}…`
            );
            const result = await (restoring
                ? this.connection.restoreBackup(selected.text, options)
                : this.connection.updateFirmware(selected.text, options));
            this.message(
                `${restoring ? 'Complete flash restore' : applicationOnly ? 'Application-only update' : 'Application/DSP update'} completed${verifyAfterWrite ? ' and every written byte verified' : '; readback verification skipped'}. ${restoring ? 'Backup SHA-256' : 'Release'}: ${result}. Unplug before rebooting; connect the freshly booted application with its matching companion.`
            );
        } catch (error) {
            if (this.dialog.open) {
                this.dialog.close('cancel');
            }
            await this.connection.close();
            this.message(
                error.flashMayHaveChanged
                    ? `${error.message}. Flash may be incomplete and the radio may not boot. Keep your backup; re-enter bootloader mode, reconnect and repeat the complete paired update or this same-radio restore.`
                    : `${error.name === 'AbortError' ? 'Operation cancelled' : error.message}. No flash write was started; reconnect to retry.`,
                true
            );
        } finally {
            this.resolveReview = null;
            this.flashTarget.onchange = null;
            this.finish();
        }
    }
}
