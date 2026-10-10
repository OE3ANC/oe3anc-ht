// SPDX-License-Identifier: GPL-3.0-or-later
import {
    MAX_BYTES,
    MAX_CHANNELS,
    MAX_BANKS,
    MAX_ID,
    VFO_STEPS_HZ,
    fresh,
    parseBytes,
    canonical,
    validate,
    putRecord,
    removeRecord,
    targetErrors,
    fail
} from './codeplug.mjs';

function element(tag, text = '', attributes = {}) {
    const node = document.createElement(tag);
    node.textContent = text;
    for (const [key, value] of Object.entries(attributes)) {
        node.setAttribute(key, value);
    }
    return node;
}

function button(text, action, disabled = false) {
    const node = element('button', text, { type: 'button' });
    node.disabled = disabled;
    node.addEventListener('click', action);
    return node;
}

function field(parent, label, value, choices, attributes = {}) {
    const wrapper = element('label', '', { class: 'cps-field' });
    wrapper.append(element('span', label));
    let input;
    if (choices) {
        input = element('select');
        for (const item of choices) {
            const [key, title] = Array.isArray(item) ? item : [item, String(item)];
            input.append(element('option', title, { value: String(key) }));
        }
        input.value = String(value);
    } else if (typeof value === 'boolean') {
        input = element('input', '', { type: 'checkbox' });
        input.checked = value;
    } else {
        input = element('input', '', {
            type: typeof value === 'number' ? 'number' : 'text',
            ...attributes
        });
        input.value = String(value);
    }
    wrapper.append(input);
    parent.append(wrapper);
    return input;
}

function number(input, path) {
    if (!/^[0-9]+$/.test(input.value)) {
        fail(path, 'enter a whole number');
    }
    const value = Number(input.value);
    if (!Number.isSafeInteger(value)) {
        fail(path, 'number is too large');
    }
    return value;
}

function decimal(input, digits, path) {
    if (!new RegExp(`^[0-9]+(?:\\.[0-9]{1,${digits}})?$`).test(input.value)) {
        fail(path, `enter a positive value with at most ${digits} decimal places`);
    }
    const [whole, fraction = ''] = input.value.split('.');
    const value = Number(whole + fraction.padEnd(digits, '0'));
    if (!Number.isSafeInteger(value)) {
        fail(path, 'number is too large');
    }
    return value;
}

function formatFrequency(hz) {
    return (hz / 1000000).toFixed(6).replace(/0+$/, '').replace(/\.$/, '');
}

function toneEditor(parent, label, original) {
    const box = element('fieldset');
    const legend = element('legend', label);
    box.append(legend);
    parent.append(box);
    const kind = field(box, 'Tone', original.kind, [
        ['none', 'None'],
        ['ctcss', 'CTCSS'],
        ['dcs', 'DCS']
    ]);
    const details = element('div', '', { class: 'cps-grid' });
    box.append(details);
    let get = () => ({ kind: 'none' });
    const branches = new Map();
    function render() {
        // Retain detached controls, including invalid raw text awaiting correction.
        if (branches.has(kind.value)) {
            const saved = branches.get(kind.value);
            details.replaceChildren(saved.node);
            get = saved.read;
            return;
        }
        const branch = element('div', '', { class: 'cps-grid' });
        details.replaceChildren(branch);
        if (kind.value === 'ctcss') {
            const value = field(
                branch,
                'CTCSS frequency (Hz)',
                String((original.tenths_hz ?? 670) / 10)
            );
            get = () => ({ kind: 'ctcss', tenths_hz: decimal(value, 1, label) });
        } else if (kind.value === 'dcs') {
            const code = field(branch, 'DCS code (octal)', original.code ?? '023', null, {
                maxlength: '3'
            });
            const polarity = field(branch, 'Polarity', original.polarity ?? 'normal', [
                'normal',
                'inverted'
            ]);
            get = () => ({ kind: 'dcs', code: code.value, polarity: polarity.value });
        } else {
            get = () => ({ kind: 'none' });
        }
        branches.set(kind.value, { node: branch, read: get });
    }
    kind.addEventListener('change', render);
    render();
    return () => get();
}

function operatingEditor(parent, original) {
    const common = element('div', '', { class: 'cps-grid' });
    parent.append(common);
    const mode = field(common, 'Mode', original.mode, [
        ['fm', 'FM'],
        ['m17', 'M17']
    ]);
    const rx = field(common, 'RX frequency (MHz)', formatFrequency(original.rx_frequency_hz));
    const tx = field(common, 'TX frequency (MHz)', formatFrequency(original.tx_frequency_hz));
    const inhibit = field(common, 'Receive only (inhibit TX)', original.tx_inhibit);
    const power = field(common, 'Requested power (W)', original.power_mw / 1000, null, {
        min: '0.001',
        max: '5',
        step: '0.001'
    });
    const width = field(common, 'Bandwidth', original.bandwidth, [
        ['narrow', 'Narrow'],
        ['wide', 'Wide']
    ]);
    const squelch = field(common, 'Squelch (0–15)', original.squelch, null, {
        min: '0',
        max: '15'
    });
    const specific = element('div', '', { class: 'cps-grid' });
    parent.append(specific);
    let getSpecific;
    const branches = new Map();
    function renderMode() {
        if (branches.has(mode.value)) {
            const saved = branches.get(mode.value);
            specific.replaceChildren(saved.node);
            getSpecific = saved.read;
            return;
        }
        const branch = element('div', '', { class: 'cps-grid' });
        specific.replaceChildren(branch);
        if (mode.value === 'fm') {
            const rxTone = toneEditor(branch, 'RX tone', original.fm?.rx_tone ?? { kind: 'none' });
            const txTone = toneEditor(branch, 'TX tone', original.fm?.tx_tone ?? { kind: 'none' });
            getSpecific = () => ({ fm: { rx_tone: rxTone(), tx_tone: txTone() } });
        } else {
            const settings = original.m17 ?? {
                destination: { kind: 'broadcast' },
                can: 0,
                rx_can_check: false
            };
            const destination = field(branch, 'M17 destination', settings.destination.kind, [
                'broadcast',
                'station'
            ]);
            const station = field(
                branch,
                'Destination callsign',
                settings.destination.callsign ?? '',
                null,
                { maxlength: '9' }
            );
            const can = field(branch, 'CAN (0–15)', settings.can, null, { min: '0', max: '15' });
            const rxCheck = field(branch, 'Check RX CAN', settings.rx_can_check);
            const updateDestination = () => {
                station.disabled = destination.value !== 'station';
            };
            destination.addEventListener('change', updateDestination);
            updateDestination();
            getSpecific = () => ({
                m17: {
                    destination:
                        destination.value === 'station'
                            ? { kind: 'station', callsign: station.value }
                            : { kind: 'broadcast' },
                    can: number(can, 'CAN'),
                    rx_can_check: rxCheck.checked
                }
            });
        }
        branches.set(mode.value, { node: branch, read: getSpecific });
    }
    mode.addEventListener('change', renderMode);
    renderMode();
    return () => ({
        mode: mode.value,
        rx_frequency_hz: decimal(rx, 6, 'RX frequency'),
        tx_frequency_hz: decimal(tx, 6, 'TX frequency'),
        tx_inhibit: inhibit.checked,
        power_mw: decimal(power, 3, 'Power'),
        bandwidth: width.value,
        squelch: number(squelch, 'Squelch'),
        ...getSpecific()
    });
}

export class CpsEditor {
    constructor(root) {
        this.root = root;
        this.document = fresh();
        this.panel = 'global';
        this.recordId = 0;
        this.dirtyForm = false;
        this.importSequence = 0;
        this.filename = 'codeplug.json';
        this.content = root.querySelector('#cps-content');
        this.status = root.querySelector('#cps-status');
        this.dialog = root.querySelector('#cps-confirm');
        root.querySelector('#cps-new').addEventListener('click', () => {
            this.importSequence++;
            this.confirm(
                'Replace the current codeplug with an empty one? Export first to keep a copy.',
                () => this.replace(fresh(), 'codeplug.json')
            );
        });
        root.querySelector('#cps-import').addEventListener('change', event =>
            this.importFile(event.target)
        );
        root.querySelector('#cps-export').addEventListener('click', () => this.exportFile());
        root.querySelectorAll('[data-cps-panel]').forEach(node => {
            node.addEventListener('click', () => this.switchPanel(node.dataset.cpsPanel));
        });
        this.render();
    }

    message(text, error = false) {
        this.status.textContent = text;
        this.status.classList.toggle('error', error);
    }

    confirm(text, accept, showReview = false) {
        const review = this.dialog.querySelector('#cps-write-review');
        if (review) {
            review.hidden = !showReview;
        }
        this.dialog.querySelector('p').textContent = text;
        this.dialog.returnValue = '';
        this.dialog.onclose = () => {
            if (this.dialog.returnValue === 'replace') {
                accept();
            }
        };
        this.dialog.showModal();
    }

    replace(document, filename) {
        this.document = structuredClone(validate(document));
        this.filename = filename;
        this.panel = 'global';
        this.recordId = 0;
        this.dirtyForm = false;
        this.render();
        this.message('Loaded locally · Radio unchanged.');
    }

    async importFile(input) {
        const file = input.files[0];
        input.value = '';
        if (!file) {
            return;
        }
        const sequence = ++this.importSequence;
        try {
            if (file.size > MAX_BYTES) {
                fail('JSON', `input exceeds ${MAX_BYTES} bytes`);
            }
            const bytes = await file.arrayBuffer();
            if (sequence !== this.importSequence) {
                return;
            }
            const document = parseBytes(new Uint8Array(bytes));
            this.confirm(
                `Replace the current codeplug with ${file.name} (${document.channels.length} channels, ${document.banks.length} banks)? Export first to keep a copy.`,
                () =>
                    this.replace(
                        document,
                        file.name.replace(/[^A-Za-z0-9_.-]/g, '_').slice(0, 80) || 'codeplug.json'
                    )
            );
        } catch (error) {
            if (sequence === this.importSequence) {
                this.message(error.message, true);
            }
        }
    }

    exportFile() {
        try {
            if (this.dirtyForm && !this.apply()) {
                return;
            }
            const bytes = canonical(this.document);
            const url = URL.createObjectURL(new Blob([bytes], { type: 'application/json' }));
            const anchor = element('a', '', { href: url, download: this.filename });
            this.root.append(anchor);
            anchor.click();
            anchor.remove();
            setTimeout(() => URL.revokeObjectURL(url), 1000);
            this.message('Codeplug exported.');
        } catch (error) {
            this.message(error.message, true);
        }
    }

    switchPanel(panel, id = 0, draft = null) {
        const switchNow = () => {
            this.panel = panel;
            this.recordId = id;
            this.creationDraft = draft;
            this.dirtyForm = !!draft;
            this.render();
            this.message('Editing local codeplug.');
        };
        if (this.dirtyForm) {
            this.confirm(
                'Discard unsaved values in this editor? Applied codeplug edits will be kept.',
                switchNow
            );
        } else {
            switchNow();
        }
    }

    apply() {
        try {
            const result = this.readEditor();
            this.document = structuredClone(validate(result.document));
            if (result.id !== undefined) {
                this.recordId = result.id;
            }
            this.creationDraft = null;
            this.dirtyForm = false;
            this.render();
            const warnings = targetErrors(this.document);
            this.message(
                'Edits applied to the local codeplug.' +
                    (warnings.length ? ' ' + warnings.join(' ') : '')
            );
            return true;
        } catch (error) {
            this.message(error.message, true);
            return false;
        }
    }

    render() {
        this.content.replaceChildren();
        this.root.querySelector('#cps-json').value = canonical(this.document);
        this.root
            .querySelectorAll('[data-cps-panel]')
            .forEach(node =>
                node.setAttribute('aria-pressed', String(node.dataset.cpsPanel === this.panel))
            );
        this.root.querySelector('#cps-summary').textContent =
            `${this.document.channels.length}/${MAX_CHANNELS} channels · ${this.document.banks.length}/${MAX_BANKS} banks`;
        const layout = element('div', '', { class: 'cps-layout' });
        this.content.append(layout);
        if (['channels', 'banks'].includes(this.panel)) {
            this.recordList(layout);
        }
        const form = element('form', '', { class: 'cps-editor' });
        layout.append(form);
        form.addEventListener('input', () => {
            this.dirtyForm = true;
        });
        form.addEventListener('change', () => {
            this.dirtyForm = true;
        });
        form.addEventListener('submit', event => {
            event.preventDefault();
            this.apply();
        });
        if (this.panel === 'global') {
            this.globalEditor(form);
        } else if (this.panel === 'vfo') {
            form.append(element('h3', 'VFO'));
            const read = operatingEditor(form, this.document.vfo);
            this.readEditor = () => ({ document: { ...this.document, vfo: read() } });
        } else if (this.panel === 'selection') {
            this.selectionEditor(form);
        } else if (this.panel === 'channels') {
            this.channelEditor(form);
        } else {
            this.bankEditor(form);
        }
        const actions = element('div', '', { class: 'actions' });
        form.append(actions);
        actions.append(
            element('button', 'Apply to codeplug', { type: 'submit' }),
            button('Reset editor', () => {
                this.dirtyForm = false;
                this.creationDraft = null;
                this.render();
                this.message('Unsaved edits discarded.');
            })
        );
    }

    globalEditor(form) {
        form.append(element('h3', 'Radio settings'));
        const radio = element('fieldset');
        radio.append(element('legend', 'Identity and operation'));
        let grid = element('div', '', { class: 'cps-grid' });
        radio.append(grid);
        form.append(radio);
        const settings = structuredClone(this.document.global);
        const readers = [];
        const add = (label, path, choices, attributes) => {
            const parts = path.split('.');
            let owner = settings;
            for (const part of parts.slice(0, -1)) {
                owner = owner[part];
            }
            const key = parts.at(-1);
            const original = owner[key];
            const input = field(grid, label, original, choices, attributes);
            readers.push(() => {
                owner[key] =
                    typeof original === 'number'
                        ? number(input, label)
                        : typeof original === 'boolean'
                          ? input.checked
                          : input.value;
            });
        };
        add('Local callsign', 'local_callsign', null, { maxlength: '9' });
        add('Gain (current radios require 0)', 'gain', null, { min: '0', max: '15' });
        add('TX time limit', 'transmit_limit_s', [
            [0, 'Off'],
            [60, '60 seconds'],
            [120, '120 seconds'],
            [180, '180 seconds']
        ]);
        add(
            'VFO tuning step',
            'vfo_step_hz',
            VFO_STEPS_HZ.map(hz => [hz, `${hz / 1000} kHz`])
        );
        const display = element('fieldset');
        display.append(element('legend', 'Display and backlight'));
        grid = element('div', '', { class: 'cps-grid' });
        display.append(grid);
        form.append(display);
        add('Theme', 'ui.theme', ['midnight', 'nord', 'solarized-dark', 'darcula']);
        add('Contrast', 'ui.contrast', ['normal', 'high', 'maximum']);
        add('Animations', 'ui.animations');
        add(
            'Active brightness',
            'ui.backlight.brightness_percent',
            [25, 50, 75, 100].map(n => [n, `${n}%`])
        );
        add('Dim after', 'ui.backlight.idle_s', [
            [0, 'Never'],
            [15, '15 seconds'],
            [30, '30 seconds'],
            [60, '60 seconds']
        ]);
        add(
            'Dim brightness',
            'ui.backlight.dim_percent',
            [10, 20, 30].map(n => [n, `${n}%`])
        );
        this.readEditor = () => {
            readers.forEach(read => read());
            return { document: { ...this.document, global: settings } };
        };
    }

    recordList(layout) {
        const kind = this.panel;
        const list = this.document[kind];
        const isChannel = kind === 'channels';
        const sidebar = element('aside', '', {
            class: 'cps-records',
            'aria-label': isChannel ? 'Channels' : 'Banks'
        });
        layout.append(sidebar);
        const full =
            list.length >= (isChannel ? MAX_CHANNELS : MAX_BANKS) ||
            this.document.allocation[isChannel ? 'channel_id_high_water' : 'bank_id_high_water'] ===
                MAX_ID;
        sidebar.append(
            button(isChannel ? 'Add channel' : 'Add bank', () => this.switchPanel(kind), full)
        );
        const ul = element('ul');
        sidebar.append(ul);
        const ordered = [...list].sort(
            isChannel ? (a, b) => a.number - b.number : (a, b) => a.id - b.id
        );
        for (const record of ordered) {
            const li = element('li');
            ul.append(li);
            const node = button(isChannel ? `${record.number}. ${record.name}` : record.name, () =>
                this.switchPanel(kind, record.id)
            );
            node.setAttribute('aria-pressed', String(this.recordId === record.id));
            li.append(node);
        }
        if (!list.length) {
            sidebar.append(element('p', isChannel ? 'No channels yet.' : 'No banks yet.'));
        }
    }

    recordActions(form, kind, original) {
        if (!original.id) {
            return;
        }
        const full =
            this.document[kind].length >= (kind === 'channels' ? MAX_CHANNELS : MAX_BANKS) ||
            this.document.allocation[
                kind === 'channels' ? 'channel_id_high_water' : 'bank_id_high_water'
            ] === MAX_ID;
        const actions = element('div', '', { class: 'actions' });
        form.append(actions);
        actions.append(
            button(
                'Duplicate',
                () => {
                    const draft = structuredClone(original);
                    draft.id = 0;
                    draft.name = draft.name.slice(0, 19) + ' copy';
                    if (kind === 'channels') {
                        draft.number = this.freeNumber();
                    }
                    this.switchPanel(kind, 0, draft);
                },
                full
            ),
            button('Delete', () => {
                this.confirm(
                    `Delete ${original.name} from the local codeplug${kind === 'channels' ? ' and all banks' : ''}?`,
                    () => {
                        this.document = removeRecord(this.document, kind, original.id);
                        this.recordId = this.document[kind][0]?.id ?? 0;
                        this.dirtyForm = false;
                        this.creationDraft = null;
                        this.render();
                        this.message('Record deleted locally.');
                    }
                );
            })
        );
    }

    freeNumber() {
        const used = new Set(this.document.channels.map(c => c.number));
        return Array.from({ length: MAX_CHANNELS }, (_, i) => i + 1).find(n => !used.has(n)) ?? 1;
    }

    channelEditor(form) {
        const original = this.document.channels.find(c => c.id === this.recordId) ??
            this.creationDraft ?? {
                id: 0,
                number: this.freeNumber(),
                name: 'New channel',
                configuration: this.document.vfo
            };
        form.append(element('h3', original.id ? `Channel ${original.number}` : 'New channel'));
        this.recordActions(form, 'channels', original);
        const grid = element('div', '', { class: 'cps-grid' });
        form.append(grid);
        const channelNumber = field(grid, 'Channel number', original.number, null, {
            min: '1',
            max: '256'
        });
        const channelName = field(grid, 'Channel name', original.name, null, { maxlength: '24' });
        const read = operatingEditor(form, original.configuration);
        this.readEditor = () =>
            putRecord(this.document, 'channels', {
                id: original.id,
                number: number(channelNumber, 'Channel number'),
                name: channelName.value,
                configuration: read()
            });
    }

    bankEditor(form) {
        const original = this.document.banks.find(b => b.id === this.recordId) ??
            this.creationDraft ?? { id: 0, name: 'New bank', channel_ids: [] };
        form.append(element('h3', original.id ? 'Edit bank' : 'New bank'));
        this.recordActions(form, 'banks', original);
        const bankName = field(form, 'Bank name', original.name, null, { maxlength: '24' });
        form.append(
            element(
                'p',
                'Membership order determines the bank’s channel order. A channel can belong to several banks.'
            )
        );
        const members = [...original.channel_ids];
        const membership = element('div');
        form.append(membership);
        const channels = new Map(this.document.channels.map(c => [c.id, c]));
        const renderMembers = () => {
            membership.replaceChildren();
            const ul = element('ol', '', { class: 'cps-members' });
            membership.append(ul);
            for (const [index, id] of members.entries()) {
                const channel = channels.get(id);
                const li = element('li');
                ul.append(li);
                li.append(element('span', `${channel.number}. ${channel.name}`));
                const move = delta => {
                    [members[index], members[index + delta]] = [
                        members[index + delta],
                        members[index]
                    ];
                    this.dirtyForm = true;
                    renderMembers();
                };
                li.append(
                    button('Up', () => move(-1), index === 0),
                    button('Down', () => move(1), index === members.length - 1),
                    button('Remove', () => {
                        members.splice(index, 1);
                        this.dirtyForm = true;
                        renderMembers();
                    })
                );
            }
            const available = [...channels.values()]
                .filter(c => !members.includes(c.id))
                .sort((a, b) => a.number - b.number);
            const select = field(
                membership,
                'Add a channel',
                available[0]?.id ?? '',
                available.map(c => [c.id, `${c.number}. ${c.name}`])
            );
            select.disabled = !available.length;
            membership.append(
                button(
                    'Add to bank',
                    () => {
                        members.push(number(select, 'Channel'));
                        this.dirtyForm = true;
                        renderMembers();
                    },
                    !available.length
                )
            );
        };
        renderMembers();
        this.readEditor = () =>
            putRecord(this.document, 'banks', {
                id: original.id,
                name: bankName.value,
                channel_ids: members
            });
    }

    selectionEditor(form) {
        form.append(element('h3', 'Startup selection'));
        const selection = this.document.selection;
        const mode = field(form, 'Operating source', selection.operating, [
            ['vfo', 'VFO'],
            ['memory', 'Memory channel']
        ]);
        const banks = field(form, 'Selected bank', selection.bank_id ?? '', [
            ['', 'All channels'],
            ...this.document.banks.map(b => [b.id, b.name])
        ]);
        const channels = field(form, 'Selected channel', selection.channel_id ?? '', [
            ['', 'None'],
            ...[...this.document.channels]
                .sort((a, b) => a.number - b.number)
                .map(c => [c.id, `${c.number}. ${c.name}`])
        ]);
        this.readEditor = () => ({
            document: {
                ...this.document,
                selection: {
                    operating: mode.value,
                    bank_id: banks.value === '' ? null : number(banks, 'Bank'),
                    channel_id: channels.value === '' ? null : number(channels, 'Channel')
                }
            }
        });
    }
}
