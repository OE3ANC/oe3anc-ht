# Web companion

Static Web Serial companion for the C62: live LVGL display/virtual keys,
offline/connected CPS and separate CSK bootloader firmware tools.

- [Normative protocol](../protocol/companion/README.md)
- [Bundled bootloader helper attribution](bootloader/README.md)

Build output is self-contained in `companion/dist/`; serve it over HTTPS or
localhost. Firmware and companion must have an exact release identity match for
live UI/connected CPS. Bootloader tools and offline editing work independently.

The page groups codeplug file actions separately from radio transfers, places the
live display beside its keypad and keeps backup/update/calibration tools together
under Firmware tools. Reset and full restore are in the Recovery disclosure.
Tagged companions link directly to their matching GitHub firmware release;
development previews link to the releases list. A version mismatch also links
to the release required by the connected radio.
