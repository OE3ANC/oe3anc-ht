<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# KISS1200 / APRS bench handoff

Hardware, RF and external-client acceptance remain **pending user testing**.
Automated codec/state tests and builds do not verify those properties. The
coordinator supplies the matching generated firmware/companion packages, exact
revision, release identity and checksums separately with the handoff.

## Prepare and connect

Use that matching pair with the normal [firmware update workflow](../README.md#quick-start),
including the flash backup and update acknowledgements. Keep its application/DSP
pair together. Do not substitute the current hosted companion for the supplied
version. Follow the [companion serving instructions](../companion/README.md): the
static site needs HTTPS or localhost and a desktop browser with Web Serial.
For a local build, run this from the repository root and open
<http://localhost:8000>:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory companion/dist
```

For a supplied companion archive, serve its extracted directory containing
`index.html` instead of `companion/dist`.

1. Reboot: KISS starts **off**. Select the intended **FM channel** and verify its
   RX/TX frequencies, bandwidth, power and transmit restrictions. The modem uses
   this channel; it does not select a separate APRS frequency. Use receive-only
   operation first, then a permitted, controlled bench setup for TX checks.
2. Enable AFSK1200 in the radio's **Modem / KISS** menu. Alternatively, connect
   the matching companion with **Connect radio**, select **Modem**, then
   **Enter KISS from companion**. Entry closes the companion session.
3. For direct access after enabling KISS on the radio, use **Connect already in
   KISS** or an external serial KISS client: **115200 baud, 8N1, no flow control,
   port 0**. Only one of the companion, KISS client or firmware flasher may own
   the serial port. Stop sending during ownership changes and wait at least
   50 ms before starting the new protocol.

The modem is 1200-bit/s Bell 202 AFSK AX.25. KISS payloads omit HDLC flags and
FCS and are limited to 330 bytes. RX audio is muted. Defaults are TX delay
300 ms, persistence 63, slot time 100 ms, tail 0, carrier bypass off and TX level
37%. Adjust TX level against measured RF deviation; the default is not a
hardware calibration. See the [protocol definition](../protocol/kiss/README.md)
for framing, timing and limits.

## Send, exit and reconnect

In **Modem**, compose one APRS uncompressed position or text message using your
callsign/SSID, destination and optional path. Inspect the raw/decoded history.
Sending is explicit: there are no automatic beacons, retries, message IDs or
ACK manager. **Sent to modem** confirms only a browser serial write, not RF
transmission or receipt. KISS supplies no delivery acknowledgement or mode detection.

Physical PTT exits KISS, cancels queued/active packet work and returns to voice;
release must unkey normally. Channel/settings changes also clear pending work.
No old packet should resume. After either event, close the client port and
reconnect explicitly. To leave normally, use the radio menu, KISS Return
(`C0 FF C0`), or **Exit KISS & close** in the companion. Then **Connect radio**
starts fresh companion discovery and verifies its release match. Closing a port
alone does not establish that the radio has left KISS.

## Manual acceptance checklist

Mark each item pass, fail or not tested; retain observations and packet captures.

- [ ] **Known AX.25 RX:** Receive an independently generated packet; compare raw
  payload, source, destination and path. Check normal and weaker signals, recording
  signal/deviation and channel settings. External bursts need sufficient preamble;
  one opening HDLC flag may fail cold acquisition.
- [ ] **Known AX.25 TX and level:** Send a known packet to an independent TNC;
  confirm payload and valid FCS there. Measure RF deviation and set/record TX
  level with appropriate equipment. Record delay/tail and bandwidth/filter settings.
- [ ] **Busy channel:** With carrier bypass off, queued traffic waits while the
  channel is busy and is sent after carrier access permits. Check bypass separately
  if used; it bypasses deferral but the radio remains half duplex.
- [ ] **Physical PTT cancellation:** Test with a packet queued, during packet TX
  and with the four-entry host queue full. PTT must stop packet work and return to
  voice; release unkeys. No discarded packet resumes after re-entering KISS.
- [ ] **Other cancellation paths:** Repeat queued/active cancellation using menu
  exit and KISS Return. Change channel/mode/settings and confirm old work cannot
  transmit on the new configuration. Verify power-off stops RF; record fault
  shutdown only if safely exercised, otherwise mark it not tested. Reboot leaves
  KISS off. Check ordinary FM/M17 voice operation afterward.
- [ ] **Serial ownership:** Test companion entry, direct KISS connection, return
  to companion and explicit disconnect/reconnect after PTT/configuration changes.
  Confirm clients/flasher do not hold the port simultaneously.
- [ ] **APRS:** Send a fixed known position and text message with known callsigns
  and path; inspect encoded bytes and independent station decoding. Confirm there
  is no automatic beacon, retry or acknowledgement handling. Do not count the
  browser's write status as an RF pass.

Record firmware revision/release identity, companion identity, client/browser and
versions, independent TNC, channel RX/TX frequencies, bandwidth/filter, power,
TX level, timing/persistence/bypass settings, test result and observed RF/PTT
behavior. Unperformed checks remain pending; emulator results cannot replace them.
