// SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Journal rules adapted from OpenRTX-c62 tx_power.h/.c at
// 1dad8235cf49b0fd42e265c673ae964f336fbfe0 (platform/targets/c62/).
export const JOURNAL_SIZE = 0x10000;
const START = 0x3b0000;
const TABLE_OFFSET = 0x450;
const RECORD_SIZE = 0x24;
const VHF = [136125000, 146125000, 156125000, 166125000, 173975000];
const UHF = [400125000, 420125000, 440125000, 460125000, 479975000];
const address = value => '0x' + value.toString(16).padStart(6, '0');
const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join(' ');
const empty = bytes => bytes.every(byte => byte === 0) || bytes.every(byte => byte === 255);

export function decodeCalibration(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < JOURNAL_SIZE) {
        throw new Error('Calibration read must cover the complete 64 KiB journal sector');
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, JOURNAL_SIZE);
    const entries = [];
    let position = 0;
    let journalLength = 0;
    let latest = null;
    let warning = '';
    while (position < JOURNAL_SIZE) {
        const at = address(START + position);
        if (JOURNAL_SIZE - position < 4) {
            warning = `Truncated header at ${at}.`;
            break;
        }
        if (empty(bytes.subarray(position, position + 4))) {
            break;
        }

        // Entries are contiguous. Do not search for magic after unknown framing:
        // payload bytes could otherwise be mistaken for another journal entry.
        const length = view.getUint16(position + 2, true);
        if (bytes[position] !== 0x5a || (length !== 0xbdc && length !== 0xd48)) {
            warning = `Unrecognized journal framing at ${at}.`;
            break;
        }
        if (length > JOURNAL_SIZE - position) {
            warning = `Truncated entry at ${at}.`;
            break;
        }
        if (journalLength && length !== journalLength) {
            warning = `Journal layout changes at ${at}.`;
            break;
        }
        journalLength = length;

        // The additive checksum covers the payload, excluding the four-byte header.
        let checksum = 0;
        for (let i = position + 4; i < position + length; i++) {
            checksum = (checksum + bytes[i]) & 255;
        }
        const entry = {
            offset: START + position,
            length,
            slots: length === 0xbdc ? 10 : 20,
            checksum: bytes[position + 1],
            computedChecksum: checksum,
            valid: checksum === bytes[position + 1],
            records: []
        };
        if (entry.valid) {
            for (let slot = 0; slot < entry.slots; slot++) {
                // The table offset is entry-relative, including the journal header.
                const offset = position + TABLE_OFFSET + slot * RECORD_SIZE;
                const raw = bytes.subarray(offset, offset + RECORD_SIZE);
                if (empty(raw)) {
                    continue;
                }
                const frequency = view.getUint32(offset, true);
                const duties = [4, 6, 8, 10].map(field => view.getUint16(offset + field, true));
                const band = VHF.includes(frequency)
                    ? 'VHF'
                    : UHF.includes(frequency)
                      ? 'UHF'
                      : 'Unknown';
                const flags = [];
                if (band === 'Unknown') {
                    flags.push('Unknown reference frequency');
                }
                if (duties.some(duty => duty > 100)) {
                    flags.push('PWM duty exceeds 100%');
                }
                entry.records.push({
                    slot,
                    offset: START + offset,
                    frequency,
                    duties,
                    band,
                    flags,
                    raw: hex(raw),
                    unknown: hex(raw.subarray(12))
                });
            }
            // Append order identifies the newest checksum-valid entry, not RF accuracy.
            latest = entry.offset;
        }
        entries.push(entry);
        position += length;
    }

    return { entries, latest, stoppedAt: START + position, warning };
}

export function showCalibration(root, result, info, hash, size) {
    root.replaceChildren();
    const add = (parent, tag, text) => {
        const element = document.createElement(tag);
        if (text !== undefined) {
            element.textContent = text;
        }
        parent.append(element);
        return element;
    };
    add(root, 'h3', 'Factory power calibration · read-only snapshot');
    add(
        root,
        'p',
        `Read from radio UUID ${info.chipId ?? 'unavailable'} · flash ${address(info.flashId)}. Verified region [${address(START)}, ${address(START + size)}) · SHA-256: ${hash}`
    );
    if (result.warning) {
        add(
            root,
            'p',
            `${result.warning} Scan stopped; later bytes have not been decoded.`
        ).className = 'error';
    }
    add(
        root,
        'p',
        result.latest === null
            ? 'No checksum-valid journal entries found.'
            : `Latest checksum-valid entry: ${address(result.latest)}. This is not proof of semantic validity or measured RF power. Selectors are PWM duty percentages, not wattages or low/high labels.`
    );
    for (const entry of result.entries) {
        const details = add(root, 'details');
        details.open = entry.offset === result.latest;
        add(
            details,
            'summary',
            `Entry ${address(entry.offset)} · ${entry.slots} slots · ${entry.valid ? 'checksum valid' : 'CHECKSUM MISMATCH'}${details.open ? ' · latest checksum-valid' : ''}`
        );
        add(
            details,
            'p',
            `Length ${address(entry.length)} · stored checksum ${entry.checksum} · computed ${entry.computedChecksum}`
        );
        if (!entry.valid) {
            add(details, 'p', 'Untrusted payload; table not decoded.').className = 'error';
            continue;
        }
        if (!entry.records.length) {
            add(details, 'p', 'All RF slots are empty or erased.');
            continue;
        }
        const scroll = add(details, 'div');
        scroll.className = 'calibration-scroll';
        const table = add(scroll, 'table');
        add(
            table,
            'caption',
            `Table ${address(entry.offset + TABLE_OFFSET)} · ${entry.records.length} populated slots`
        );
        const headings = add(add(table, 'thead'), 'tr');
        for (const title of [
            'Slot',
            'Frequency (Hz)',
            'Band',
            'Selector 0 (%)',
            'Selector 1 (%)',
            'Selector 2 (%)',
            'Selector 3 (%)',
            'Flags'
        ]) {
            add(headings, 'th', title).scope = 'col';
        }
        const body = add(table, 'tbody');
        for (const record of entry.records) {
            const row = add(body, 'tr');
            for (const value of [
                record.slot,
                record.frequency,
                record.band,
                ...record.duties,
                record.flags.join('; ') || '—'
            ]) {
                add(row, 'td', String(value));
            }
            if (record.flags.length) {
                row.className = 'error';
            }
        }
        const raw = add(details, 'details');
        add(raw, 'summary', 'Raw record bytes, including undecoded fields');
        for (const record of entry.records) {
            add(
                raw,
                'pre',
                `Slot ${record.slot} at ${address(record.offset)}\n36 bytes: ${record.raw}\nUndecoded +0x0C..+0x23: ${record.unknown}`
            );
        }
    }
}
