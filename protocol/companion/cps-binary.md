# CPS binary payload version 1

This is the normative complete-codeplug encoding for the companion protocol's CPS capability.
JSON schema 1 remains file interchange. Firmware need not parse JSON. Binary
v1 reuses the explicitly serialized HTDB record layout below, with **wire
generation always 1**. RAM/storage revisions are transfer metadata, independent
of this binary version, JSON schema and application protocol version. Changes
to this layout require paired definition/implementation/fixture updates.

All numbers are unsigned LE; booleans are exactly 0/1. Strings are printable
ASCII (callsigns use their narrower JSON alphabet), NUL terminated and zero
padded to the specified fixed byte length. Unknown enums/versions, nonzero
reserved/padding, CRC errors, bad counts/IDs/references, inactive mode fields,
truncation or trailing bytes are rejected. Storage-specific palette fallback
does not apply to CPS. Settings ranges and semantic
validation follow the [codeplug JSON schema](../../schemas/codeplug-v1.schema.json)
and [semantic validation](../../tools/codeplug.py).

A payload concatenates one manifest, its channels in manifest ID order, then
its banks in manifest ID order. Record-array order carries no additional
meaning; ordered bank membership is preserved. Browser writes sort channels
by number and banks by ID. Radio reads may use the owner's existing record order.
The maximum is `1187 + 256*87 + 16*1071 = 40595` bytes.

Each record starts with:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | ASCII HTDB |
| 4 | 1 | Record version 1 for manifest, channel and bank |
| 5 | 1 | Manifest kind 1, channel kind 2, bank kind 3 |
| 6 | 2 | Total record length including 16-byte header |
| 8 | 4 | Wire generation 1 |
| 12 | 4 | CRC-32/ISO-HDLC over header bytes 0..11 followed by payload; CRC field excluded |

Manifest payload, in order: channel high-water:u32, bank high-water:u32,
channel count:u16 (0..256), bank count:u8 (0..16), global settings (19 bytes),
selection (9 bytes), VFO operating configuration (40 bytes), channel IDs:u32
times channel count, bank IDs:u32 times bank count, VFO step Hz:u32.
Its length is `99 + 4*(channel count + bank count)`.

Global settings in order: local callsign[10], gain:u8, transmit limit seconds:u16,
theme:u8 (midnight=0, nord=1, solarized-dark=2, darcula=3), contrast:u8
(normal=0, high=1, maximum=2), animations:bool, brightness percent:u8,
idle seconds:u8, dim percent:u8.

Selection in order: operating:u8 (VFO=0, memory=1), bank ID:u32, channel ID:u32.
Nullable JSON IDs encode as zero; committed entity IDs are positive.

Operating configuration in order: mode:u8 (FM=0, M17=1), bandwidth:u8
(narrow=0, wide=1), squelch:u8, TX inhibit:bool, RX Hz:u32, TX Hz:u32,
requested power mW:u32, RX tone (4 bytes), TX tone (4 bytes), destination:u8
(broadcast=0, station=1), CAN:u8, RX CAN check:bool, reserved zero:u8,
destination callsign[10], reserved zero:u16. A tone is kind:u8
(none=0, CTCSS=1, DCS=2), inverted:bool, value:u16 (CTCSS tenths Hz; DCS
9-bit numeric value, displayed in octal). Disabled tone value/inverted are zero.
FM's M17 fields are broadcast/zero/empty; M17's FM tones are disabled.

Channel payload: ID:u32, channel number:u16, name[25], operating configuration
(40 bytes). Total record length is 87 bytes.

Bank payload: ID:u32, name[25], membership count:u16, ordered member IDs:u32
times count. Total length is `47 + 4*count`, at most 1071 bytes.

`cps-fixtures.json` contains independent Python-produced complete payloads and
rejection vectors for the real C++/JavaScript codecs. Regenerate with
`python3 tests/companion/generate_cps_fixtures.py`; the paired conformance command
checks freshness and runs both codecs. Maximum capacity, field semantics and
canonical JSON round trips are included.
