# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit HTDB manifest v2 / channel-bank v1 Linux-profile codec; no native struct layout is serialized."""
import json
import struct
import zlib

import codeplug

MAX_RECORD = 1188
MAX_PROFILE = 1188 + 256 * 87 + 16 * 1071


def string(value, size):
    encoded = value.encode('ascii')
    assert len(encoded) < size
    return encoded.ljust(size, b'\0')


def tone(value):
    kind = value['kind']
    data = value.get('tenths_hz', 0) if kind != 'dcs' else int(value['code'], 8)
    return struct.pack(
        '<BBH', ('none', 'ctcss', 'dcs').index(kind), value.get('polarity') == 'inverted', data
    )


def configuration(value):
    m17 = value.get('m17', {})
    destination = m17.get('destination', {'kind': 'broadcast'})
    fm = value.get('fm', {'rx_tone': {'kind': 'none'}, 'tx_tone': {'kind': 'none'}})
    return (
        struct.pack(
            '<BBBBIII',
            ('fm', 'm17').index(value['mode']),
            ('narrow', 'wide').index(value['bandwidth']),
            value['squelch'],
            value['tx_inhibit'],
            value['rx_frequency_hz'],
            value['tx_frequency_hz'],
            value['power_mw'],
        )
        + tone(fm['rx_tone'])
        + tone(fm['tx_tone'])
        + struct.pack(
            '<BBBB',
            destination['kind'] == 'station',
            m17.get('can', 0),
            m17.get('rx_can_check', False),
            0,
        )
        + string(destination.get('callsign', ''), 10)
        + b'\0\0'
    )


def envelope(kind, payload, generation=1):
    header = b'HTDB' + struct.pack('<BBHI', 2 if kind == 1 else 1, kind, len(payload) + 16, generation)
    return header + struct.pack('<I', zlib.crc32(header + payload)) + payload


def channel(value, generation=1):
    return envelope(
        2,
        struct.pack('<IH', value['id'], value['number'])
        + string(value['name'], 25)
        + configuration(value['configuration']),
        generation,
    )


def manifest(value, generation=1):
    g, s, a = value['global'], value['selection'], value['allocation']
    ui, light = g['ui'], g['ui']['backlight']
    payload = (
        struct.pack(
            '<IIHB',
            a['channel_id_high_water'],
            a['bank_id_high_water'],
            len(value['channels']),
            len(value['banks']),
        )
        + string(g['local_callsign'], 10)
        + struct.pack(
            '<BHBBBBBB',
            g['gain'],
            g['transmit_limit_s'],
            codeplug.THEMES.index(ui['theme']),
            ('normal', 'high', 'maximum').index(ui['contrast']),
            ui['animations'],
            light['brightness_percent'],
            light['idle_s'],
            light['dim_percent'],
        )
        + struct.pack('<BII', s['operating'] == 'memory', s['bank_id'] or 0, s['channel_id'] or 0)
        + configuration(value['vfo'])
    )
    for item in value['channels'] + value['banks']:
        payload += struct.pack('<I', item['id'])
    payload += struct.pack('<IB', g['vfo_step_hz'], g.get('fm_ctcss_level', 74))
    return envelope(1, payload, generation)


def bank(value, generation):
    members = value['channel_ids']
    payload = (
        struct.pack('<I', value['id'])
        + string(value['name'], 25)
        + struct.pack('<H', len(members))
        + b''.join(struct.pack('<I', member) for member in members)
    )
    return envelope(3, payload, generation)


def encode(document, generation):
    codeplug.integer(generation, 1, codeplug.MAX_ID, '$profile.generation')
    # Use the same deterministic record ordering as exported JSON, retaining
    # bank membership order and object IDs. Validation precedes any encoding.
    ordered = json.loads(codeplug.canonical(document))
    return (
        manifest(ordered, generation)
        + b''.join(channel(item, generation) for item in ordered['channels'])
        + b''.join(bank(item, generation) for item in ordered['banks'])
    )


class Reader:
    def __init__(self, data, label):
        self.data, self.label, self.position = data, label, 0

    def get(self, count):
        if self.position + count > len(self.data):
            codeplug.fail(self.label, f'truncated at byte {self.position}')
        result = self.data[self.position : self.position + count]
        self.position += count
        return result

    def unpack(self, layout):
        return struct.unpack(layout, self.get(struct.calcsize(layout)))

    def enum(self, choices, fallback=None):
        (value,) = self.unpack('<B')
        if value >= len(choices):
            if fallback is not None:
                return fallback
            codeplug.fail(self.label, f'unknown enum {value} at byte {self.position - 1}')
        return choices[value]

    def boolean(self):
        (value,) = self.unpack('<B')
        if value > 1:
            codeplug.fail(self.label, f'noncanonical boolean {value}')
        return bool(value)

    def string(self, size):
        data = self.get(size)
        end = data.find(b'\0')
        if end < 0 or any(data[end:]):
            codeplug.fail(self.label, 'noncanonical string/padding')
        try:
            return data[:end].decode('ascii')
        except UnicodeError as error:
            codeplug.fail(self.label, f'non-ASCII string: {error}')

    def zero(self, count):
        if any(self.get(count)):
            codeplug.fail(self.label, 'nonzero reserved bytes')

    def finish(self):
        if self.position != len(self.data):
            codeplug.fail(self.label, 'unexpected trailing bytes/records')

    def record(self, kind, generation=None):
        header = self.get(16)
        magic, version, found_kind, size, found_generation, checksum = struct.unpack(
            '<4sBBHII', header
        )
        if magic != b'HTDB' or not 16 <= size <= MAX_RECORD:
            codeplug.fail(self.label, 'bad record header/length')
        payload = self.get(size - 16)
        if zlib.crc32(header[:12] + payload) != checksum:
            codeplug.fail(self.label, 'record CRC mismatch')
        if version != 1 and not (kind == 1 and version == 2):
            codeplug.fail(self.label, f'unsupported binary version {version}')
        if found_kind != kind or not found_generation or generation not in (None, found_generation):
            codeplug.fail(self.label, 'record kind/generation mismatch')
        reader = Reader(payload, self.label + f'.record{kind}')
        reader.version = version
        return reader, found_generation


def read_tone(reader):
    kind = reader.enum(('none', 'ctcss', 'dcs'))
    inverted = reader.boolean()
    (value,) = reader.unpack('<H')
    if kind == 'none':
        if value or inverted:
            codeplug.fail(reader.label, 'noncanonical disabled tone')
        return {'kind': kind}
    if kind == 'ctcss':
        if inverted:
            codeplug.fail(reader.label, 'CTCSS inversion is invalid')
        return {'kind': kind, 'tenths_hz': value}
    return {'kind': kind, 'code': f'{value:03o}', 'polarity': 'inverted' if inverted else 'normal'}


def read_configuration(reader):
    mode = reader.enum(('fm', 'm17'))
    bandwidth = reader.enum(('narrow', 'wide'))
    (squelch,) = reader.unpack('<B')
    inhibit = reader.boolean()
    rx, tx, power = reader.unpack('<III')
    rx_tone, tx_tone = read_tone(reader), read_tone(reader)
    destination = reader.enum(('broadcast', 'station'))
    (can,) = reader.unpack('<B')
    can_check = reader.boolean()
    reader.zero(1)
    station = reader.string(10)
    reader.zero(2)
    if destination == 'broadcast' and station:
        codeplug.fail(reader.label, 'noncanonical broadcast station')
    result = dict(
        mode=mode,
        bandwidth=bandwidth,
        squelch=squelch,
        tx_inhibit=inhibit,
        rx_frequency_hz=rx,
        tx_frequency_hz=tx,
        power_mw=power,
    )
    if mode == 'fm':
        if destination != 'broadcast' or can or can_check:
            codeplug.fail(reader.label, 'inactive M17 fields must be defaults')
        result['fm'] = dict(rx_tone=rx_tone, tx_tone=tx_tone)
    else:
        if rx_tone['kind'] != 'none' or tx_tone['kind'] != 'none':
            codeplug.fail(reader.label, 'inactive FM tones must be disabled')
        address = {'kind': destination}
        if destination == 'station':
            address['callsign'] = station
        result['m17'] = dict(destination=address, can=can, rx_can_check=can_check)
    return result


def read_global(reader):
    local = reader.string(10)
    gain, limit = reader.unpack('<BH')
    theme = reader.enum(codeplug.THEMES, 'midnight')
    contrast = reader.enum(('normal', 'high', 'maximum'), 'normal')
    animations = reader.boolean()
    brightness, idle, dim = reader.unpack('<BBB')
    return dict(
        local_callsign=local,
        gain=gain,
        transmit_limit_s=limit,
        ui=dict(
            theme=theme,
            contrast=contrast,
            animations=animations,
            backlight=dict(brightness_percent=brightness, idle_s=idle, dim_percent=dim),
        ),
    )


def decode(data):
    if len(data) > MAX_PROFILE:
        codeplug.fail('$profile', f'input exceeds {MAX_PROFILE} bytes')
    records = Reader(data, '$profile')
    first, generation = records.record(1)
    channel_water, bank_water, channel_count, bank_count = first.unpack('<IIHB')
    if channel_count > 256 or bank_count > 16:
        codeplug.fail('$profile', 'record count exceeds capacity')
    global_settings = read_global(first)
    operating = first.enum(('vfo', 'memory'))
    bank_id, channel_id = first.unpack('<II')
    vfo = read_configuration(first)
    channel_ids = [first.unpack('<I')[0] for _ in range(channel_count)]
    bank_ids = [first.unpack('<I')[0] for _ in range(bank_count)]
    global_settings['vfo_step_hz'] = first.unpack('<I')[0]
    global_settings['fm_ctcss_level'] = first.unpack('<B')[0] if first.version == 2 else 74
    first.finish()
    channels, banks = [], []
    for index, expected_id in enumerate(channel_ids):
        item, _ = records.record(2, generation)
        record_id, number = item.unpack('<IH')
        name = item.string(25)
        config = read_configuration(item)
        item.finish()
        if record_id != expected_id:
            codeplug.fail(f'$profile.channels[{index}]', 'ID differs from manifest')
        channels.append(dict(id=record_id, number=number, name=name, configuration=config))
    for index, expected_id in enumerate(bank_ids):
        item, _ = records.record(3, generation)
        (record_id,) = item.unpack('<I')
        name = item.string(25)
        (count,) = item.unpack('<H')
        if count > 256:
            codeplug.fail(f'$profile.banks[{index}]', 'membership exceeds capacity')
        members = [item.unpack('<I')[0] for _ in range(count)]
        item.finish()
        if record_id != expected_id:
            codeplug.fail(f'$profile.banks[{index}]', 'ID differs from manifest')
        banks.append(dict(id=record_id, name=name, channel_ids=members))
    records.finish()
    result = dict(
        format=codeplug.FORMAT,
        schema_version=codeplug.SCHEMA_VERSION,
        allocation=dict(channel_id_high_water=channel_water, bank_id_high_water=bank_water),
        global_settings=global_settings,
        vfo=vfo,
        selection=dict(operating=operating, bank_id=bank_id or None, channel_id=channel_id or None),
        channels=channels,
        banks=banks,
    )
    result['global'] = result.pop('global_settings')
    return codeplug.validate(result), generation
