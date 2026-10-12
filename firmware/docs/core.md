# Portable gateway core

`gateway.c`, `gateway_protocol.c`, `gateway_registry.c` and `gateway_cockpit.c` are allocation-free C11
and have no MCU, RTOS, TinyUSB or host-library dependency. The STM32 board
adapts CAN FD frames, time, USB, and persistent storage around these interfaces.
All mutation belongs to a single event loop; callbacks must not mutate the core
concurrently. Rejected frames are counted and do not refresh their source's health.

Initialize with a valid saved registry and a freshly reserved persisted lease
block. Every saved identity occupies one of 10 slots. Discoveries allocate chain
node IDs independently; temporary absence, moving between the two ports, or
allocation order does not move the registered identity's HID slot. Registered
identities reserve inventory capacity even while unplugged. Offline unregistered
discoveries can be replaced when the bounded inventory fills.

`osg_gateway_register()` stages the observed online module's profile in the
pending registry. Existing identity uses its previous slot; a new identity uses
the first free slot. `osg_gateway_unregister()` stages removal without compacting
other slots. Profiles comprise buttons, axis usages, neutrals and revision; neutral
values are part of the registered layout. A staged change affects only pending
state. `osg_gateway_needs_apply()` compares effective profiles and identities,
not the generation counter.

For an explicit layout apply, first save the pending registry successfully, then
disconnect USB, call `osg_gateway_apply_layout()`, rebuild the HID descriptor,
and reconnect USB. Applying stops input; the board starts again after USB mounts
and requires fresh CAN input. USB must keep returning the old descriptor until it has disconnected.
Offline slots remain present and produce their configured neutral values.

There is one HID interface with a joystick top-level application collection per
occupied slot. Report ID is slot index plus 1; no slots are renumbered automatically.
The descriptor uses standard joystick, button and axis usages, with no proprietary
input fields. The full maximum profile has 35 on-wire bytes including its report ID,
well below the 64-byte full-speed interrupt packet. Hosts may expose top-level
collections differently; binding stability needs physical Windows/Linux/macOS
validation. There is no claim of equivalence to the simulator's multiple HID
interfaces. An empty registry exposes a one-byte vendor management collection
with report ID 11 and no joystick. The board supplies one zero byte for this
placeholder when queried.

`osg_gateway_report()` supplies a registered slot's payload without its report ID.
It returns 0 for an empty slot or insufficient output capacity. Active input needs
running state, an online module, fresh accepted input, matching registered profile,
and no direct-USB owner. Any other state emits neutral. The application must
service `osg_gateway_tick()` before scheduling reports, including during USB
disconnects and CDC activity.

The persistent codec is an exact 500-byte, versioned little-endian record:

| Offset | Contents |
| --- | --- |
| 0–3 | ASCII `OGR1` |
| 4–5 | format version 2 (version 1 remains readable) |
| 6 | slot count 10 |
| 7 | zero |
| 8–11 | registry generation |
| 12–15 | record size 500 |
| 16–495 | ten 48-byte slot records |
| 496–499 | CRC-32/ISO-HDLC over bytes 0–495 |

Each present slot contains occupied 1, button count, axis count, hat count,
identity 16 bytes, revision 2 bytes, usages 8 bytes, neutrals 16 bytes, reserved 2 bytes.
An empty slot is all zero. Unknown versions, wrong length, nonzero reserved bytes,
invalid capabilities, duplicate identities, or bad CRC are rejected without
modifying the caller's output. Struct memory is never serialized directly.

`osg_gateway_registry_select()` picks the newest valid of two records using
generation arithmetic; a corrupt/interrupted inactive write preserves the other
record. Generation differences must remain within half the 32-bit range. A tie
or ambiguous half-range difference chooses record 0. This codec does not make
flash writes atomic: the board stores each record on an independent erase page,
verifies its bytes, and writes a separate commit marker last. The board checks
that marker before calling the codec. The same journal also preserves the boot
lease-block base. All hardware flash behavior still requires power-loss testing.

Native tests cover strict decoding and frame CRCs, capability and payload bounds,
sequence/time wrap, direct-USB ownership, stop/start rearming, stale node reuse,
module timeout, bounded allocation, lease exhaustion, stable descriptors, explicit
profile application, registry generation wrap, corrupt records and every interrupted
byte position in an inactive write. These are logic tests, not electrical validation.

Report order is buttons, packed four-bit hats, then unsigned little-endian axes.
A neutral used hat has nibble 8; an unused high nibble is zero. The maximum
profile is 128 buttons, four hats and eight axes, producing 34 payload bytes.
The v2 codec routes compact INPUT/INPUT_ACK by node, boot and lease; full UUIDs
remain in ANNOUNCE/ASSIGN/HEARTBEAT. The registry writes hat count in slot byte 3;
legacy version 1 records require that byte zero and retain their 64-button bound.

`osg_gateway_delivery_prepare()` captures an eligible active input’s exact
sequence/report and run/module generations at USB submission.
`osg_gateway_delivery_complete()` validates the callback’s report ID, length and
bytes, plus the still-current route and generations, before encoding an ACK.
New input cannot overwrite the captured sequence, and a heartbeat’s sequence is
not confused with the last input sequence. USB/run, ownership, stale input,
lease/session and active profile changes discard old completions. Neutral
fallbacks never qualify; legitimate neutral input does. The STM32 calls this
only from TinyUSB’s successful interrupt report callback.

`gateway_cockpit.c` parses bounded machine requests and formats correlated JSON
snapshots. Mutation epochs must match before staging or flash callbacks run.
Apply only schedules USB activation after persistence succeeds. The fixed writer
returns an empty string on overflow, and tests parse fresh maximum snapshots
independently as JSON. Full device calibration/settings/update commands remain
outside this inventory/registry contract.
