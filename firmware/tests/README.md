# Firmware validation

The portable suites test observable gateway behavior and interrupted flash
journal writes. See [recorded validation results](../VALIDATION.md). Hardware
acceptance must cover USB HID/CDC enumeration, CAN communication, ownership,
power, disconnect recovery, endpoint and inline placement, and independent
gateways. Build success alone is not hardware validation.

## Portable core validation

The combined `gateway_test.c` runs without the MCU SDK or TinyUSB. The firmware
CMake native preset builds and registers it with CTest. It covers protocol bounds,
stale traffic, stable HID slots, ownership, timers, and the persistent registry.
The source-binding tests use CAN node IDs outside 1–10, reject another source
on a valid application route, renew leases after source migration and invalidate
USB completions during a collision. Historical `can-fd-v1-vectors.json` and
`can-fd-v2-vectors.json` remain archived protocol fixtures, unused by active tests.
The Python peer's ctypes bridge links the actual shared Cyphal stack; its tests
exercise generated profiles, source metadata, transfer-ID deduplication and
SocketCAN backpressure without maintaining a second wire codec. Canonical protocol
conformance tests additionally exercise the shared transport and cross-firmware routing.

## Flash journal validation

`journal_test.c` tests the actual portable journal implementation using a flash
backend that permits erase and 1-to-0 doubleword programming. It interrupts every
erase/program operation, including a partial erase and partial commit doubleword,
for empty, one-byte, 500-byte registry, and maximum-size payloads. The previous
committed page must remain loadable after every interrupted write. Tests also
check readback corruption, CRC fallback, blank versus corrupt nonblank pages,
bounds and a successful retry. STM32 erase timing, option-bank configuration,
endurance and brownout behavior remain bench checks.

The two gateway suites and shared Cyphal transport suite are included in
`cmake --preset native`, and the Arm board
build produces ELF/HEX/bin with strict application warnings. CI uses unallocated
USB IDs solely for compilation; it does not flash hardware or validate USB
signal integrity, CAN cabling, jack pin orientation, host collection mapping,
or power behavior. See the board target README for the hardware test sequence.

The behavior suite also tests 128 buttons, four packed hats, eight axes, exact
maximum HID bit counts, invalid hat/button padding and legacy registry upgrades.
Delivery tests ensure that successful USB completion acknowledges the immutable
submitted report rather than newer CAN input, and reject stale run/ownership/
freshness/session/lease contexts, wrong callback bytes/IDs/lengths and neutral
fallbacks. Machine console tests cover decimal overflow, guarded mutation epochs,
persistence failure before apply and bounded JSON output.
Configuration tests additionally reject stale boot/epoch/source/lease/request
contexts and reordered/duplicate reply chunks, and verify response-drain and
250 ms descriptor reconnect boundaries. The shared transport suite covers all
seven subjects across ten source nodes within its fixed allocator.

The adjacent Cockpit native emulator links the production gateway console,
forwarder, registry/layout, module configuration/acquisition and Cyphal code.
Its sanitizer acceptance tests exercise all controls, calibration, writer
ownership, descriptor/report lengths, persisted/volatile reboot, interrupted
flash, cached replies and process restart. See
`cockpit/simulator/native-gateway/README.md` in the sibling checkout.

`osg_gateway_test --cockpit-vectors` emits actual formatter responses for
discovery, pending registration, maximum active input, direct USB, offline slots,
stale epoch and profile changes. Cockpit Rust tests consume their canonical
fixtures. The Python suite also parses fresh C responses and verifies report
length/neutral values against each returned observed profile.
