# OpenSimGear CAN FD v2 prototype

The gateway, generic module and reference Python peer share this bounded codec.
It is not Cyphal/CAN. Version 1 wire frames are rejected; only the persisted
500-byte version 1 registry remains readable. Canonical codec snapshots and
shared vectors live in the protocol repository. USB controller collection
behavior and physical CAN traffic still need assembled-board validation.

Use one linear powered 4P4C chain, pins 1=12 V, 2=CAN_H, 3=CAN_L, 4=GND,
with two 120 ohm endpoint terminators. Both gateway sockets share one bus.
CAN FD uses 500 kbit/s arbitration, 2 Mbit/s data, BRS and extended identifiers.
The identifier is `0x1a500000 | (type << 8) | node`. ANNOUNCE uses node 0;
assigned nodes are 1–10. Socket position is not an address.

Bytes 0–7 are common: `4f 47`, version 2, type, node, direct-USB flag,
and two zero bytes. The exact identifier must match type/node. All reserved
bytes, flag bits, lengths and profile padding are validated. CRC is little-endian
CRC-32/ISO-HDLC over all preceding frame bytes (polynomial `0xedb88320`,
initial/final XOR `0xffffffff`; `123456789` gives `0xcbf43926`).

| Type | Length | Payload after byte 7 |
| --- | --- | --- |
| ANNOUNCE=1 | 64 | UUID 8–23; boot 24–27; buttons 28; axes 29; revision 30–31; usages 32–39; axis neutrals 40–55; hats 56; zero 57–59; CRC 60–63 |
| ASSIGN=2 | 48 | UUID 8–23; boot 24–27; lease 28–31; gateway epoch 32–35; zero 36–43; CRC 44–47 |
| INPUT=3 | 64 | boot 8–11; lease 12–15; sequence 16–19; report 20–53; zero 54–59; CRC 60–63 |
| HEARTBEAT=4 | 48 | UUID 8–23; boot 24–27; lease 28–31; sequence 32–35; zero 36–43; CRC 44–47 |
| INPUT_ACK=5 | 64 | Same compact payload as INPUT; exact sequence/report delivered by gateway USB |

Integers are little-endian. UUIDs are opaque 16 bytes; all-zero/all-FF are invalid.
Generic modules use ASCII `OSGD` followed by the raw 12-byte MCU UID. Boot sessions,
leases, gateway epochs and profile revisions are nonzero. Only ANNOUNCE and
HEARTBEAT may carry direct-USB ownership. INPUT and INPUT_ACK omit the UUID and
resolve to exactly one online node, boot session and lease. Ambiguous routes fail.

Profiles support 128 buttons, four hats and eight axes; at least one control is
required. Axis usages are unique Generic Desktop values `0x30`–`0x38`.
Unused usages/neutrals are zero. Reports contain `ceil(buttons/8)` button bytes,
then `ceil(hats/2)` packed hat bytes, then two bytes per axis. Buttons and hats
pack low bits/nibbles first. Hat directions are 0–7 and neutral is 8; other
nibbles are invalid. Unused high hat nibbles, button padding and unused bytes
in the fixed 34-byte report area are zero. A maximum profile is 34 bytes, or
35 USB bytes with the stable report ID.

ANNOUNCE supplies capabilities and obtains an idempotent assignment. A changed
profile/session, timeout or bus recovery revokes the lease. Module announcements
continue after assignment to recover gateway reset. Generic firmware uses
identity/session-derived timing jitter because simultaneous ANNOUNCE frames
share an identifier; collision and retry behavior still needs physical testing.
INPUT and HEARTBEAT share forward-half-range sequence checks. Stale input yields
neutral after 250 ms; no accepted traffic for 1500 ms marks a module offline.

The gateway captures eligible routed input when submitting a USB interrupt
report. Only its successful completion can produce INPUT_ACK. The ACK echoes
that snapshot’s sequence/report rather than newer module state. Failed transfers,
neutral fallback reports and old USB/run/ownership/session/lease/freshness
snapshots cannot acknowledge. A valid fresh input that itself contains neutral
values can be acknowledged. Modules match the ACK to recent sent history and
encoder phase before advancing press/release pulses. CAN receipt alone does not
advance a pulse.

Each gateway reserves a new persisted block of 65,536 lease values per boot,
issuing at most 65,535. Epochs are scoped to one gateway, not globally ordered.
The v2 ASSIGN has no gateway identity; a module clears its assignment scope on
CAN reset/timeout or reboot before migration. Factory-erasing a gateway or module
requires power-cycling the chain to discard retained sessions. Exhaustion, epoch
wrap or corrupt nonblank storage fails closed.

`tests/can-fd-v2-vectors.json` contains five message types and maximum-profile
frames. Generate them with `osg_gateway_test --vectors`. The independent Python
encoder checks identical bytes. `--cockpit-vectors` generates actual machine
console snapshots for native Cockpit conformance tests.
