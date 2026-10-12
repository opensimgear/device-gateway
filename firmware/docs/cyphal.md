# Gateway Cyphal/CAN application contract

Gateway and generic-device firmware use canonical generated OpenSimGear DSDL
and pinned libcanard 5.0. The deployed transport is Cyphal/CAN 1.1 with the
13-bit subject-ID messages compatible with Cyphal 1.0 tools. Application contract
version is 3 (`canProtocol=3`); the USB CDC console remains version 2. The former
private CAN FD v1/v2 identifiers and payloads are rejected. Persistent registry
versions 1 and 2 still load and preserve their stable HID slots.

The active definitions and subject bindings belong to the protocol repository.
Each firmware keeps a maintained `shared-cyphal/` snapshot so native and embedded
builds use the same generated types and real transport without a network fetch.
See [shared transport](../shared-cyphal/README.md) for pins, generation and limits.

| Subject | DSDL type | Direction and purpose |
| --- | --- | --- |
| 2000 | `opensimgear.input.Announce.1.0` | Device publishes stable UID, boot session, direct-USB ownership and full input profile |
| 2001 | `opensimgear.input.Assignment.1.0` | Gateway publishes UID/session-bound logical route, lease and persisted gateway epoch |
| 2002 | `opensimgear.input.Input.1.0` | Device publishes route/session/lease, application sequence and exact packed HID report |
| 2003 | `opensimgear.input.Status.1.0` | Device publishes identity-bound ownership/presence state |
| 2004 | `opensimgear.input.InputAck.1.0` | Gateway publishes the sequence/report actually completed by USB HID |
| 7509 | `uavcan.node.Heartbeat.1.0` | Each node publishes standard health/mode/uptime every second |

Standard `uavcan.node.GetInfo.1.0` service 430 returns protocol version 1.1,
stable 128-bit UID and reversed-domain node name. The gateway name is
`org.opensimgear.gateway`; its Cyphal UID comprises the MCU's 96-bit UID followed
by bytes `47 57 54 59`. The existing 24-hex USB serial/CDC identity remains the
MCU UID. Node information does not expose calibration, settings or update services.

Cyphal CAN node IDs are selected and repaired by libcanard's collision handling.
The gateway's logical routes 1–10 identify leased devices within one chain;
permanent registry slots determine HID report IDs. These three addresses are
independent. Source CAN node metadata is tied to the announced UID/session/lease.
Input or Status from a different CAN source is rejected. Source migration renews
the application lease; colliding UIDs invalidate the previous route until the
transport resolves their addresses. Old leases and USB snapshots cannot carry
input across that boundary. This is stale-traffic protection, not authentication.

All five application types fit a single CAN FD frame at maximum profile size.
libcanard constructs the identifier, pads before the tail byte, assigns transfer
IDs, suppresses duplicate transfers and reassembles Classic CAN or multi-frame
service transfers with the Cyphal transfer CRC. No private magic prefix or
application frame CRC is added. Standard Cyphal tools can inspect the traffic
using the canonical DSDL.

The driver transmits at 500 kbit/s arbitration and 2 Mbit/s data with BRS.
It receives extended data frames broadly for application subjects, services and
node-ID occupancy tracking; Classic CAN and FD without BRS are also accepted.
The shared transport uses a fixed 32 KiB pool and 16-frame queue. Driver queue
backpressure leaves transport frames pending for subsequent ticks. Outgoing
application transfers expire after 100 ms, standard traffic after 1 s; the board
preserves deadlines through its eight-frame software queue and aborts expired
hardware requests before retrying. Bus recovery flushes transport and hardware
state and revokes application leases. Local CAN node-ID migration also flushes
already admitted controller frames and invalidates routes; devices reconnect
after their current gateway source expires.

Input neutralizes after 250 ms without accepted fresh input, and a device goes
offline after 1500 ms without accepted application presence. Standard Heartbeat
alone does not renew a route or grant input ownership. Direct USB ownership
neutralizes the gateway slot until released and fresh input arrives. Only
successful USB interrupt completion creates InputAck, preserving the exact
immutable submitted report even when newer input arrives while USB is busy.
Neutral fallback, control GET_REPORT and failed transfers cannot acknowledge pulses.

The electrical chain remains one linear bus: pins 1=12 V, 2=CAN_H, 3=CAN_L,
4=GND, with two endpoint terminators and one gateway. Follow the board and power
qualification instructions before live testing.
