# OpenSimGear gateway

USB gateway aggregating OpenSimGear peripherals across **five independent powered CAN FD ports**.

## Status

Repository scaffold initialized. Gateway MCU/platform, controller arrangement, package, board, and firmware target remain undecided. STM32G0B1 is the module baseline, not an approved five-port gateway implementation.

## Architecture baseline

- Each port is an independent linear CAN segment with its own controller/transceiver path. No passive five-way star or ring.
- Endpoint termination at the gateway and the far end of each populated segment. Do not connect a module between two gateway ports.
- Initial target: up to ten modules and 5 m total cable across all segments combined.
- One external 12 V supply with a separately protected feed per port; electrical limits remain to be measured.
- One USB connection exposes HID controllers for registered modules plus serial configuration/output access.
- Temporary module unplugging preserves controller slots; registration/layout changes deliberately re-enumerate USB with user notice.
- Direct module USB takes input priority. Stable unit identity preserves registration when moving between gateway ports.

## Layout

- [firmware/](firmware/README.md): gateway-specific firmware, board targets, and controller aggregation.
- [hardware/](hardware/README.md): mechanical editions, enclosures, and assembly.
- [electronics/](electronics/README.md): controller/transceiver paths, power distribution, and PCB designs.

Lite/Pro folders reserve a possible edition structure; no gateway edition, PCB revision, or hardware compatibility is implemented. Build, flashing, connectors/pinouts, update/recovery, and tested capacity will be documented after hardware selection.

## Related repositories

- [Protocol](https://github.com/opensimgear/protocol): shared definitions and compatibility rules.
- [Firmware common](https://github.com/opensimgear/firmware-common): reusable C implementation.
- [Firmware template](https://github.com/opensimgear/firmware-template): starting layout for new devices.
- [Cockpit](https://github.com/opensimgear/cockpit): desktop configuration application.
- [Website](https://github.com/opensimgear/opensimgear.github.io): system architecture and user documentation.

## Licensing

License selection is pending. No license grant is implied by this scaffold. Firmware, mechanical design, and electronics will each need documented license coverage before releases and any public USB identifier application.
