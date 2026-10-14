# OpenSimGear device-gateway

STM32G0B1 USB-to-CAN FD gateway with **USB-C, two parallel 4P4C powered CAN sockets, and a centre-positive 5.5 × 2.1 mm 12 V power socket**. The two CAN sockets continue one linear bus; they are not separate controller channels. The gateway can sit at either end or in the middle of the chain.

## Implemented prototype

- C firmware for `gateway_g0b1cb_v0_1_0`, with USB HID and CDC, a unique MCU-derived USB serial, and active CAN FD at 500 kbit/s arbitration / 2 Mbit/s data.
- Automatic discovery, enrollment and logical route allocation for up to 32 remembered module identities. New input identities append to a persistent ordered controller list; output-only identities retain guarded routes without a HID collection or report ID; enabled offline devices keep neutral HID collections.
- One HID interface with a joystick top-level collection and stable report ID for each enabled module. Each profile supports up to eight 16-bit axes, four hats and 128 buttons. Host/game interpretation of multiple collections still needs physical testing.
- Bounded CAN queues, sequence/lease checks, direct USB input ownership, 250 ms input timeout, 1.5 s presence timeout, and bus-off recovery.
- CRC-protected device visibility and controller order stored in two independent flash pages with a commit marker written last. Automatic batched activation saves an exact generation before USB reconnects; explicit Disable/Enable/Remove retain or forget identities as requested.
- Editable KiCad schematic, PCB, exact-part BOM, local libraries, review and fabrication exports, and a Linux SocketCAN reference peer.

The generic device and gateway now share **Cyphal/CAN**, using generated OpenSimGear
DSDL types and the actual libcanard 5.0 transport. Standard Heartbeat and GetInfo,
CAN node-ID autoconfiguration, transfer-ID deduplication and multi-frame reassembly
run in both firmwares. CAN addresses are separate from the gateway's 32 logical
routes, persistent descriptor order and explicit report IDs. Input acknowledgements still follow actual USB
HID completion. The prior private CAN FD v1/v2 frames are incompatible and rejected.

Cockpit desktop uses the correlated CDC v3 adapter with `canProtocol=3`.
Enrollment runs in the gateway with Cockpit closed. Enabled devices remain neutral
controllers while temporarily offline and appear ghosted in Cockpit. Disable
removes a Windows HID collection while remembering the device and its disabled
choice; repeated announcements and reboot never undo it. Enable appends at the
end. Remove forgets an offline identity; rediscovery enrolls it as new. Removing
published collections warns that remaining game bindings may need updating.

USB exposes only enabled controllers, including their retained offline ghosts.
There is no permanent management collection or unused joystick placeholder;
zero enabled controllers means CDC-only enumeration. Existing controller order,
report IDs and input definitions survive automatic additions, but Windows/game
binding continuity still requires physical qualification, especially transitions
between one and multiple collections. Software resource bounds are explicit,
and excess announcements produce visible capacity diagnostics. Direct device
USB ownership leaves gateway input neutral. The [Pico debug gateway](simulation/pico/README.md)
reuses current gateway/device contracts for eight virtual devices, Cockpit input
controls, physical USB HID, persistent settings and DCS-BIOS panel testing. Older
protocol-1 Pico images retain their compatibility adapter.

Cockpit can claim a module configuration draft, change input mappings/calibration,
and apply or persist its settings over the current identity/session/lease-bound
CAN route. Enabled HID profiles remain frozen until an explicit update-profile
operation; disabled devices remain configurable. Native behavior/journal tests,
actual transport integration, independent gateway/device acceptance and the
STM32 cross-build validate source behavior. Physical USB/CAN acceptance remains
pending. See the [architecture record](https://github.com/opensimgear/opensimgear.github.io/blob/main/src/content/docs/gear/architecture/usb-hid.md)
and [GitHub project](https://github.com/orgs/opensimgear/projects/2).

Cockpit also discovers and invokes device-advertised actions through the guarded
CDC/CAN route. Descriptions, invocations and readable state reuse bounded request/reply
fragments, with separate action dispatch and `targetAction` results. Actions remain
reachable on online disabled controllers and direct-USB input owners after current
lease status arrives. LED setters are volatile and do not claim settings, write flash
or change HID descriptors. The gateway forwards action catalogues without retaining
them in registry storage. Per-endpoint claims/renewal/owned invocation/release
use these same routes and prevent competing clients at the device. Independent
device expiry or route loss applies its validated fallback; tokens remain private.
Output-only modules enroll with `outputOnly:true` and `reportId:null`; Enable
returns `input_not_supported`. Unchanged descriptors activate without USB
reconnection. See the [ownership contract](https://github.com/opensimgear/protocol/blob/main/serial/output-ownership.md). See [milestone 1](https://github.com/opensimgear/cockpit/issues/13)
and the [action contract](https://github.com/opensimgear/protocol/blob/main/serial/device-actions.md).

The same guarded request/reply transport also carries `events state` and
`events read <stream> <after>`, returning request-scoped `targetEvents` JSON.
Devices retain ordered debounced button/encoder transitions in their own bounded
log; the gateway forwards pages independently of HID enablement or direct USB
ownership. Consumers baseline before forwarding new presses and recover from
explicit stream/gap errors. See the [panel event contract](https://github.com/opensimgear/protocol/blob/main/serial/panel-input-events.md).

Radio panels reuse these same routes: display actions declare fitted geometry and
encoding, and event snapshots add maintained/selector metadata plus independent
calibrated axes and availability. The gateway forwards these bounded JSON fields
without storing output state or inferring it from HID. The starter has no fitted
display; native radio frequency/preset/text ports execute the same product callbacks
that a physical display driver supplies. See the [radio simulation](https://github.com/opensimgear/device-template/blob/main/simulation/configs/radio-panel.json).

## Independent simulation

From this repository, launch the native gateway:

```sh
python3 simulation/run.py --identity 0123456789abcdef01234567
```

Start Cockpit normally, then start two or three device processes from
[device-template/simulation](https://github.com/opensimgear/device-template/tree/main/simulation),
using the same gateway identity and separate JSON definitions. Devices may start
before or after the gateway. Cockpit discovers the ordinary gateway endpoint;
it does not start or configure simulation processes. The gateway owns discovery,
automatic enrollment, configuration forwarding and dynamic HID reports. Closing Cockpit
leaves the processes running. See [the simulation guide](simulation/README.md)
for state, diagnostics and the optional [Pico implementation](simulation/pico/README.md).

Run integration tests from this repository alone:

```sh
python3 tests/e2e/run.py --sanitize
python3 tests/e2e/run.py --sanitize --with-cockpit
```

The runner clones and initializes required repositories, builds their components,
and tests three independently configured devices. `--with-cockpit` also clones
Cockpit and tests its normal Rust adapters. Everything stays under this repository’s
ignored `.e2e/`; no sibling checkout or parent workspace setup is required. See the
[E2E guide](tests/e2e/README.md) for source revisions and local development overrides.
Standalone simulation builds and state stay in `.simulation/`.

The native simulation generates production HID descriptors and input reports,
with HID delivery completed inside the process. It does not publish devices to
the host operating system, so simulated controllers do not appear in `hidutil list`.

## Connectors and power

Both sockets use the same four contacts, numbered by the selected manufacturer's drawing:

| Contact | Signal            |
| ------- | ----------------- |
| 1       | Protected BUS_12V |
| 2       | CAN_H             |
| 3       | CAN_L             |
| 4       | GND               |

Use continuity-checked straight-through wiring with CAN_H/L on a qualified twisted pair. Ordinary reversed or coiled handset cords are not qualified CAN cables. Put 120 Ω across contacts 2/3 in a removable endpoint plug at each physical end of the chain; an inline gateway has no termination. A 4P4C jack is commonly called RJ9/RJ10/RJ22 rather than RJ11. This connector choice follows the requested four-position, four-contact standard.

One regulated external 12 V supply feeds each chain through protected gateway injection. USB powers the local gateway electronics and does not energize the chain. The provisional bench budget is **250 mA total for the gateway's 12 V branch and all modules**, at 20–40 °C, with a supply current limit of 0.5 A. The PCB's resettable fuse is not a precision current limiter. Cable resistance, thermal behaviour, short-circuit protection and hot-plug behaviour require measurement. The 32-identity software bound does not establish a 32-module power allowance.

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

ELF, BIN, HEX and MAP outputs are placed under `.build/gateway/`. No command flashes hardware automatically. See the [firmware guide](firmware/README.md) for exact dependency pins, SWD flashing, recovery and CDC commands. See [CAN peer testing](tools/README.md) for an executable matching module peer.

- [PCB project, pin map and BOM](electronics/boards/gateway/0.1.0/README.md)
- [Fabrication construction](electronics/boards/gateway/0.1.0/FABRICATION.md)
- [Electrical/static validation](electronics/boards/gateway/0.1.0/VALIDATION.md)
- [Firmware validation results](firmware/VALIDATION.md)
- [CAN application framing](firmware/docs/cyphal.md)
- [Registration, HID layout and storage semantics](firmware/docs/core.md)
- [Assembly and enclosure constraints](hardware/README.md)

Static checks and cross-compilation establish source-level behaviour. No gateway PCB has been assembled, flashed or electrically measured; fabrication review and physical USB/CAN/power acceptance remain required. The initial network validation target is up to ten modules and 5 m total cable with typical links below 1 m, subject to the power budget established by measurement.

## Related repositories and licensing

[Protocol](https://github.com/opensimgear/protocol), [firmware-common](https://github.com/opensimgear/firmware-common), [device-template](https://github.com/opensimgear/device-template), [Cockpit](https://github.com/opensimgear/cockpit), and the [website](https://github.com/opensimgear/opensimgear.github.io) own their respective shared definitions and implementations. This gateway uses pinned common firmware dependencies; a checked-in shared Cyphal snapshot tracks the canonical protocol repository.

No license grant is implied for newly authored OpenSimGear firmware and electronics. Included KiCad library data retains its [upstream license](electronics/boards/gateway/0.1.0/LICENSE-KICAD.md), and firmware dependencies retain their upstream notices. No public USB VID/PID allocation is claimed.

PCB version **0.1.0** stays fixed while pre-live; see [PCB versioning](electronics/VERSIONING.md).

## JLCPCB exports

From this repository root, run `sh tools/bundle-jlcpcb.sh` for the latest numeric board version, or `sh tools/bundle-jlcpcb.sh 0.1.0` for a specific version. Requires KiCad 10 (`kicad-cli`) and Python 3. The script generates a Gerber ZIP plus separate JLCPCB BOM/CPL CSVs in the workspace `.research/jlcpcb/` directory. Use `--output-dir path` to change the destination or `--smd-only` to omit through-hole assembly. See [tool usage](tools/README.md) and the board’s fabrication notes before ordering.
