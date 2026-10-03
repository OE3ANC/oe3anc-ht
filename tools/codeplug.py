#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate and canonicalize the version 1 codeplug JSON contract."""
import argparse
import json
from pathlib import Path
import re
import sys

FORMAT = 'oe3anc-ht-codeplug'
SCHEMA_VERSION = 1
VFO_STEPS_HZ = (1000, 2500, 5000, 6250, 10000, 12500, 20000, 25000, 50000, 100000)
MAX_BYTES = 512 * 1024
MAX_ID = 0xFFFFFFFF
MAX_CHANNELS = 256
MAX_BANKS = 16
BANDS = ((136000000, 174000000), (400000000, 480000000))


class InvalidCodeplug(ValueError):
    pass


def fail(path, reason):
    raise InvalidCodeplug(f'{path}: {reason}')


def fields(value, names, path):
    if type(value) is not dict:
        fail(path, 'expected an object')
    expected = set(names.split())
    if set(value) != expected:
        missing, extra = expected - set(value), set(value) - expected
        fail(path, f'missing fields {sorted(missing)}; unknown fields {sorted(extra)}')


def integer(value, low, high, path):
    if type(value) is not int or not low <= value <= high:
        fail(path, f'expected integer {low}..{high}')


def choice(value, options, path):
    if type(value) is not type(options[0]) or value not in options:
        fail(path, f'expected one of {options}')


def boolean(value, path):
    if type(value) is not bool:
        fail(path, 'expected a boolean')


def name(value, path):
    if (
        type(value) is not str
        or not 1 <= len(value) <= 24
        or not all(' ' <= c <= '~' for c in value)
    ):
        fail(path, 'expected 1..24 printable ASCII characters')


def callsign(value, path, allow_empty=False):
    if allow_empty and value == '':
        return
    if (
        type(value) is not str
        or not re.fullmatch(r'[A-Z0-9./-]{1,9}', value)
        or value in ('ALL', 'INVALID')
    ):
        fail(path, 'expected a local/station callsign (1..9 uppercase radio characters)')


def tone(value, path):
    if type(value) is not dict:
        fail(path, 'expected a tone object')
    kind = value.get('kind')
    choice(kind, ('none', 'ctcss', 'dcs'), path + '.kind')
    fields(
        value, {'none': 'kind', 'ctcss': 'kind tenths_hz', 'dcs': 'kind code polarity'}[kind], path
    )
    if kind == 'ctcss':
        integer(value['tenths_hz'], 670, 2541, path + '.tenths_hz')
    if kind == 'dcs':
        if type(value['code']) is not str or not re.fullmatch(r'[0-7]{3}', value['code']):
            fail(path + '.code', 'expected exactly three octal digits, e.g. "023"')
        choice(value['polarity'], ('normal', 'inverted'), path + '.polarity')


def configuration(value, path):
    if type(value) is not dict:
        fail(path, 'expected an operating configuration')
    mode = value.get('mode')
    choice(mode, ('fm', 'm17'), path + '.mode')
    fields(
        value,
        'mode rx_frequency_hz tx_frequency_hz tx_inhibit power_mw bandwidth squelch ' + mode,
        path,
    )
    for key in ('rx_frequency_hz', 'tx_frequency_hz'):
        hz = value[key]
        integer(hz, 1, MAX_ID, path + '.' + key)
        if not any(low <= hz <= high for low, high in BANDS):
            fail(path + '.' + key, 'frequency outside C62/emulator supported bands')
    boolean(value['tx_inhibit'], path + '.tx_inhibit')
    integer(value['power_mw'], 1, 5000, path + '.power_mw')
    choice(value['bandwidth'], ('narrow', 'wide'), path + '.bandwidth')
    integer(value['squelch'], 0, 15, path + '.squelch')
    specific, subpath = value[mode], path + '.' + mode
    if mode == 'fm':
        fields(specific, 'rx_tone tx_tone', subpath)
        tone(specific['rx_tone'], subpath + '.rx_tone')
        tone(specific['tx_tone'], subpath + '.tx_tone')
    else:
        fields(specific, 'destination can rx_can_check', subpath)
        integer(specific['can'], 0, 15, subpath + '.can')
        boolean(specific['rx_can_check'], subpath + '.rx_can_check')
        destination = specific['destination']
        if type(destination) is not dict:
            fail(subpath + '.destination', 'expected an explicit destination object')
        kind = destination.get('kind')
        choice(kind, ('broadcast', 'station'), subpath + '.destination.kind')
        fields(
            destination, 'kind callsign' if kind == 'station' else 'kind', subpath + '.destination'
        )
        if kind == 'station':
            callsign(destination['callsign'], subpath + '.destination.callsign')


def records(value, limit, path):
    if type(value) is not list or len(value) > limit:
        fail(path, f'expected an array with at most {limit} items')


def validate(document):
    """Validate the complete contract without mutating any supplied data."""
    fields(document, 'format schema_version allocation global vfo selection channels banks', '$')
    choice(document['format'], (FORMAT,), '$.format')
    choice(document['schema_version'], (SCHEMA_VERSION,), '$.schema_version')
    allocation = document['allocation']
    fields(allocation, 'channel_id_high_water bank_id_high_water', '$.allocation')
    for key, value in allocation.items():
        integer(value, 0, MAX_ID, '$.allocation.' + key)
    global_settings = document['global']
    fields(
        global_settings,
        'local_callsign gain transmit_limit_s ui vfo_step_hz',
        '$.global',
    )
    choice(global_settings['vfo_step_hz'], VFO_STEPS_HZ, '$.global.vfo_step_hz')
    callsign(global_settings['local_callsign'], '$.global.local_callsign', allow_empty=True)
    integer(global_settings['gain'], 0, 15, '$.global.gain')
    choice(global_settings['transmit_limit_s'], (0, 60, 120, 180), '$.global.transmit_limit_s')
    ui = global_settings['ui']
    fields(ui, 'theme contrast animations backlight', '$.global.ui')
    choice(ui['theme'], ('midnight', 'nord', 'solarized-dark', 'darcula'), '$.global.ui.theme')
    choice(ui['contrast'], ('normal', 'high', 'maximum'), '$.global.ui.contrast')
    boolean(ui['animations'], '$.global.ui.animations')
    backlight = ui['backlight']
    fields(backlight, 'brightness_percent idle_s dim_percent', '$.global.ui.backlight')
    choice(
        backlight['brightness_percent'],
        (25, 50, 75, 100),
        '$.global.ui.backlight.brightness_percent',
    )
    choice(backlight['idle_s'], (0, 15, 30, 60), '$.global.ui.backlight.idle_s')
    choice(backlight['dim_percent'], (10, 20, 30), '$.global.ui.backlight.dim_percent')
    configuration(document['vfo'], '$.vfo')
    records(document['channels'], MAX_CHANNELS, '$.channels')
    channels, numbers = {}, set()
    for index, channel in enumerate(document['channels']):
        path = f'$.channels[{index}]'
        fields(channel, 'id number name configuration', path)
        integer(channel['id'], 1, allocation['channel_id_high_water'], path + '.id')
        integer(channel['number'], 1, MAX_CHANNELS, path + '.number')
        if channel['id'] in channels or channel['number'] in numbers:
            fail(path, 'duplicate channel ID or number')
        name(channel['name'], path + '.name')
        configuration(channel['configuration'], path + '.configuration')
        channels[channel['id']] = channel
        numbers.add(channel['number'])
    records(document['banks'], MAX_BANKS, '$.banks')
    banks = {}
    for index, bank in enumerate(document['banks']):
        path = f'$.banks[{index}]'
        fields(bank, 'id name channel_ids', path)
        integer(bank['id'], 1, allocation['bank_id_high_water'], path + '.id')
        if bank['id'] in banks:
            fail(path + '.id', 'duplicate bank ID')
        name(bank['name'], path + '.name')
        records(bank['channel_ids'], MAX_CHANNELS, path + '.channel_ids')
        seen = set()
        for member, channel_id in enumerate(bank['channel_ids']):
            member_path = f'{path}.channel_ids[{member}]'
            integer(channel_id, 1, MAX_ID, member_path)
            if channel_id not in channels or channel_id in seen:
                fail(member_path, 'missing or duplicate channel reference')
            seen.add(channel_id)
        banks[bank['id']] = bank
    selection = document['selection']
    fields(selection, 'operating bank_id channel_id', '$.selection')
    choice(selection['operating'], ('vfo', 'memory'), '$.selection.operating')
    for key, items in (('bank_id', banks), ('channel_id', channels)):
        value = selection[key]
        if value is not None:
            integer(value, 1, MAX_ID, '$.selection.' + key)
            if value not in items:
                fail('$.selection.' + key, 'missing object reference')
    if selection['operating'] == 'memory' and selection['channel_id'] is None:
        fail('$.selection.channel_id', 'memory operation requires a selected channel')
    if selection['bank_id'] is not None and selection['channel_id'] is not None:
        if selection['channel_id'] not in banks[selection['bank_id']]['channel_ids']:
            fail('$.selection.channel_id', 'selected channel is outside the selected bank')
    return document


def load(path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                fail('$', f'duplicate JSON key {key!r}')
            value[key] = item
        return value

    def constant(value):
        fail('$', f'non-JSON numeric constant {value}')

    with Path(path).open('rb') as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        fail('$', f'input exceeds {MAX_BYTES} bytes')
    try:
        document = json.loads(
            data.decode('utf-8'), object_pairs_hook=unique, parse_constant=constant
        )
    except (UnicodeError, ValueError, RecursionError) as error:
        raise InvalidCodeplug(f'JSON: {error}') from error
    return validate(document)


def canonical(document):
    document = validate(document)
    # Record order is not identity. Bank membership order remains significant.
    ordered = dict(document)
    ordered['channels'] = sorted(document['channels'], key=lambda channel: channel['number'])
    ordered['banks'] = sorted(document['banks'], key=lambda bank: bank['id'])
    return (
        json.dumps(ordered, ensure_ascii=True, sort_keys=True, separators=(',', ':')) + '\n'
    ).encode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('validate', 'canonicalize'))
    parser.add_argument('input', type=Path)
    parser.add_argument(
        '--output', type=Path, help='required for canonicalize; a JSON file, not a firmware profile'
    )
    args = parser.parse_args()
    if (args.action == 'canonicalize') != (args.output is not None):
        parser.error('canonicalize requires --output; validate accepts no output')
    try:
        document = load(args.input)
        encoded = canonical(document)
        if args.output is not None:
            args.output.write_bytes(encoded)
        print(
            f'Valid schema {SCHEMA_VERSION}: {len(document["channels"])} channels, '
            f'{len(document["banks"])} banks, {len(encoded)} canonical bytes'
        )
    except (OSError, InvalidCodeplug) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
