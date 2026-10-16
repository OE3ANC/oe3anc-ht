# KISS modem implementation

Approved scope: 1200-baud Bell 202 AFSK AX.25 on the current FM channel; serial KISS replaces companion traffic while enabled; disabled after reboot; physical PTT exits KISS, cancels queued/audio packet work and returns to voice. Switching channel/settings or exiting discards pending packets. Controller owns all RF. Standard KISS timing/persistence controls. Companion KISS client with raw/decoded packet display and APRS position/message composer (callsign/SSID, destination, path). No automatic beacons/retries. User performs client/hardware interoperability tests later.

No DSP replacement, additional modulation implementation, network service, automatic flashing, push or release publication in this scope. Existing 48 kHz audio and pinned DSP retained.

| ID | Status | Deliverable and acceptance | Dependencies / evidence |
|---|---|---|---|
| M1 | DONE | Bounded portable KISS + AX.25 HDLC/AFSK core. Known CRC/framing vectors, malformed/oversize recovery, independently generated RX signals and timing variations; no allocation in audio processing. | Native regression + GCC14 ASan/UBSan pass; review_kiss_core: no blockers, including zero-delay preamble fix. build/kiss/m1-core-tests.log. |
| M2 | ACTIVE | Firmware serial ownership, packet queues/channel access, controller-owned TX/RX and physical PTT interruption; Modem menu/settings/status. Tests cover transitions, queue pressure, faults, cancellation and shared serial/PTT behavior. | M1 |
| M3 | READY | Companion KISS connection and APRS composer, AX.25 decoding; explicit switching and exit, paired protocol definition/fixtures where needed. Tests cover encoding, malformed inputs and connection lifecycle. | M2 |
| M4 | READY | Full paired release gate, independent review, focused fixes, committed candidate, matching firmware/companion package and manual bench checklist. | M1–M3; hardware/client verification explicitly pending. |

Each milestone requires independent review before commit. Coordinator owns this board/state, integration, commits and final verification; workers do not delegate or commit. New worker per milestone. No external-client or RF test is claimed from automated checks.
