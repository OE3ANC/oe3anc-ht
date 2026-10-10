# Web companion

Static Web Serial companion for the C62: live LVGL display/virtual keys,
offline/connected CPS and separate CSK bootloader firmware tools.

- [Normative protocol](../protocol/companion/README.md)
- [Bundled bootloader helper attribution](bootloader/README.md)

Build output is self-contained in `companion/dist/`; serve it over HTTPS or
localhost. Firmware and companion must have an exact release identity match for
live UI/connected CPS. Bootloader tools and offline editing work independently.

The cassette-inspired shell uses warm instrument panels, orange controls and dark
display fields, with system fonts and CSS only (no external assets).

The Channels, Radio and Firmware tabs show one workspace at a time. Connection
status stays visible. Tab switching preserves editor drafts and ongoing transfers;
leaving Radio pauses live polling and releases virtual keys. Use Left/Right or
Home/End on the tab bar, or link directly to `#cps`, `#live-ui` or `#firmware-tools`.
Setup details are expandable, with flash risks and destructive confirmations kept
visible in Firmware. Reset and full restore are in the Recovery disclosure.
Tagged companions link directly to their matching GitHub firmware release;
development previews link to the releases list. A version mismatch also links
to the release required by the connected radio.

Review and write is available as soon as a compatible radio is connected. If no
radio read is available for that session, it reads automatically for the
before/after comparison without replacing the local draft. Upload still requires
explicit confirmation; protected stores and stale writes remain rejected.
