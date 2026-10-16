# KISS modem implementation

Approved scope: 1200-baud Bell 202 AFSK AX.25 on the current FM channel; serial KISS replaces companion traffic while enabled; disabled after reboot; physical PTT exits KISS, cancels queued/audio packet work and returns to voice. Switching channel/settings or exiting discards pending packets. Controller owns all RF. Standard KISS timing/persistence controls. Companion KISS client with raw/decoded packet display and APRS position/message composer (callsign/SSID, destination, path). No automatic beacons/retries. User performs client/hardware interoperability tests later.

No DSP replacement, additional modulation implementation, network service, automatic flashing, push or release publication in this scope. Existing 48 kHz audio and pinned DSP retained.

| ID | Status | Deliverable and acceptance | Dependencies / evidence |
|---|---|---|---|
| M1 | DONE | Bounded portable KISS + AX.25 HDLC/AFSK core. Known CRC/framing vectors, malformed/oversize recovery, independently generated RX signals and timing variations; no allocation in audio processing. | Native regression + GCC14 ASan/UBSan pass; review_kiss_core: no blockers, including zero-delay preamble fix. build/kiss/m1-core-tests.log. |
| M2 | DONE | Firmware serial ownership, packet queues/channel access, controller-owned TX/RX and physical PTT interruption; Modem menu/settings/status. Tests cover transitions, queue pressure, faults, cancellation and shared serial/PTT behavior. | M1; full gate build/kiss/m2-full-gate-fixed.log PASS; independent review cleared ListenTx fix. |
| M3 | DONE | Companion KISS connection and APRS composer, AX.25 decoding; explicit switching and exit, paired protocol definition/fixtures where needed. Tests cover encoding, malformed inputs and connection lifecycle. | M2; full gate build/kiss/m3-full-gate.log PASS, independent review_kiss_companion no blockers, native13 and paired/browser checks pass. |
| M4 | DONE | Full paired release gate, independent review, focused fixes, committed candidate, matching firmware/companion package and manual bench checklist. | Full gate m3-full-gate.log PASS; independent code/doc reviews clear. Paired archives built and identity/hash/ZIP verified in build/kiss/handoff; metadata refreshed after final commit. Manual guide docs/kiss-bench.md. Hardware/client verification pending user. |

Each milestone requires independent review before commit. Coordinator owns this board/state, integration, commits and final verification; workers do not delegate or commit. New worker per milestone. No external-client or RF test is claimed from automated checks.

## RX verification and client cleanup

User reported successful TX decode on another HT after enabling Ignore carrier; hardware RX remains pending. Preserve standard KISS and explicit sends; no status extension, DSP changes, automatic retries or publication in this follow-up.

| ID | Status | Deliverable and acceptance | Dependencies / evidence |
|---|---|---|---|
| M5 | DONE | Trace and verify PCM-to-KISS RX with known packets; radio shows busy-channel/TX-inhibited/queued/TX states; companion separates received/sent records, decodes supported APRS positions/messages, previews packets before explicit send, preserves drafts across reconnect, and clearly distinguishes serial connection from radio mode. Bounded malformed-input tests, review, full paired gate. | Full gate build/kiss/m5-full-gate.log PASS; independent review clear; synthetic RX14/14, menu19/19 and browser preview/invalidation checks pass. Hardware RX pending user. |
| M6 | DONE | Final matching local packages, metadata/checksum verification and updated RX bench steps. | M5; build/kiss/rx-handoff paired archives passed identity/hash/ZIP checks; m6-builds.log builds-only pass, full M5 test results reused. Final commit metadata refreshed before handoff. |
