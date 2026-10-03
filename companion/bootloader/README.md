# CSK6 RAM helper

`burner_venus.bin` is copied without modification from LISTENAI/cskburn commit
`df83dc2067a5f16c8b825a6e3ad4d2d6511de303`, path
`libcskburn_serial/burner_venus.bin`. It is 38,724 bytes, SHA-256
`f3aad327667b3e419bf4c1c90c1a0206a6d608a2d3633dbf6b54af8dc1796e65`.
The upstream Apache-2.0 license is retained in `LICENSE`; that revision has no
project NOTICE file. The static build distributes these files together.

The browser implementation follows that pin's `libcskburn_serial/src/cmd.c`
and `core.c` for CSK6/VENUS only: SLIP, little-endian command headers, RAM load
at address zero, 115200 baud and legacy 64-byte flash reads. It does not use the
VENUSA-only streaming read protocol. The helper is loaded into RAM, never into
flash. Its functionality and device-specific identity still require hardware
acceptance for this browser implementation.
