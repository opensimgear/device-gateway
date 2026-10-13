# STM32 CAN gateway firmware

The v0.1.0 target is `gateway_g0b1cb_v0_1_0`: STM32G0B1CBT6, USB-C device,
one CAN FD controller/transceiver, two parallel **4P4C** chain jacks, and a
center-positive 12 V barrel input. The jacks share one bus and carry
pin 1 BUS12V, pin 2 CANH, pin 3 CANL, pin 4 GND. Moving a module between the
jacks does not change its identity or controller slot.

The firmware builds a real STM32 ELF, binary and Intel HEX. It has active CAN FD,
USB HID plus CDC, discovery, bounded chain-local logical route allocation, an identity-based
registry, persistent storage, routed runtime module configuration and explicit USB layout application. It uses the
[Cyphal/CAN application contract](docs/cyphal.md), shared with the generic
module firmware and Cockpit’s native STM32 adapter. The Pico simulator remains
a separate transport and does not implement this console.
Electrical operation and host/game treatment of multiple HID collections still
require assembled-board validation.

## Build and tests

Native tests need CMake 3.24 or newer and a C11 compiler. They do not fetch any
external dependency or need an Arm compiler:

```sh
cd firmware
cmake --preset native
cmake --build --preset native --parallel
ctest --preset native
cd ..
OSG_GATEWAY_TEST=.build/native/osg_gateway_test python3 -m unittest discover -s tools -p 'test_*.py'
```

For the board build, put Arm GNU's `arm-none-eabi-*` tools on PATH. Supply USB
identifiers you are authorized to use; no VID/PID is allocated or silently chosen:

```sh
cd firmware
cmake --preset gateway -DOSG_USB_VID=<your-vid> -DOSG_USB_PID=<your-pid>
cmake --build --preset gateway --parallel
```

Outputs are under the repository `.build/gateway/` directory as
`osg_gateway.{elf,bin,hex,map}`. Flash the HEX with SWD
using a programmer configured for **STM32G0B1CB, 128 KiB flash**, preserving the
last two pages when retaining the registry. Never flash or distribute the
unallocated `0xFFFF:0xFFFF` identifiers used solely by CI compile checks.

The build pins firmware-common to commit
`35e06787fb9696977fe92d20c5ff5afc1e3e1ebf` for TinyUSB, ST HAL and CMSIS.
The maintained `shared-cyphal/` snapshot comes from canonical `protocol/cyphal/`
and supplies generated Nunavut bindings, OpenSimGear DSDL and pinned libcanard 5.0.
Both native tests and the board target link that actual transport. Dependency
clones remain under the build directory’s `_deps/`. See
[dependency configuration](cmake/README.md) for local source overrides and
[shared transport](shared-cyphal/README.md) for generation, pins and provenance.

## USB and registered controllers

The MCU's 96-bit factory UID is the stable 24-hex-character gateway identity and
USB serial number. The CDC `identity` response includes the discriminator
`gatewayType=stm32-can-gateway protocol=2 canProtocol=3 firmware=0.3.0`. Module identities are separate
16-byte values printed as 32 hexadecimal digits.

There are ten permanent registry slots, each retaining its module identity and
profile. One HID interface contains a standard joystick top-level application
collection per occupied slot; report ID is slot index plus one. Reports contain
up to 128 buttons, four packed four-bit hats and eight unsigned 16-bit axes.
Reports pack buttons first, then hats (8 means neutral), then little-endian axes. An empty registry has a vendor
management collection, report ID 11, and no joystick. HID descriptors change
only on explicit `apply` or reboot after `save`; discovery and temporary loss
do not re-enumerate USB. Registered modules that are absent, stale, directly USB
owned or profile mismatched produce their configured neutral values.

The single interface fits the STM32 full-speed endpoint budget. OS/game mapping
of multiple collections must be checked on Windows, Linux and macOS; this layout
is not equivalent to the Pico simulator's multiple HID interfaces. USB attaches
only while PB0 senses VBUS. Detach, reset, suspend and resume invalidate input;
a fresh CAN input sequence is required before active reports resume.

## CDC console

Use a serial terminal with newline-delimited commands. The line buffer is bounded
at 479 characters, with child configuration commands limited to 383 characters;
overlong lines are rejected. Replies are buffered without
blocking the CAN/USB service loop. Example registration flow:

```text
identity
inventory
register 00112233445566778899aabbccddeeff
registry
apply
```

| Command | Result |
| --- | --- |
| `identity`, `info`, `hello` | Stable gateway identity, board and protocol |
| `inventory` | Discovered identities, online/ownership/profile state, node and registry slots |
| `registry` | Pending and active identities for each permanent slot |
| `register <32hex>` | Stage the observed online module's identity and current profile |
| `unregister <32hex>` | Stage removal without renumbering other slots |
| `save` | Persist the pending registry; activate on `apply` or next boot |
| `apply` | Persist first, disconnect USB, activate pending layout, rebuild descriptor and reconnect after 250 ms |
| `diagnostics` | CAN errors/drop counts, queue depth, bus-off recovery and protocol counters |
| `help` | Command list |

Unregistered modules are visible in inventory but do not become HID controllers.
A registered module may be directly USB owned; its gateway report stays neutral
until ownership is released and fresh input arrives. Registration requires an
online discovery so the stored profile describes the actual module.

## Cockpit machine console

Cockpit uses correlated, bounded requests alongside the human console:

```text
cockpit 1 snapshot
cockpit 2 65536 register 00112233445566778899aabbccddeeff
cockpit 3 65536 apply
cockpit 4 65536 config 00112233445566778899aabbccddeeff 1 claim 17
cockpit 5 65536 config 00112233445566778899aabbccddeeff 1 get
```

Use the epoch returned by the current snapshot rather than the example value.
All mutations (`register`, `unregister`, `save`, `apply`) require that epoch;
a stale epoch returns `stale_epoch` without modifying state. Every response is
one JSON line with `{id,ok,error,state}`. The state contains gateway identity,
versions, running/layout state and all discovered or retained registered devices.
Each device includes its node, source node, boot session, configuration generation and lease, complete observed profile, pending/active
slots, `profileMatch`, `pendingProfileMatch`, input freshness and a report whose
length matches that observed profile. Missing, stale, directly USB owned,
unregistered or mismatched devices return neutral values under the observed
profile. `apply` acknowledges persistence before USB disconnects; the host must
reconnect to the same gateway UID and query the resulting state.

The receiver preserves a host’s first command when DTR opens so that a write
arriving with the control-line change is not discarded. Input is flushed on
close. Requests remain at most 479 characters, configuration children at most
383 bytes, and JSON responses fit the fixed 12 KiB writer; overflow fails without
emitting partial JSON. The legacy console commands remain available.

`config <identity> <boot> <child>` forwards to the identified module over the
current CAN route. The resulting JSON adds `targetConfig` for that request,
including active/draft settings, generations and descriptor bytes. The module
enforces a 30-second exclusive writer claim; Apply activates validated settings,
Save persists active settings and Apply and save persists before activation.
Mapping/calibration/name changes use the same C handlers on hardware and the
native emulator. See the protocol repository's
[`serial/runtime-configuration.md`](https://github.com/opensimgear/protocol/blob/main/serial/runtime-configuration.md)
for the child-command grammar.

Settings activation renews the CAN lease even when HID shape is unchanged.
Registered profile changes refresh the existing pending slot; explicit gateway
Apply persists and activates that profile and reconnects USB. Control counts,
axis usages and neutral values belong to that profile; a changed neutral requires
gateway Apply even when descriptor bytes stay the same. Names, raw calibration
endpoints, inversion and deadzone preserve the registered profile.

## CAN service and persistence

An INPUT_ACK is sent only after successful USB interrupt completion. It echoes
the immutable input sequence and exact delivered report, even if a newer CAN
input arrived while USB was busy. USB/run, route, lease, boot session, ownership
and freshness changes invalidate outstanding acknowledgments. Neutral fallback
reports, failed USB transfers and control `GET_REPORT` reads do not acknowledge
encoder pulses. A correctly routed module therefore advances pulses only when
the host has actually received them.

Cyphal/CAN FD uses 500 kbit/s arbitration, 2 Mbit/s data and bit-rate switching.
The hardware filter receives extended data frames for Cyphal application traffic,
standard services and CAN node-ID occupancy tracking. libcanard validates Cyphal
identifiers, transfer tails, transfer IDs and multi-frame CRCs/reassembly; generated
DSDL and application checks validate identity, source node, lease, boot session,
sequence, profile and report bounds. The receive
budget is four frames per loop. The eight-frame transmit queue sends one request
at a time, disables automatic hardware retransmission, and allows at most three
attempts with 20 ms retries. A request that remains pending 20 ms after cancellation begins forces
controller recovery. Transport deadlines are preserved into the hardware queue;
expired frames are discarded before sending or retrying. Standard Heartbeat runs
every second and GetInfo exposes the gateway UID and node name. Classic CAN input
transfers and FD without BRS are accepted by the stack too. Bus-off recovery runs at one-second intervals and
purges transport sessions/queues and revokes module leases. Diagnostics expose FIFO losses and queue drops. Inputs
neutralize after 250 ms without fresh input; nodes go offline after 1500 ms.

The application owns only the first 124 KiB of flash. Pages at `0x0801F000` and
`0x0801F800` form a two-page journal. Each record contains a sequence, CRC,
boot lease-block base and the portable 500-byte registry. New writes use format version 2 with a hat
count; version 1 records remain readable with zero hats and upgrade on the next
successful save. The inactive page is
erased, programmed and read back before its commit doubleword is programmed
last. Page indexes account for the actual MCU bank configuration. Power failure
before commit retains the previous complete record.

Before input can run, each boot reserves and persists a new block of 65,536
lease values. At most 65,535 leases can be issued in one boot; exhaustion rejects
new allocation until a reboot. Epoch wrap, a boot lease-block write failure, a corrupt
registry, or two nonblank invalid pages cause a fault with transceiver standby
and steady LED. Completely erased pages are treated as a virgin device. A
factory erase resets the lease history: power-cycle the entire module chain
before reuse, not only the gateway. Every boot/save/apply erases one journal
page; endurance and interrupted hardware erase/program need bench qualification.
A failed CDC `save` or `apply` returns an error and keeps the active layout and
service running; it does not enter the boot fault state.

The linker uses the guaranteed 128 KiB SRAM region and reserves 16 KiB for stack
plus 512 bytes for heap, avoiding dependence on the optional additional parity
SRAM. Arm GCC emits `.su` stack-usage files for reviewing call paths. ST describes
the SRAM distinction in the
[STM32G0B1 datasheet, section 3.4](https://www.st.com/resource/en/datasheet/stm32g0b1kb.pdf).

See [board wiring and bring-up](boards/gateway_g0b1cb_v0_1_0/README.md),
[portable behavior](docs/core.md), [CAN peer utility](../tools/README.md), and
[validation results](VALIDATION.md).
