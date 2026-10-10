# Read-only UI snapshots, schema 1

This document, `contract.json` and `ui-fixtures.json` are normative for companion
protocol 1.0. The payload is presentation data for the existing shared LVGL
renderer. It contains no native structs, pointers, LVGL objects, radio commands
or whole codeplug. Integers are unsigned little-endian; fields have no padding.
Unknown schema, enum, flags, invalid length/range and trailing bytes are rejected
before updating the visible display. The radio owns navigation and drafts.

## Transfer and freshness

Both messages require the exact release/session match and `CAP_UI_SNAPSHOT`.
Errors are exactly one status byte with the protocol's status values.

| Message | Request | Successful reply, including status |
| --- | --- | --- |
| UI_POLL (32) | known revision:u32; zero forces a complete snapshot | Unchanged: OK:u8, changed=0:u8, revision:u32 (6 bytes). Changed: OK:u8, changed=1:u8, revision:u32, token:u32, length:u16, CRC32:u32 (16 bytes). |
| UI_CHUNK (33) | token:u32, offset:u16, count:u8 | OK:u8, token:u32, offset:u16, exactly count data bytes |

The UI owner captures and serializes one coherent presentation under its model
mutex. It publishes a positive revision, incremented only when the canonical
visible bytes change; inactive drafts and struct padding cannot cause changes.
Revisions persist across sessions, reset on reboot, and fail rather than wrap.
Each successful publication refreshes its timestamp, even when bytes are unchanged.
An unavailable first publication/temporarily locked owner returns BUSY. A copy
at least 500 ms old returns STALE; encoding/internal failures return FAILED.
The serial worker uses a nonblocking owner copy and never touches live LVGL.

Every valid new POLL discards the prior incomplete UI read. When revision differs,
it freezes one complete copy, maximum 512 bytes, with CRC-32/ISO-HDLC and token
equal to the POLL request ID. When unchanged, no read token is established.
Chunks are contiguous starting at zero, with count 1..180 not exceeding the
remaining length. Wrong token/order is STALE; malformed/oversize count is INVALID.
Errors do not advance the cursor. The last chunk ends the read; the session's
identical last-request cache permits its retry without advancing again.
Publishing later UI changes cannot alter the frozen transfer. Next POLL, session
expiry/new HELLO, UART failure or mode-off discards it. There is no UI mutation
to roll back and no separate CANCEL message; pausing stops polling and the next
POLL supersedes any partial read. UI and CPS buffers have independent bounded
ownership, and a UI read cannot invalidate a CPS baseline.

The browser applies only a complete CRC-checked and decoded snapshot. It rejects
results after a changed session or a transfer taking at least 500 ms from POLL
start through complete reception, including unchanged replies. On failure it
marks the canvas stale, freezes animation and clears its known revision to force
a complete retry. New sessions/Resume also request revision zero. Normal polling
waits 200 ms after completion. Unchanged replies confirm freshness of the retained
display. Browser foreground CPS waits for an in-flight snapshot and reserves the
next serial slot; further polls pause during the pending/running CPS operation.
These are starting rates, requiring physical UART/latency acceptance on C62.

## Encoding

Each string is `length:u8` followed by that many printable ASCII bytes
(`0x20..0x7e`). Length zero is allowed. NUL, non-ASCII and control bytes are
invalid. Limits below are maximum **characters**, excluding the length byte.
The 20-byte common header is followed by four action strings (31 characters
each), the visible layout's strings in the listed order, then its scalar fields.
Omitted internal fields are reset to their default values by the renderer decoder.

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Schema, exactly 1 |
| 1 | u8 | Screen ID from `ui_screens` in `contract.json` (0..28) |
| 2 | u8 | Theme: Midnight=0, Nord=1, SolarizedDark=2, Darcula=3 |
| 3 | u8 | Contrast: Normal=0, High=1, Maximum=2 |
| 4 | u16 | Flags below |
| 6 | u8 | Form cursor, 0..3 |
| 7 | u8 | List-return screen ID, 0..28 |
| 8 | u32 | Animation interruption sequence |
| 12 | u32 | PTT presentation sequence |
| 16 | u32 | Monitor presentation sequence |

Flags: bit 0 motion, 1 error, 2 system-visible, 3 home-visible, 4 home transmitting,
5 home locked, 6 diagnostic editing, 7 quick controls available, 8 text multi-tap
pending, 9 preference animations enabled. Other bits are invalid. Motion requires
animations enabled; system-visible and home-visible are mutually exclusive.
Home transmitting/locked require the Home layout; diagnostic editing requires
Diagnostics; quick availability requires QuickControls; text pending requires
Text layout and a positive cursor. Encoder normalizes irrelevant flags to zero.
Sequence values affect rendering only and grant no input/RF permission.

Layout selection has priority: system-visible → System; home-visible → Home;
otherwise select by screen: Menu → Menu; Status/CompanionExit → Status;
Appearance → Appearance; Diagnostics → Hex if editing, otherwise List;
Frequency/Callsign/ChannelNumber/ChannelField/BankName → Text;
Channels/Banks/ChannelBank/BankMembers/BankAdd → List;
remaining ChannelEditor..ChannelSaved and BankEditor..BankSaved screens, plus
VfoStep/QuickControls/Backlight/TransmitLimit → Form; otherwise None.
Numeric screen IDs are frozen in the machine-readable contract; generation
checks their agreement with the firmware's `UiScreen` enum.

| Layout | Strings, in order (character limits) | Following scalar fields |
| --- | --- | --- |
| System, Status | title(24), detail(31), four rows(31 each) | color:u8 |
| Home | identity(15), name(24), context(31), frequency(15), settings(31), activity(31), battery(11), mode(3) | status:u8, contextColor:u8, batteryColor:u8, bars:u8 |
| Menu, List | list title(24), detail(31), four rows each name(24)/prefix(4)/suffix(4), line 6(31) | cursor:u16, count:u16, ready:u8 |
| Hex | list title(24), detail(31), four rows each name(24)/prefix(4)/suffix(4), diagnostic text(4) | cursor:u16, count:u16, ready:u8 |
| Appearance | line 1(31), line 6(31) | None |
| Text | line 1(31), line 6(31), line 0(31), line 4(31), text value(24) | text kind:u8, cursor:u8 |
| Form | line 1(31), line 6(31), line 0(31), lines 2/3/4/5(31 each) | None |
| None | None | None |

Color/status values are Muted=0, Accent=1, Amber=2, Red=3. Home mode is exactly
`FM` or `M17`; bars are 0..5. List count is 0..256; cursor must be below a
positive count, or zero for count zero. Ready is exactly 0 or 1. Text kinds are
Name=0, Callsign=1, Frequency=2, ChannelNumber=3 with value limits 24/9/11/3;
cursor is 0..value length. This exposes a copied shared draft rather than an
independently editable browser field. Maximum-length List encoding is 382 bytes,
within the 512-byte bound. Four visible rows and copied footer/help lines
preserve scroll position and pending/error explanations without exporting data
for other pages. Brightness/idle settings are represented by visible lines;
browser rendering does not control radio backlight.

Codec2-mod timing/memory statistics use the existing Status strings. M17 reception
quality uses the Home context string, with storage/radio explanations taking
priority. These are rendered text, not new numeric protocol fields; schema and
protocol versions remain unchanged. BK4819 RX register pages also use the existing
Status strings, including sample age and unavailable/paused explanations. The
browser renders these radio-owned observations without reading or writing
registers. Shared fixtures cover these presentations.

Temporary FM RX test controls appear as ordinary Menu rows and use the existing
virtual keys. Firmware owns range validation, FM/RX-only application and the
reboot-scoped values; the companion never applies register writes independently.
No screen IDs, payload fields, limits or codeplug formats change.

Requested power is displayed in watts in radio presentation strings and the CPS
editor (for example, `2.5 W`). CPS converts decimal watts to integer milliwatts;
the stored and binary `power_mw` fields retain their existing units and limits.
