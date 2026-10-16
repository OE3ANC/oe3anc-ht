# Serial KISS modem

This interface follows the [KISS specification](https://www.ka9q.net/papers/kiss.html).
It is separate from the paired companion protocol. Only one protocol owns the
serial port at a time. Serial framing is 115200 baud, 8 data bits, no parity,
one stop bit, no hardware flow control.

## Initial scope

The initial modulation is Bell 202 AFSK, 1200 bits/s, mark 1200 Hz and space
2200 Hz, with AX.25 HDLC framing, NRZI, bit stuffing and CRC-16/X-25. KISS host
frames omit flags and FCS; received frames are delivered only after FCS validation.
The modem does not implement AX.25 connection management or packet retries.
Port 0 is the only supported port. AX.25 payloads are limited to 330 bytes;
this is narrower than the original KISS recommendation of at least 1024 bytes.
Oversized frames and queue overflow are dropped without replacing older packets.

Transmit preambles always contain at least two HDLC flags (13.3 ms), including
when TXDELAY is zero, to allow initial receiver clock acquisition. External
bursts with only one opening flag may fail cold acquisition.

Commands are data (0), TXDELAY (1), persistence (2), slot time (3), TX tail (4),
full duplex (5), and return (0xff). Timing values are in 10 ms units. Commands
1–5 have one parameter byte. Unsupported commands/ports are ignored. KISS has
no command or packet acknowledgements. Return is the exact frame C0 FF C0.
The radio hardware remains half duplex; the full-duplex setting only bypasses
carrier deferral, not the hardware limitation on simultaneous receive/transmit.

FEND is C0 and FESC is DB; escaped values are DB DC for C0 and DB DD for DB.
Frames have leading and trailing FEND. Empty frames are ignored. Malformed or
oversized input is discarded until the next delimiter. Serial transport errors
invalidate the current frame. No received host text is interpreted as commands.

## Ownership and radio operation

KISS is disabled after reboot. Enabling it suspends companion parsing and cancels
companion sessions. Disabling it restores companion protocol discovery; an old
companion session is not resumed. The current FM channel supplies frequencies,
power and transmit restrictions. A queued packet is authorization to transmit
subject to controller admission, carrier access and the radio's transmit limits.

Physical PTT exits KISS, discards queued packet work and stops packet audio before
normal voice transmission. Exit, power-off, faults and configuration changes must
invalidate old packet work. They must not depend on ordinary queue capacity.
No queued packet resumes after returning to voice. The physical menu and the KISS
return command provide exit paths.

## Verification boundary

Automated tests establish framing, codec behavior on synthetic signals, ownership
and cancellation logic. They do not establish RF deviation, hardware timing or
external-client interoperability. The user performs those checks on the radio.
The companion APRS composer targets the [APRS 1.0.1 reference](https://www.aprs.org/doc/APRS101.PDF):
uncompressed position reports and text messages, with no automatic beacons,
message acknowledgement handling or retries.
