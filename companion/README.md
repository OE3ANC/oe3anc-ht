# Web companion

Static Web Serial companion for the C62: live LVGL display/virtual keys,
offline/connected CPS, KISS/APRS and separate CSK bootloader firmware tools.

- [Normative protocol](../protocol/companion/README.md)
- [Bundled bootloader helper attribution](bootloader/README.md)

Build output is self-contained in `companion/dist/`; serve it over HTTPS or
localhost. Firmware and companion must have an exact release identity match for
live UI/connected CPS. Bootloader tools and offline editing work independently.

The companion uses a restrained plastic-console style: flat buttons matching the
selected tab, thin borders, faint highlights and recessed display glass. System fonts and CSS keep
the site self-contained. LVGL also offers Terminal Green, Terminal Amber and
Terminal Ice palettes on the radio and in the shared live renderer.
The header's Theme selector offers System (default), Light and Dark. The browser
remembers the last choice locally. System follows desktop/browser changes immediately;
an explicit Light or Dark choice overrides them. If browser storage is blocked,
switching still works for the current page.
The radio display keeps its selected LVGL palette independently.

The Channels, Radio, Modem and Firmware tabs show one workspace at a time. Connection
status stays visible. Tab switching preserves editor drafts and ongoing transfers;
leaving Radio pauses live polling and releases virtual keys. Use Left/Right or
Home/End on the tab bar, or link directly to `#cps`, `#live-ui`, `#modem` or `#firmware-tools`.
Setup details are expandable, with flash risks and destructive confirmations kept
visible in Firmware. Reset and full restore are in the Recovery disclosure.
Tagged companions link directly to their matching GitHub firmware release;
development previews link to the releases list. The automatically replaced
`/nightly/` companion shows a UTC build date and downloads its paired firmware
from `./firmware-nightly.zip`. A version mismatch also links
to the release required by the connected radio.

Review and write is available as soon as a compatible radio is connected. If no
radio read is available for that session, it reads automatically for the
before/after comparison without replacing the local draft. Upload still requires
explicit confirmation; protected stores and stale writes remain rejected.


In **Modem**, enter KISS from a matching connected radio, or select KISS on the
radio first and use **Connect already in KISS**. Start receive-only: connecting
sends no packets, and received AX.25/APRS records appear above the composer.
Received and sent histories have separate counts and retain the last 100 records
each. Supported uncompressed positions and text messages are readable alongside
raw bytes; other packet types remain raw. Preview positions/messages offline;
**Send once** uses the current form. Drafts survive disconnect/reconnect while
the page stays open. There are no automatic beacons, retries or acknowledgements.

The received-position map keeps the latest valid uncompressed, untimestamped APRS
position for up to 100 callsigns/SSIDs. Pan/zoom, **Fit stations** or a packet’s
**Map** button to inspect it. Station coordinates, comments and received times
work offline. **Enable online map** loads OpenStreetMap tiles over the internet,
sharing the viewed area and your IP address with OpenStreetMap. **Clear packets**
also clears positions; positions are not saved after the page closes.

An open serial port is not proof of radio KISS mode, RF transmission or delivery.
Physical PTT and channel/settings changes exit KISS; close the port and reconnect
explicitly afterward. **Exit KISS & close** sends return; **Connect radio** then
verifies companion mode. See the [RX bench steps](../docs/kiss-bench.md).
