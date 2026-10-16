# Pico W firmware

## Development status and disclaimer

This project is heavily under development and is not ready for use. Use it entirely at your own risk. To the fullest extent permitted by applicable law, the authors and contributors disclaim all liability for any damage, loss, or other issues arising from its use.

See the [Pico debug guide](../README.md) for build, flash, Cockpit controls, DCS-BIOS and tests. `build.mjs` produces the current RP2040 Pico W protocol-3 debug UF2 under `.research/pico/build/`. It reuses production gateway and shared device logic for eight virtual devices, physical USB HID and persistent journals. `test.mjs` runs sanitized debug/storage tests and coverage gates, and preserves the older protocol-1 compatibility harness.
