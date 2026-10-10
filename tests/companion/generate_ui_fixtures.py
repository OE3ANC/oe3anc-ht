#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent Python UI snapshot schema 1 examples for both codecs/renderers."""
import argparse
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
SCREENS = json.loads((ROOT / 'protocol/companion/contract.json').read_text())['ui_screens']


def kind(v):
    if v['flags'] & 4:
        return 'system'
    if v['flags'] & 8:
        return 'home'
    s = v['screen']
    if s == 2:
        return 'menu'
    if s in (27, 28):
        return 'status'
    if s == 5:
        return 'appearance'
    if s == 4:
        return 'hex' if v['flags'] & 64 else 'list'
    if s in (1, 3, 8, 10, 17):
        return 'text'
    if s in (6, 7, 12, 18, 19):
        return 'list'
    if 9 <= s <= 22 or s in (23, 24, 25, 26):
        return 'form'
    return 'none'


def encode(v):
    out = bytearray(
        struct.pack(
            '<BBBBHBBIII',
            1,
            v['screen'],
            v['theme'],
            v['contrast'],
            v['flags'],
            v['formCursor'],
            v['listReturn'],
            v['interruptions'],
            v['pttSequence'],
            v['monitorSequence'],
        )
    )

    def text(value):
        b = value.encode('ascii')
        out.append(len(b))
        out.extend(b)

    for action in v['actions']:
        text(action)
    k = kind(v)
    if k in ('system', 'status'):
        text(v['status']['title'])
        text(v['status']['detail'])
        for row in v['status']['rows']:
            text(row)
    elif k == 'home':
        for key in (
            'identity',
            'name',
            'context',
            'frequency',
            'settings',
            'activity',
            'battery',
            'mode',
        ):
            text(v['home'][key])
    elif k in ('menu', 'list', 'hex'):
        text(v['list']['title'])
        text(v['list']['detail'])
        for row in v['list']['rows']:
            for key in ('name', 'prefix', 'suffix'):
                text(row[key])
        text(v['diagnosticText'] if k == 'hex' else v['lines']['6'])
    elif k in ('appearance', 'text', 'form'):
        text(v['lines']['1'])
        text(v['lines']['6'])
        if k == 'text':
            text(v['lines']['0'])
            text(v['lines']['4'])
            text(v['text']['value'])
        elif k == 'form':
            text(v['lines']['0'])
            for i in range(2, 6):
                text(v['lines'][str(i)])
    if k in ('system', 'status'):
        out.append(v['status']['color'])
    if k == 'home':
        for key in ('status', 'contextColor', 'batteryColor', 'bars'):
            out.append(v['home'][key])
    if k in ('menu', 'list', 'hex'):
        out.extend(struct.pack('<HHB', v['list']['cursor'], v['list']['count'], v['list']['ready']))
    if k == 'text':
        out.extend(bytes([v['text']['kind'], v['text']['cursor']]))
    return out


def example(screen):
    v = dict(
        screen=screen,
        theme=screen % 4,
        contrast=screen % 3,
        flags=512,
        formCursor=screen % 4,
        listReturn=2,
        interruptions=1,
        pttSequence=2,
        monitorSequence=3,
        actions=['OK Apply', 'BACK Cancel', 'P1 Field', ''],
    )
    if screen == 0:
        v['flags'] |= 8
    if screen == 24:
        v['flags'] |= 128
    k = kind(v)
    if k == 'home':
        v['home'] = dict(
            identity='VFO',
            name='UHF SIMPLEX',
            context='MHz / direct tuning',
            frequency='433.500',
            settings='W SQL4 1 W',
            activity='Listening',
            battery='7.40V',
            mode='FM',
            status=0,
            contextColor=0,
            batteryColor=0,
            bars=4,
        )
        v['actions'] = ['OK Menu', 'BACK Quick', 'P1 Memory', 'P2 Save']
    elif k in ('menu', 'list', 'hex'):
        v['list'] = dict(
            title='MENU' if k == 'menu' else 'CHANNELS',
            detail='Current page',
            rows=[
                dict(name='Row ' + str(i + 1), prefix=f'{i+1:03}', suffix='FM') for i in range(4)
            ],
            cursor=5,
            count=8,
            ready=1,
        )
        if k == 'hex':
            v['diagnosticText'] = '1A2B'
        else:
            v['lines'] = {'6': ''}
    elif k in ('system', 'status'):
        v['status'] = dict(
            title='STATUS',
            detail='Controller state',
            rows=['Receiving', 'FM', 'No error', 'Saved'],
            color=0,
        )
    elif k in ('appearance', 'text', 'form'):
        v['lines'] = {'1': 'Draft / radio owned', '6': ''}
        if k == 'text':
            v['lines'].update({'0': 'EDIT FIELD', '4': 'Up/Down cursor'})
            tk = 2 if screen == 1 else 1 if screen == 3 else 3 if screen == 8 else 0
            value = ['BANK NAME', 'OE3ANC', '433.500001', '001'][tk]
            v['text'] = dict(value=value, kind=tk, cursor=len(value))
        elif k == 'form':
            v['lines']['0'] = 'EDIT SETTINGS'
            for i in range(2, 6):
                v['lines'][str(i)] = '  Field ' + str(i - 1)
    return v


def fixtures():
    valid = [dict(name=name.lower(), document=example(screen)) for name, screen in SCREENS.items()]
    extra = example(4)
    extra['flags'] |= 64
    extra['diagnosticText'] = '1A2B'
    extra.pop('lines')
    valid.append(dict(name='diagnostic-hex', document=extra))
    extra = example(3)
    extra['flags'] |= 256
    extra['text']['cursor'] = 3
    valid.append(dict(name='pending-multi-tap', document=extra))
    extra = example(0)
    extra['flags'] |= 4
    extra['flags'] &= ~8
    extra.pop('home')
    extra['status'] = dict(
        title='RADIO FAULT',
        detail='Backend failed',
        rows=['TX disabled', 'Reboot required', '', ''],
        color=3,
    )
    valid.append(dict(name='fault', document=extra))
    extra = example(0)
    extra['home'].update(
        frequency='145.000', mode='M17', context='BER~1.10% L2 B1',
        settings='CAN0 ALL', activity='RX OE3VOICE',
    )
    valid.append(dict(name='m17-quality', document=extra))
    extra = example(27)
    extra['status'].update(
        title='CODEC2 / 6 OF 9', detail='mod/3200/36600B/heap0',
        rows=['Enc avg/max 4.3/8.1 ms', 'Dec avg/max 9.6/12.1 ms',
              'Frames E100 D200', '>20ms E0 D1'],
    )
    extra['actions'] = ['OK Reset', 'BACK Menu', 'P1 Page', '']
    valid.append(dict(name='codec2-statistics', document=extra))
    extra = example(27)
    extra['status'].update(
        title='RX PATH / 7 OF 9', detail='RX age 200 ms',
        rows=['30:BFF1  33:0040', '38:04F0  39:00DE',
              '43:4068  47:6140', '48:B3C1  37:1F0F'],
    )
    valid.append(dict(name='rx-registers', document=extra))
    extra = example(2)
    extra['list'].update(
        detail='FM RX test / until reboot', cursor=19, count=23,
        rows=[dict(name=name, prefix='', suffix='') for name in
              ['Power: 1 W', 'RX tone: off', 'TX tone: off', 'FM weak BW: 4.00k']],
    )
    extra['actions'] = ['OK Change', 'BACK Home', 'P1 Prev', 'P2 Next']
    valid.append(dict(name='fm-rx-controls', document=extra))
    extra = example(6)
    extra['list'].update(title='T' * 24, detail='D' * 31, cursor=255, count=256)
    extra['list']['rows'] = [dict(name='N' * 24, prefix='P' * 4, suffix='S' * 4) for _ in range(4)]
    extra['actions'] = ['A' * 31] * 4
    extra['lines']['6'] = 'E' * 31
    valid.append(dict(name='maximum-list', document=extra))
    for item in valid:
        item['wire'] = encode(item['document']).hex()
    invalid = []
    seed = bytearray.fromhex(valid[0]['wire'])
    for name, index, value in [
        ('schema', 0, 2),
        ('screen', 1, 255),
        ('theme', 2, 4),
        ('contrast', 3, 3),
        ('reserved-flags', 5, 6),
        ('form-cursor', 6, 4),
        ('list-return', 7, 255),
        ('text-size', 20, 32),
        ('text-byte', 21, 0),
        ('bars', len(seed) - 1, 6),
        ('status-color', len(seed) - 4, 4),
        ('conflicting-priority', 4, 12),
    ]:
        bad = seed.copy()
        bad[index] = value
        invalid.append(dict(name=name, wire=bad.hex()))
    invalid += [
        dict(name='truncated', wire=seed[:-1].hex()),
        dict(name='trailing', wire=(seed + b'\0').hex()),
    ]
    for name, index, value in [('ready', -1, 2), ('list-count', -2, 2)]:
        bad = bytearray.fromhex(valid[-1]['wire'])
        bad[index] = value
        invalid.append(dict(name=name, wire=bad.hex()))
    text = bytearray.fromhex(valid[1]['wire'])
    text[-1] = 25
    invalid.append(dict(name='text-cursor', wire=text.hex()))
    return dict(valid=valid, invalid=invalid)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    path = ROOT / 'protocol/companion/ui-fixtures.json'
    content = json.dumps(fixtures(), indent=2) + '\n'
    if a.check:
        if not path.exists() or path.read_text() != content:
            raise SystemExit('Stale UI fixtures')
    else:
        path.write_text(content)


if __name__ == '__main__':
    main()
