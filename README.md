# OpenSimGear device-gateway

STM32G0B1 USB-to-CAN FD gateway with **USB-C, two parallel 4P4C powered CAN sockets, and a centre-positive 5.5 × 2.1 mm 12 V power socket**. The two CAN sockets continue one linear bus; they are not separate controller channels. The gateway can sit at either end or in the middle of the chain.

## Implemented prototype

- C firmware for `gateway_g0b1cb_rev_a`, with USB HID and CDC, a unique MCU-derived USB serial, and active CAN FD at 500 kbit/s arbitration / 2 Mbit/s data.
- Discovery and node allocation for up to ten module identities. Permanent controller slots store each registered identity and its input profile; unplugging a module preserves its slot and emits configured neutral values.
- One HID interface with a joystick top-level collection and stable report ID for each registered module. Each profile supports up to eight 16-bit axes, four hats and 128 buttons. Host/game interpretation of multiple collections still needs physical testing.
- Bounded CAN queues, sequence/lease checks, direct USB input ownership, 250 ms input timeout, 1.5 s presence timeout, and bus-off recovery.
- CRC-protected registration stored in two independent flash pages with a commit marker written last. Explicit `apply` saves the pending registry and reconnects USB with the new layout.
- Editable KiCad schematic, PCB, exact-part BOM, local libraries, review and fabrication exports, and a Linux SocketCAN reference peer.

The generic device and gateway now share **private CAN FD protocol v2**, with
lossless profiles and acknowledgements after actual USB HID completion. Cockpit
desktop uses a separate correlated CDC v2 adapter to discover devices, stage
registration and explicitly apply the saved USB layout. Offline slots survive
disconnects and gateway restarts. Direct device USB takes ownership and leaves
the gateway slot neutral. The Pico simulator keeps its own protocol and adapter.
As of 2026-10-12, native behavior/journal tests, cross-firmware integration and
conformance checks pass, and the STM32 target cross-builds. Physical USB/CAN
acceptance remains pending. Device calibration/settings, outputs, firmware
updates and canonical Cyphal definitions remain future work. `libcanard` is
pinned scaffolding; the current wire path does not use Cyphal.

## Connectors and power

Both sockets use the same four contacts, numbered by the selected manufacturer's drawing:

| Contact | Signal |
| --- | --- |
| 1 | Protected BUS_12V |
| 2 | CAN_H |
| 3 | CAN_L |
| 4 | GND |

Use continuity-checked straight-through wiring with CAN_H/L on a qualified twisted pair. Ordinary reversed or coiled handset cords are not qualified CAN cables. Put 120 Ω across contacts 2/3 in a removable endpoint plug at each physical end of the chain; an inline gateway has no termination. A 4P4C jack is commonly called RJ9/RJ10/RJ22 rather than RJ11. This connector choice follows the requested four-position, four-contact standard.

One regulated external 12 V supply feeds each chain through protected gateway injection. USB powers the local gateway electronics and does not energize the chain. The provisional bench budget is **250 mA total for the gateway's 12 V branch and all modules**, at 20–40 °C, with a supply current limit of 0.5 A. The PCB's resettable fuse is not a precision current limiter. Cable resistance, thermal behaviour, short-circuit protection and hot-plug behaviour require measurement. Ten-module software capacity does not establish a ten-module power allowance.

Use one gateway per CAN/power chain. Multiple gateways connect independently to the host by USB; do not join their CAN or powered trunks. CAN and power pass-through are passive and continue through local MCU reset. Historical six-contact module boards have a different pinout and are incompatible with the new four-contact standard.

## Build and review

From `firmware/`, run the native checks without downloading dependencies:

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

For the embedded build, put Arm GNU Embedded tools on PATH and supply USB identifiers you are authorized to use:

```sh
cmake --preset gateway -DOSG_USB_VID=<your-VID> -DOSG_USB_PID=<your-PID>
cmake --build --preset gateway
```

ELF, BIN, HEX and MAP outputs are placed under `firmware/build/gateway/`. No command flashes hardware automatically. See the [firmware guide](firmware/README.md) for exact dependency pins, SWD flashing, recovery and CDC commands. See [CAN peer testing](tools/README.md) for an executable matching module peer.

- [PCB project, pin map and BOM](electronics/boards/gateway/rev-a/README.md)
- [Fabrication construction](electronics/boards/gateway/rev-a/FABRICATION.md)
- [Electrical/static validation](electronics/boards/gateway/rev-a/VALIDATION.md)
- [Firmware validation results](firmware/VALIDATION.md)
- [CAN application framing](firmware/docs/can-fd-v2.md)
- [Registration, HID layout and storage semantics](firmware/docs/core.md)
- [Assembly and enclosure constraints](hardware/README.md)

Static checks and cross-compilation establish source-level behaviour. No gateway PCB has been assembled, flashed or electrically measured; fabrication review and physical USB/CAN/power acceptance remain required. The initial network validation target is up to ten modules and 5 m total cable with typical links below 1 m, subject to the power budget established by measurement.

## Related repositories and licensing

[Protocol](https://github.com/opensimgear/protocol), [firmware-common](https://github.com/opensimgear/firmware-common), [device-template](https://github.com/opensimgear/device-template), [Cockpit](https://github.com/opensimgear/cockpit), and the [website](https://github.com/opensimgear/opensimgear.github.io) own their respective shared definitions and implementations. This gateway uses pinned common firmware dependencies; the private bring-up protocol does not assign identifiers in the canonical protocol repository.

License selection for new OpenSimGear firmware and electronics remains pending. Included KiCad library data retains its [upstream license](electronics/boards/gateway/rev-a/LICENSE-KICAD.md), and firmware dependencies retain their upstream notices. No public USB VID/PID allocation is claimed.
