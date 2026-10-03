# Web companion

Static Web Serial companion for the C62: live LVGL display/virtual keys,
offline/connected CPS and separate CSK bootloader firmware tools.

- [Normative protocol](../protocol/companion/README.md)
- [Bundled bootloader helper attribution](bootloader/README.md)

Build output is self-contained in `companion/dist/`; serve it over HTTPS or
localhost. Firmware and companion must have an exact release identity match for
live UI/connected CPS. Bootloader tools and offline editing work independently.
