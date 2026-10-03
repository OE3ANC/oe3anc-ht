# Companion application protocol 1.0

This document and `contract.json` are the normative definition. `fixtures.json`
contains frozen examples used by both codecs. Generated constants belong to both
applications; regenerate with `python3 tools/companion_contract.py`, and check
freshness with `--check`. Any interface change updates this definition, both
implementations, fixtures and version together. Additive changes increment minor;
incompatible changes increment major. This initial pair requires exact major,
minor and release identity agreement.

Implemented scope: identification, session establishment, keepalive, close and
complete CPS transfer, live UI snapshots, ordinary virtual keys and remote PTT. Capability bit 0
(`CAP_CPS = 1`) advertises connected CPS when both settings and UI owners are
present; bit 1 (`CAP_UI_SNAPSHOT = 2`) advertises UI snapshots when the UI owner
is present; bit 2 (`CAP_UI_KEYS = 4`) advertises ordinary front keys and bit 3
(`CAP_PTT = 8`) advertises controller-owned remote PTT. Other bits are zero.
This is the public protocol baseline. Development-era protocol pairs and file
formats are unsupported; use firmware and companion from the same public release.
CSK bootloader traffic is a separate protocol and must never use this envelope.

## Wire envelope

UART is 115200 baud, 8 data bits, no parity, one stop bit, no flow control.
Integers are unsigned little-endian. Encode the following bytes using COBS and
send `00 + COBS(raw) + 00`. Empty delimiters are ignored. No native structs are
sent. The raw frame is at most 216 bytes, its COBS body at most 217 bytes and the
complete wire frame at most 219 bytes, below the backend's 256-byte write limit.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | Magic ASCII `HT` (`48 54`) |
| 2 | 1 | Protocol major, currently 1 |
| 3 | 1 | Protocol minor, currently 0 |
| 4 | 1 | Message ID |
| 5 | 1 | Flags: request 0, response 1; other values invalid |
| 6 | 2 | Payload length, 0..192 |
| 8 | 4 | Request ID; requests require 1..4294967295 |
| 12 | 8 | Session correlation nonce; zero before handshake |
| 20 | length | Payload |
| 20 + length | 4 | CRC-32/ISO-HDLC of preceding raw bytes |

CRC uses reflected polynomial `0xedb88320`, initial value `0xffffffff`, final
XOR `0xffffffff`; `123456789` gives `0xcbf43926`. Reject malformed COBS, magic,
length, flags or CRC without exposing partial payloads or replying. A partial
frame expires 500 ms after its first body byte. Oversize or expired input is
discarded through the next zero. Every outgoing frame has a leading zero so a
failed write's prefix cannot poison the next complete frame. UART errors reset
the parser and session. Mode off also resets both. Cable loss does not exit the
radio's locally enabled companion mode or restore physical PTT.

Responses use this pair's major/minor, echo message and request IDs and normally
echo session. The browser accepts only its outstanding request's response and
requires exact protocol versions for established sessions. During HELLO only,
it accepts a correlated bounded rejection from another protocol version to show
an immediate compatibility error, then closes without creating a session.
Short HELLO errors are handled before parsing extended identity/limits fields.
Unknown messages are never interpreted.

## Messages and payloads

IDs and status numbers are in `contract.json`. All replies start with one status
byte. Errors are exactly one byte; successful HELLO/CPS/UI replies have the layouts
specified below. Errors with malformed HELLO requests or protocol version mismatch also
use the short status reply.

**HELLO (1)** requires header session zero. Request payload is `release length:
u8`, that many printable ASCII bytes (`0x21..0x7e`, length 1..96), then `nonce:
u64`. The browser generates a fresh nonzero random nonce for every connection.
Reply payload for a well-formed version-matched HELLO is `status: u8`, `firmware
release length: u8`, release bytes, `target length: u8`, target ASCII bytes (1..8),
`capabilities: u32`, `max payload: u16`, `lease milliseconds: u16`. Targets are
`c62` or `emulator`. Limits are 192 and 1000; capability bit 0 is CPS and bit 1
is read-only UI snapshots, bit 2 is virtual front keys, bit 3 is remote PTT.

Release comparison is exact. On success the reply header session is the nonce,
the request ID establishes ordering and the lease starts. A release mismatch
returns the radio's required identity with MISMATCH and header session zero.
An identical retry of the last successful HELLO returns its cached reply without
extending its lease. A different well-formed HELLO replaces the current session;
a nonce equal to the most recently ended session is rejected with SESSION.
An expired session must reconnect with a fresh nonce. Nonces correlate messages;
this local serial connection provides no cryptographic authentication or replay
protection across reboot. Do not expose it as an authenticated network service.

**PING (2)** requires the active session and an empty payload. A new valid PING
extends the lease to 1000 ms from acceptance. An identical retry returns the
cached reply without extending the lease. The browser schedules a PING every
250 ms while idle; it permits only one outstanding request.

**CLOSE (3)** requires the active session and an empty payload. It returns OK and
immediately clears the session. Retransmitting CLOSE after closure returns
SESSION. Browser disconnect cancels serial streams and lets the radio lease
expire; it does not acknowledge the radio's local unplug/Done exit.

Non-HELLO requests require the active nonce and increasing request IDs. For the
last accepted ID, exactly matching message/payload returns the cached reply;
changed message/payload or older IDs return STALE. A new authenticated request
with invalid payload or unknown message consumes its ID and caches its error.
A protocol-version error or invalid session does not consume an ID. Reconnect
before the 32-bit request counter would wrap. The browser retries the identical
frame once after 250 ms and fails after a total of 500 ms. After session expiry
it must reconnect rather than replaying old mutations. New product requests that
return OK renew the lease; retries/errors do not. This permits paced multi-second
transfers without heartbeat interleaving. The browser suspends idle PINGs while
a transfer runs, waiting for any already outstanding PING before starting it.

| Status | Meaning |
| --- | --- |
| OK (0) | Accepted |
| INVALID (1) | Invalid payload or configured identity |
| MISMATCH (2) | Protocol or exact release mismatch |
| SESSION (3) | No active matching session, or reused last nonce |
| UNSUPPORTED (4) | Unknown message |
| STALE (5) | Old ID or changed contents for the last ID |
| BUSY (6) | Transfer/owner/local UI conflict or transient state change |
| FAILED (7) | RF/storage/other operation failure |
| PROTECTED (8) | Read-only protected settings store |
| CANCELLED (9) | Owner operation cancelled |

## Complete CPS transfer

The [CPS binary v1 definition](cps-binary.md) and `cps-fixtures.json` define the
complete payload. It is at most 40,595 bytes, including every bank containing
all 256 channels. The 192-byte frame limit stays unchanged; chunks carry at most
180 data bytes. Only one CPS transfer/baseline/result is retained per session.
All integers below are unsigned LE unless explicitly a status/flags byte.
Transfer tokens are the nonzero request IDs that begin the corresponding read
or write, scoped to the active session. A new request ID is used for each chunk
and status poll; retries must replay the exact frame. Cached chunk/COMMIT replies
do not advance a cursor or submit another settings operation.

| Message | Request payload | Successful response after status byte |
| --- | --- | --- |
| CPS_READ (16) | Empty | token:u32, RAM revision:u32, radio lifecycle:u32, radio config revision:u32, byte length:u32, complete CRC32:u32, storage generation:u32, flags:u8, save status:u8 |
| CPS_READ_CHUNK (17) | token:u32, offset:u32, count:u16 | token:u32, offset:u32, exactly count bytes |
| CPS_WRITE (18) | completed read token:u32, byte length:u32, complete CRC32:u32 | new write token:u32 |
| CPS_WRITE_CHUNK (19) | write token:u32, offset:u32, 1..180 bytes | No additional fields |
| CPS_COMMIT (20) | write token:u32 | write token:u32 |
| CPS_STATUS (21) | accepted write token:u32 | token:u32, state:u8, applied RAM revision:u32, storage generation:u32, operation status:u8, save status:u8 |
| CPS_CANCEL (22) | read/write token:u32 | No additional fields |

Read flags are bit 0 RAM data has a pending save, bit 1 store is protected;
other bits are invalid. Save/operation status bytes use the status table above,
never platform errno numbers. State IDs are ACCEPTED=1, APPLIED=2, DURABLE=3,
FAILED=4. In ACCEPTED, application is pending and applied revision is zero.
FAILED carries a nonzero operation status and zero applied revision. APPLIED
and DURABLE carry a positive applied revision and operation status OK. APPLIED
may carry a save failure or OK while waiting; DURABLE carries save status OK.

READ copies all applied settings and their RAM revision/status under one owner
lock. It also records stable controller lifecycle/configuration revisions,
rejecting a change across capture as BUSY. Chunks are read in strictly contiguous
offset order starting at zero, with positive count not exceeding the remaining
length. The last chunk creates a completed read baseline. Local editing during
read does not change the copied bytes/CRC or mix revisions. The reader validates
the complete CRC and binary codeplug before offering it for editing.

WRITE requires that completed baseline and rejects changes to RAM or controller
revisions as STALE. It consumes the baseline, creates a new token and stages the
replacement outside the applied database. Upload chunks use strictly contiguous
offsets starting at zero; wrong token/order is STALE, malformed/oversize chunks
are INVALID. An error never advances/overwrites the cursor. COMMIT requires all
declared bytes, matching complete CRC and fully valid binary/structural data.
CRC/decode or owner-submission failure discards the transaction. A rejected write
must start with a fresh complete read. Reading again replaces an old completed
read/finished result; an incomplete upload or still-pending accepted application
reports BUSY and must be cancelled/completed first.

COMMIT rejects open local editors/dialogs and pending local UI operations.
Owner-side validation covers every target configuration, stale RAM revision,
RX state and protected stores. The controller checks lifecycle/configuration
revision at execution. The complete RAM replacement publishes only after its
successful retained acknowledgement, preserving the maximum destination/import
ID counters. Ordinary UI keys report BUSY during this short accepted application
window; PTT/release/monitor/power/fault paths continue independently. Nothing
silently closes a local draft to permit a remote write.

An OK COMMIT means complete operation-slot acceptance. Poll STATUS with new
request IDs for application/durability, using a retained replacement result that
subsequent local operation IDs cannot overwrite. APPLIED data may have an
unsaved/failed store; visible storage publication may advance generation without
establishing durability. DURABLE means a successful complete owner save at/after
the applied revision; later ordinary local edits can be included in that save.
It does not claim the radio has remained unchanged since application.

CANCEL discards an incomplete read/completed baseline/upload, leaving applied
settings untouched. Accepted COMMITs return BUSY to cancellation and may settle
after cable loss or session expiry. Expiry, new HELLO, UART errors or mode-off
discard all partial transfers/baselines; they do not undo a complete accepted
write or restore physical PTT. A lost COMMIT/status reply therefore has uncertain
completion until a fresh read. The browser preserves its draft, offers export,
shows uncertainty and requires a fresh radio read before retrying. Before/after
JSON review and explicit confirmation precede each browser write.

## Read-only UI snapshots

The [UI snapshot schema 1 definition](ui-snapshot.md) and `ui-fixtures.json`
define the bounded presentation payload and transfer lifecycle. UI_POLL (32)
and UI_CHUNK (33) use the same exact paired session, ordering, retry and lease
rules. No UI navigation or RF command is permitted by this capability.

## Virtual front keys

Messages require `CAP_UI_KEYS`, the exact paired session and ordinary request
ordering/retry rules. Successful replies are exactly one OK status byte. An OK
press means copied FIFO acceptance, not proof of navigation or durable saving.
The radio owns all guards, drafts, wake/lock behavior and command execution.
Poll a fresh snapshot after acknowledgement; never predict the action locally.

| Message | Request |
| --- | --- |
| UI_KEY (34) | key:u8, pressed:u8 (0 release, 1 press) |
| UI_KEYS_CLEAR (35) | Empty |
| UI_KEYS_KEEP (36) | held mask:u32 |

Key IDs in `contract.json`: Up 0, Down 1, P1/Left 2, P2/Right 3, OK 4,
Back 5, Star 6, Hash 7, digits 0..9 are IDs 8..17. No literal text, PTT,
monitor, power or quit command exists here. Unknown IDs/levels/mask bits/lengths
are INVALID. Digits use the radio's physical multi-tap/numeric behavior.

Physical and virtual ordinary presses enter one 16-entry copied UI FIFO in
successful enqueue order. A full FIFO returns BUSY without changing the held
state. One press per held key is accepted: another press without release is
INVALID. Neither firmware nor browser auto-repeats ordinary keys. Browser OS
repeat is ignored; repeated taps require releases. Producer uptime preserves
multi-tap timing. Existing shared drafts, Cancel/Save and wake-only first input
remain authoritative. Independent local RF/monitor/shutdown paths bypass this
queue. Every remote key is ignored on the radio's local unplug/Done page,
including OK: remote input cannot acknowledge disconnection or restore PTT pins.

Accepted presses start a separate 500 ms virtual-key lease. KEEP requires a
positive mask exactly matching the radio's held keys and renews it for 500 ms;
browser renewal is scheduled every 150 ms while held. Poll/PING/CPS never renew
this lease. A stale KEEP returns STALE and cannot reassert a held key. Release
is idempotent, bypasses FIFO capacity and clears its held bit/Star level; it
does not discard already accepted taps or extend their expiry. CLEAR bypasses
capacity, releases every remote key and invalidates unapplied remote presses.
Session replacement, close, lease expiry, UART error and mode-off do the same.
Local entries survive all remote cancellation. After expiry no old press is
applied; a freshly accepted press establishes a new input generation. Generations
never wrap; exhaustion disables remote admission with STALE until reboot.

The UI owner independently expires remote Star before evaluating its one-second
lock gesture, even if the serial worker is stalled. A physical Star level cannot
be overwritten by virtual release, and matrix scanning cannot overwrite virtual
Star. Simultaneous physical and virtual Star cancels the combined lock gesture
until both release and a fresh press occurs. Other front presses and existing
PTT/fault/wake guards cancel it as before. Input dequeue is the owner acceptance
point; cancellation does not roll back an action already consumed by the owner.

The browser disables controls until a matching session and a snapshot younger
than 500 ms are available. Blur, leaving keypad focus, pointer cancellation,
hidden tabs, stale/paused display and foreground CPS work cancel virtual keys.
Cleanup requests wait for any current request/transfer; radio expiry remains
independent. Reconnect requires fresh input. Keyboard shortcuts are scoped to
the focused keypad, preserving typing in the offline codeplug editor.

## Remote PTT

`CAP_PTT` enables a separate controller-owned companion source. Physical PTT
remains suppressed throughout Companion mode. Audio uses the radio microphone
and existing FM/M17 path; the browser sends no audio. All request/session/retry
rules above apply, including cached retries without renewal.

| Message | Request | Successful response after status |
| --- | --- | --- |
| PTT_PRESS (48) | Empty | press token:u32 (the PRESS request ID) |
| PTT_KEEP (49) | press token:u32, nonzero | None |
| PTT_RELEASE (50) | Empty | None |

PRESS accepts one hold only in active receiving Companion mode, with valid TX
configuration, no TX inhibit and no fault. Busy controller/preparation/drain,
diagnostics, or an already held source returns BUSY; absent/old source session
returns STALE; invalid M17 callsign returns INVALID; power/fault/inhibit returns
FAILED. Acceptance is intent admission, not proof of RF keying. The sole radio
owner validates again and publishes actual TX/error in its normal presentation.
A successful PRESS interrupts local monitor and UI drafts/gestures using the
shared PTT activity observation. A held monitor cannot reactivate until released
and pressed again. Normal TX limits still require PTT release followed by a
fresh PRESS even when KEEP continues.

PRESS starts an independent 500 ms RF lease. A fresh KEEP with the exact press
token renews only that unexpired hold for 500 ms. Wrong token, old session,
released or expired holds return STALE; KEEP cannot reassert TX. Clients send KEEP
every 150 ms while held. PING, UI_KEY/KEEP, snapshots and CPS never renew RF.
Radio owner and preparation/drain checks observe expiry without serial-worker
or browser cleanup. Release bypasses ordinary queue/state-mutex capacity and is
idempotent within the active session. A normal M17 release may send its end
marker only within the remaining RF lease and normal transmit deadline; expiry
forces PA/audio stop without classifying expiry as a hardware fault. Session
replacement/close/UART error/mode loss and power/fault shorten the deadline
immediately, including during termination. Existing hardware shutdown bounds
still apply; physical RF/audio timing requires the C62 acceptance bench.

The bundled web companion does not expose remote PTT or send PRESS, KEEP or
RELEASE requests. Its virtual keypad sends ordinary front-panel keys only.
The protocol retains these messages and the independent radio safety paths.

`ptt-fixtures.json` freezes payload shapes used by both sides. Native tests
exercise the actual independent radio intent and controller paths, including
expired/duplicate requests, queues, faults, power, limits and monitor arbitration.

## Pairing and checks

Development builds use `dev-` plus the first 32 hexadecimal characters of a
SHA-256 fingerprint of the shared source snapshot. Firmware and web builds must
be rebuilt after source changes. Tagged builds use `vX.Y.Z@<full Git commit>`
(optionally `-rc.N` on the tag), verify the tag identifies HEAD and require a
clean checkout. Target dependencies remain pinned by the existing build helper.
The archived-companion link extracts a validated version tag and uses the site's
configured hosting base path. Immutable archive assembly and deployment use
[release packaging](../../tools/release_packages.py) and the
[Pages workflow](../../.github/workflows/pages.yml).

Run `python3 tests/companion/check.py --node /path/to/node` for both codecs against
shared fixtures, recovery checks, C++ session semantics and mocked Web Serial
retry/unplug/reconnect behavior. Build both targets with `tools/build.py` and the
website with `companion/build.py`. The emulator backend currently has no serial
byte transport; these host checks exercise the actual codec/session core without
claiming electrical or on-radio timing validation.
