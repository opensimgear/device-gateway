# STM32 CAN gateway firmware

The v0.1.0 target is `gateway_g0b1cb_v0_1_0`: STM32G0B1CBT6, USB-C device,
one CAN FD controller/transceiver, two parallel **4P4C** chain jacks, and a
center-positive 12 V barrel input. The jacks share one bus and carry
pin 1 BUS12V, pin 2 CANH, pin 3 CANL, pin 4 GND. Moving a module between the
jacks does not change its identity or controller order.

The firmware builds a real STM32 ELF, binary and Intel HEX. It has active CAN FD,
USB HID plus CDC, discovery, bounded chain-local logical route allocation, an identity-based
registry, persistent storage, routed runtime module configuration and automatic saved USB layout activation. It uses the
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

The build pins firmware-common in [`cmake/common-revision.cmake`](cmake/common-revision.cmake) for TinyUSB, ST HAL and CMSIS.
The maintained `shared-cyphal/` snapshot comes from canonical `protocol/cyphal/`
and supplies generated Nunavut bindings, OpenSimGear DSDL and pinned libcanard 5.0.
Both native tests and the board target link that actual transport. Dependency
clones remain under the build directory’s `_deps/`. See
[dependency configuration](cmake/README.md) for local source overrides and
[shared transport](shared-cyphal/README.md) for generation, pins and provenance.

## USB controllers and remembered devices

The MCU's 96-bit UID is the stable 24-hex gateway identity and USB serial.
The CDC handshake is `gatewayType=stm32-can-gateway protocol=3 canProtocol=3 firmware=0.4.0`.
Module UUIDs are separate 16-byte values printed as 32 hex digits.

The gateway remembers up to 32 identities, with independent enabled state,
explicit HID report ID and position in the enabled controller order. Unknown
valid CAN identities enable and append automatically without Cockpit. Enabled
offline devices remain enumerated with their stored neutral reports. Disable
removes a collection but persists the identity/disabled choice; Enable appends it
at the end; Forget is accepted only while offline and removes the remembered
record. Repeated announcements and gateway boot respect explicit disable.

One HID interface contains a standard joystick top-level collection for each
enabled identity, retaining up to 128 buttons, four packed hats and eight 16-bit
axes per profile. Reports pack buttons, hats (8 neutral), then little-endian axes.
Zero enabled identities use CDC-only enumeration with no HID placeholder or
permanent management collection. Direct USB ownership, stale input, missing
modules or an observed profile mismatch produce configured neutral HID reports.

Existing descriptor collections remain ordered and unchanged when new ones
append. Disable/Forget can shift later Windows collection identities, requiring
rebinding; Enable can assign a returning controller a different collection index.
Windows' single/multiple collection transition also requires physical testing.
The gateway preserves USB VID/PID, serial and CDC interface numbers; the HID
interface remains number 2 whenever present. USB attaches only with VBUS.
Disconnect, suspend and resume require fresh CAN input before active reports.

## CDC console

New identities enroll automatically. The human console provides `identity`,
`inventory`, `registry`, `diagnostics`, `help`, and these explicit lifecycle actions:

```text
disable 00112233445566778899aabbccddeeff
enable 00112233445566778899aabbccddeeff
forget 00112233445566778899aabbccddeeff
update-profile 00112233445566778899aabbccddeeff
```

Commands stage intent. The background worker saves it and reconnects USB only
when the enabled layout changes. `forget` rejects an online device. Disabled
online devices remain discoverable and configurable; forgotten devices enroll
as new if they return. Human lifecycle replies use the same JSON schema as
machine requests. There is no manual Register/Save/Apply onboarding.

## Cockpit machine console

```text
cockpit 1 snapshot
cockpit 2 65536 disable 00112233445566778899aabbccddeeff
cockpit 3 65536 enable 00112233445566778899aabbccddeeff
cockpit 4 65536 config 00112233445566778899aabbccddeeff 1 claim 17
cockpit 5 65536 config 00112233445566778899aabbccddeeff 1 get
cockpit 6 65536 events 00112233445566778899aabbccddeeff 1 state
cockpit 7 65536 events 00112233445566778899aabbccddeeff 1 read 1 0
```

Use the current snapshot epoch. All lifecycle/configuration mutations validate
that epoch; configuration additionally validates the module boot and route.
Each response is one correlated `{id,ok,error,state}` JSON line. `ok` acknowledges
staging, not final durable publication. Observe `layoutPending`, `layoutError`,
`activeGeneration` and `pendingGeneration`; reconnect to the same gateway UID
and query state without replaying an ambiguous mutation.
After a renewed lease, `configuration_route_not_ready` rejects configuration
before dispatch until fresh Input/Status proves the device received its new
Assignment. Read-only queries can wait for fresh state and retry; mutation commands
remain single-dispatch.

Each remembered device includes the observed profile/report, CAN route/session,
`enabled` desired intent, `published` active collection, `descriptorIndex` (-1
when absent), explicit `reportId` (null when absent), `profileMatch` and input
freshness. Disabled inventory reports are neutral even during a pending removal.
`admissionFailure`/`lastRejectedIdentity` expose capacity failures instead of
silently hiding announcements. The CDC line limit is 479 characters, child
configuration limit 383 bytes, and response writer 32 KiB; overflow returns no
partial JSON. DTR opening preserves the first host command; closing flushes input.

`config <identity> <boot> <child>` forwards configuration and adds `targetConfig`
to the correlated response. Settings use the module's exclusive writer/draft and
Apply/Save semantics. See [runtime configuration](https://github.com/opensimgear/protocol/blob/main/serial/runtime-configuration.md)
and [CDC v3](https://github.com/opensimgear/protocol/blob/main/serial/stm32-gateway-v3.md).

`events <identity> <boot> state|read <stream> <after>` forwards a bounded
read-only panel event command and adds `targetEvents`. It validates the same
current route/session/lease as settings and actions. Device event pages are
nonconsuming, at most 16 records from a 64-record ring, and remain reachable
with HID disabled or direct USB active. State establishes a baseline; mapping
activation/reboot changes the scope, and overflow reports an explicit gap.
The gateway does not persist events or infer them from its latest HID report.
See the [panel event contract](https://github.com/opensimgear/protocol/blob/main/serial/panel-input-events.md).

Enabled profiles stay frozen until `update-profile`; changing an input layout or
neutral value therefore needs explicit activation with a binding-change warning.
Disabled profiles can refresh without publishing a collection. Settings activation
still renews the CAN lease independently of unchanged HID shape, preventing
input from the previous configuration from carrying across activation.

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
boot lease-block base and the portable 1,556-byte version-3 registry. Versions 1/2 retain their exact
500-byte decoder; migration gathers occupied entries in descriptor order while
preserving their original report IDs. New records remember enabled state and
independent descriptor order/report ID. The inactive page is
erased, programmed and read back before its commit doubleword is programmed
last. Page indexes account for the actual MCU bank configuration. Power failure
before commit retains the previous complete record.

Before input can run, each boot reserves and persists a new block of 65,536
lease values. At most 65,535 leases can be issued in one boot; exhaustion rejects
new allocation until a reboot and exposes `lease_exhausted`. Enable/Disable
publication renews the affected device's lease and clears its delivery state, so
the next Assignment rearms device acquisition and discards queued encoder turns.
The worker checks capacity before save, reserves it after save, and allocates
monotonically newer leases at publication. Offline identities are included in the
reservation because they may reconnect before publication. Unchanged announcements
and metadata-only persistence do not renew leases. Epoch wrap, a boot lease-block write failure, a corrupt
registry, or two nonblank invalid pages cause a fault with transceiver standby
and steady LED. Completely erased pages are treated as a virgin device. A
factory erase resets the lease history: power-cycle the entire module chain
before reuse, not only the gateway. Each boot or changed-state transaction erases one journal page. Closely spaced
changes batch for 100 ms, capped at 500 ms; unchanged announcements cause no
writes. A failed background save retains the previous active layout, exposes
`layoutError` and retries after at least one second. Serial backpressure cannot
delay detach beyond the bounded grace period. Endurance and interrupted hardware
erase/program still need bench qualification.

The linker uses the guaranteed 128 KiB SRAM region and reserves 16 KiB for stack
plus 512 bytes for heap, avoiding dependence on the optional additional parity
SRAM. Arm GCC emits `.su` stack-usage files for reviewing call paths. ST describes
the SRAM distinction in the
[STM32G0B1 datasheet, section 3.4](https://www.st.com/resource/en/datasheet/stm32g0b1kb.pdf).

See [board wiring and bring-up](boards/gateway_g0b1cb_v0_1_0/README.md),
[portable behavior](docs/core.md), [CAN peer utility](../tools/README.md), and
[validation results](VALIDATION.md).
