# Portable gateway core

`gateway.c`, `gateway_registry.c`, `gateway_journal.c`, `gateway_cockpit.c`,
`gateway_config.c` and `gateway_layout.c` are allocation-free C11. The STM32 and
native simulation adapt CAN frames, USB, time and persistent storage around the
same code. One event loop owns mutations; callbacks must not modify the core
concurrently. Rejected frames do not refresh their source's health.

## Remembered devices and controller order

Initialize with a valid saved registry and a newly persisted lease block.
Unknown valid announcements automatically enable their identity and append its
HID collection. Up to 32 identities, routes and enabled controllers are supported.
Remembered records store an explicit report ID and enabled descriptor order;
record storage indexes and CAN routes are independent of both. Historical names
`slots`/`find_slot` in the C API refer to record storage, not user-assigned slots.

Enabled devices retain their collection while offline, producing the stored
neutral report. Cockpit shows these as ghosted; Windows still sees an attached
neutral controller. Moving CAN source or connector, reconnecting, and reboot do
not reorder an existing identity. Explicit Disable persists the disabled choice
and removes its collection. Repeated announcements and reboot never re-enable
it. Enable appends at the end. Forget rejects online identities and removes the
remembered record; its later announcement enrolls as new.

Enabled control profiles remain frozen until explicit `update-profile`, while
disabled profiles may refresh from announcements without publishing anything.
Control counts, axis usages, neutral values and revision belong to the retained
profile. Settings generation renews the CAN lease independently of HID shape.

Inventory and persistence are bounded. Excess announcements return
`OSG_GW_RX_FULL` and expose a rejection count, identity and admission error through
CDC3. Offline remembered identities continue to consume capacity until forgotten;
disabling alone does not release the remembered-device bound.

## Automatic save and USB activation

`osg_gateway_layout_service()` batches changes for 100 ms, with a 500 ms bound
under continuous arrivals. It validates the target and saves an immutable registry
snapshot before scheduling a descriptor change. All valid profiles at the
32-controller limit fit the 4,096-byte descriptor buffer; a compile-time maximum
size bound and maximum-profile tests enforce that admission guarantee.

For DETACH, the adapter first disconnects USB, invalidates reports/cancels routed
configuration, calls `osg_gateway_activate_prepared()` with `layout.target` and
`layout.activation`, and rebuilds the descriptor. USB must
continue returning its old cached descriptor until disconnected. Reattachment
follows at least 250 ms later, and live input requires fresh CAN state. Additional
announcements after save queue another transaction; they cannot enter the saved
layout. Remembered changes that do not affect any enabled collection save and
activate without reconnecting or stopping current input.

Before save, the worker checks lease capacity for every identity whose published
enabled state changes, including offline identities. After save it reserves that
capacity; announcements cannot spend it. Publication allocates monotonically newer
leases and clears those devices' input, sequence and delivery state. A fresh
Assignment makes the device rearm acquisition and discard queued encoder turns,
preventing turns accumulated while disabled from replaying on re-enable. A late
boot/profile announcement cannot make the publication lease older than the device's
latest assignment. Unchanged visibility and metadata changes do not renew leases.
Exhaustion preserves the active layout, reports `lease_exhausted`, and uses the same
bounded retry interval.

A save/readback failure leaves the active descriptor and reports unchanged,
exposes `layoutError`, and retries no more frequently than once per second.
Reverting a failed change clears the obsolete error. Repeated unchanged
announcements do not write flash or reconnect. Serial/configuration drain gets a
100 ms grace; a non-reading host cannot block detach indefinitely (maximum an
additional second). The worker runs with Cockpit and CDC disconnected.

## HID collections and reports

One HID interface has a joystick top-level collection per enabled identity, in
persistent descriptor order. Report IDs are explicit retained values, independent
of collection indexes. Disable/removal can shift later Windows collection numbers;
re-enable appends. Keep VID/PID, USB serial, CDC interface numbers and existing
controls stable. Single/multiple collection transitions and game binding behavior
still require physical Windows/Linux/macOS qualification.

Zero enabled devices produce a zero-length HID descriptor and CDC-only USB
configuration, without a management collection or joystick placeholder. Nonempty
layouts use standard joystick/button/axis/hat usages. The maximum profile is
128 buttons, four hats and eight axes: 34 payload bytes plus the report ID, below
the 64-byte interrupt packet. Reports pack buttons, four-bit hats (8 neutral),
then unsigned little-endian axes. Unused bits/nibbles must be zero.

`osg_gateway_report()` addresses a remembered active record. It returns zero for
an absent/disabled entry or insufficient capacity. Live input requires running
USB, an online module, fresh accepted input, equal retained/observed profile and
no direct USB owner; every other condition produces retained neutral values.
Adapters must service `osg_gateway_tick()` even during CDC activity/USB disconnect.

`osg_gateway_delivery_prepare()` captures the exact eligible report, sequence and
run/module generations when submitted to USB. `delivery_complete()` checks the
successful callback's report ID, bytes, length, current identity/profile, route,
lease, boot, ownership and freshness before creating InputAck. New input cannot
rewrite the captured sequence. Neutral fallbacks, GET_REPORT and failed transfers
never qualify; legitimate neutral input does. USB completions from an old layout
or CAN source are rejected.

## Persistent codec and journal

The version-3 codec is an exact 1,556-byte little-endian record:

| Offset | Contents |
| --- | --- |
| 0–3 | ASCII `OGR1` |
| 4–5 | Format version 3 |
| 6 | Record capacity 32 |
| 7 | Zero |
| 8–11 | Registry generation |
| 12–15 | Record size 1556 |
| 16–1551 | Thirty-two 48-byte remembered-device records |
| 1552–1555 | CRC-32/ISO-HDLC over bytes 0–1551 |

A present record contains flags (occupied bit0, enabled bit1), button/axis/hat
counts, 16-byte identity, two-byte revision, eight usages, sixteen neutral bytes,
report ID, and enabled order (255 for disabled). Empty records are all zero.
Validation rejects unknown versions, sizes/flags, invalid profiles, duplicate
identities/report IDs, noncontiguous enabled order, bad CRC or reserved data
without changing the output. Struct memory is never serialized directly.

Exact 500-byte v1/v2 records remain readable. Migration gathers occupied records
in their old descriptor order and preserves each old report ID (`old index + 1`).
Version 1 requires zero hats and its original 64-button limit. The next successful
save writes v3. `registry_select()` picks the newest valid record using wrap-safe
generation arithmetic, with deterministic tie/half-range handling.

The board journal stores each record on its own 2 KiB erase page, verifies all
programmed bytes and writes a separate commit marker last. It also stores the boot
lease-block base. Interrupted inactive writes preserve the previous committed
page. Erase/program timing, endurance and brownout behavior remain bench tests.

## CAN routes and CDC3

The shared Cyphal transport supplies complete decoded transfers with source node
metadata. Logical routes1–32 are not Cyphal CAN node IDs. Announce/Assignment/Status
carry full UUIDs; Input/InputAck carry compact route, boot and lease fields. Input
and Status from another source are rejected. Source migration needs a fresh
announcement/lease and invalidates old USB completions. A different UUID on an
occupied source revokes the previous route while CAN collision handling repairs
addresses. Bus recovery resets transport and application leases.

CDC3 supports snapshot, enable, disable, forget, update-profile and routed config.
Mutations validate the current gateway epoch before staging; child configuration
also binds boot, identity, source, route, lease, epoch and request ID. Responses
acknowledge staging. Hosts observe `layoutPending`, active/pending generations and
publication after reconnect without replaying an ambiguous command.
`configuration_route_not_ready` prevents dispatch until a post-assignment
Input/Status confirms the current route. A client can retry a read-only query
after refreshing state; it must not replay an uncertain mutation.

Snapshots retain offline and disabled identities. Each module reports enabled
intent, active publication, descriptorIndex (-1 absent), reportId (null absent),
observed profile, route/session, profileMatch and input freshness. Disabled CDC
inventory is neutral even before its old active collection has been detached.
Responses fit a 32 KiB fixed writer; overflow returns no partial JSON. Commands
are at most479 bytes, child commands383 bytes, and module responses4095 bytes.
Configuration fragments expire after four seconds. Target responses appear only
once route-guarded assembly is complete.

Native tests cover all lifecycle transitions, append descriptor prefixes, sparse
legacy migration, 32 maximum profiles, saturation diagnosis, neutral ghosts,
explicit profile update, interrupted saves, saved-generation races, bounded
batch/backoff/drain and disabled restart. Delivery/route/configuration tests retain
stale-traffic and ACK semantics. The journal suite interrupts every erase/program
operation for legacy500, current1556 and maximum1984-byte payloads. These are
software checks, not physical USB/CAN qualification.
