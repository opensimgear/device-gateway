# Gateway CAN FD bring-up protocol, version1

This is a private, bounded prototype protocol for the gateway and the matching
`tools/can_peer.py` peer. It is **not Cyphal/CAN**, does not implement the canonical
OpenSimGear protocol, and cannot communicate with the current device-template
firmware, which keeps CAN in standby. The cockpit Pico simulator's JSON and USB
layout are a separate implementation. Interoperability with the current cockpit
and recognition of these controller collections on each host OS remain unmeasured.

The gateway's two true4P4C sockets continue one CAN FD bus; socket position is
not an address. There are up to10 modules, each with a unique16-byte persistent
identity, a nonzero32-bit boot session, and up to64 buttons and8 unsigned16-bit
axes. Identity bytes are opaque and preserve byte order; all-zero and all-FF
identities are rejected. A module must change its boot session when restarting.
Direct USB ownership announced by a module immediately suppresses routed input.

Use500 kbit/s nominal arbitration and2 Mbit/s data, extended29-bit identifiers,
CAN FD and bit-rate switching. The wire ID is `0x1a500000 | (type <<8) | node`.
The receive filter is ID`0x1a500000`, mask`0x1ffff800`; decoding also verifies the
exact type/node ID. A node address1–10 is local to this gateway's chain. ANNOUNCE
always uses node0. Every field and frame has an exact bound; no fragmentation,
dynamic allocation, protocol retransmission queue, or arbitrary CAN-to-USB tunnel
exists. The board driver separately bounds its transmit queue to eight frames and
retries a frame at most three times; modules recover lost assignments by announcing
again.

| Common byte offset | Meaning |
| --- | --- |
|0–1|`4f47` (`OG`) |
|2|protocol version1 |
|3|type:1 ANNOUNCE,2 ASSIGN,3 INPUT,4 HEARTBEAT |
|4|node, identical to low ID byte |
|5|bit0 direct USB owns input; remaining bits zero |
|6–7|zero |
|8–23|persistent module identity |
|24–27|module boot session, little endian |
|last4 bytes|little-endian CRC-32/ISO-HDLC of all preceding bytes |

CRC uses reflected polynomial`0xedb88320`, initial value`0xffffffff`, final XOR
`0xffffffff`. The check for ASCII`123456789` is`0xcbf43926`. The CAN controller
also checks the CAN FD link CRC; the application CRC makes stored/offline fixtures
self-checking. It does not authenticate participants.

| Type | Exact frame bytes | Fields after the common header |
| --- | --- | --- |
|ANNOUNCE|64|28 button count;29 axis count;30–31 nonzero profile revision;32–39 eight axis usages;40–55 eight little-endian neutral values;56–59 zero;60–63 CRC |
|ASSIGN|48|28–31 nonzero lease;32–35 gateway epoch;36–43 zero;44–47 CRC |
|INPUT|64|28–31 lease;32–35 sequence;36–59 report payload;60–63 CRC |
|HEARTBEAT|48|28–31 lease;32–35 sequence;36–43 zero;44–47 CRC |

Unused ANNOUNCE axis usage and neutral entries must be zero. Active axis usages
are unique Generic Desktop usages`0x30`–`0x38`. A profile requires at least one
button or axis. INPUT and ASSIGN must have direct-USB flag0. ANNOUNCE and
HEARTBEAT carry current ownership; a heartbeat with flag0 releases ownership,
but fresh input is required before a routed report becomes active again.

The report packs buttons first, least-significant bit first, rounded to whole
bytes, followed by the configured axes as little-endian unsigned16-bit values.
It occupies at most24 bytes. Button padding bits and unused report bytes must
be zero. Example:3 buttons and2 axes require5 bytes, with a payload
`05 34 12 ff ff` for buttons1 and3 pressed and axes`0x1234`,`0xffff`.

A matching module sends ANNOUNCE periodically, e.g. every500 ms, until assigned
and continues announcing to recover gateway resets. It accepts only assignments
that match its identity and current boot session, retaining the provided lease
and node. INPUT and HEARTBEAT share a monotonically increasing32-bit sequence;
wrap is accepted only within the forward half of the sequence space. Sending
input every10 ms and a heartbeat every500 ms is a useful bring-up policy.

An assignment is idempotent for an online unchanged session/profile. Module
restart, timeout, CAN reset, or profile change revokes its old lease. The gateway
checks identity, session, lease, node, sequence, profile payload length and padding
before accepting input; reuse of a node by another identity does not reuse its
controller slot. Duplicate physical modules with identical persistent identities
cannot be reliably distinguished; identities must actually be unique.

An input older than250 ms yields a neutral report even while heartbeats continue.
A module with no accepted traffic for1500 ms goes offline and releases its node.
The board calls `osg_gateway_tick()` continuously; intervals and ordering use
unsigned wrap-safe arithmetic. Intervals shorter than half the32-bit timer range
are required. Malformed, stale and out-of-order frames never refresh health.
Gateway stop, start, USB loss, and layout application clear input, retaining sequence
guards. Fresh input is required after start. A profile different from the registered
profile yields neutral input until explicitly registered and applied again.

The board persists a nonzero lease-block base before accepting CAN traffic and
advances it by65536 each boot. The core allocates at most65535 leases per boot
and refuses further renewals on exhaustion. The board must refuse persistent
epoch overflow instead of reusing an old block. Erasing storage removes this
cross-boot guarantee; power-cycle the attached modules when factory-resetting
the gateway. This guards against accidental stale traffic, not a malicious peer.

Exact frames from the C implementation are committed in
[`../tests/can-fd-v1-vectors.json`](../tests/can-fd-v1-vectors.json). Run the native
test executable with`--vectors` to reproduce them; the Python peer checks the
same fixtures independently.
