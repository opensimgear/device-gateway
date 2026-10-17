# Firmware validation

The volatile publication owner has a hardware-free production validation runner:
`python3 firmware/tests/publication_validation.py`. It builds all four C suites
with ASan/UBSan, repeats them with GCC coverage, and requires more than 70% line
coverage in the gateway/parser/configuration/layout/publication/registry sources.
It fetches no dependencies and accesses no devices. Publication tests compare
configured flash bytes and counters, actual descriptor collection removal/return,
disabled-original restoration, unrelated arrival persistence, all 32 mask targets,
duplicate immutable claims, stale transition cancellation, lost confirmation,
autonomous expiry, bounded reconnect, real transport/module-boot loss, immediate
delivery revocation, newer-state preservation and unknown-state attention.

## Development status and disclaimer

This project is heavily under development and is not ready for use. Use it entirely at your own risk. To the fullest extent permitted by applicable law, the authors and contributors disclaim all liability for any damage, loss, or other issues arising from its use.

Action routing tests exercise the production parser, bounded prefixing,
current-lease status with direct USB ownership, disabled HID access, fragmented
responses, nested device validation errors, cancellation and timeout. Existing
configuration source/boot/epoch/lease/request guards apply to actions too. The
independent E2E suite checks advertised RGB/boolean LEDs, invalid arguments and
catalogue revisions, applied state, lost replies and unchanged settings/descriptors.

Panel-event routing tests cover the separate CDC verb, bounded `events `
prefix, fragmented JSON and nested gap errors in `targetEvents`. The independent
process test retains a press/release between reads, retries an immutable request
after later edges, reads fresh pages, rejects stale stream/boot and checks direct
USB/disabled HID access without settings, descriptor or reconnect changes.
Canonical cross-firmware integration additionally exercises the production event
collector and device handler over sealed Cyphal request/response envelopes.

The portable suites test observable gateway behavior and interrupted flash
journal writes. See [recorded validation results](../VALIDATION.md). Hardware
acceptance must cover USB HID/CDC enumeration, CAN communication, ownership,
power, disconnect recovery, endpoint and inline placement, and independent
gateways. Build success alone is not hardware validation.

Radio regression checks retain UTF-8 string readback through `targetAction` and
independent axis observations through `targetEvents`. The process acceptance
adds a fourth radio device, disables its HID controller, invokes 7-cell ASCII
frequency/2-cell preset and 16-cell UTF-8 displays, rejects unsupported sizes and
encoding before driver writes, and tests callback failure. Maintained/encoder
records and calibrated/stale axes remain reachable under direct USB ownership.
Module and gateway flash, descriptors and reconnect counters stay unchanged.

## Portable core validation

The combined `gateway_test.c` runs without the MCU SDK or TinyUSB. The firmware
CMake native preset builds and registers it with CTest. It covers protocol bounds,
stale traffic, stable ordered HID collections, ownership, timers, and the persistent registry.
The source-binding tests use CAN node IDs independent of application routes1–32, reject another source
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
for empty, one-byte, legacy500/current1,556-byte registries, and maximum-size payloads. The previous
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
persistence failure before automatic activation and bounded JSON output.
Configuration tests additionally reject stale boot/epoch/source/lease/request
contexts and reordered/duplicate reply chunks, and verify response-drain and
250 ms descriptor reconnect boundaries. The shared transport suite covers all
seven subjects plus32-source input/presence load within its fixed allocator.

The adjacent Cockpit native emulator links the production gateway console,
forwarder, registry/layout, module configuration/acquisition and Cyphal code.
Its sanitizer acceptance tests exercise all controls, calibration, writer
ownership, descriptor/report lengths, persisted/volatile reboot, interrupted
flash, cached replies and process restart. See
`cockpit/simulator/native-gateway/README.md` in the sibling checkout.

`osg_gateway_test --cockpit-vectors` emits actual formatter responses for
discovery, pending automatic enrollment, maximum active input, direct USB, offline retained controllers,
stale epoch and profile changes. Cockpit Rust tests consume their canonical
fixtures. The Python suite also parses fresh C responses and verifies report
length/neutral values against each returned observed profile.

Automatic lifecycle tests cover append-only descriptor prefixes, persistent disable across announcement/reboot, re-enable at the end, offline forget/rediscovery, no HID placeholder, batched exact saved generations, late discovery during detach, save failure/backoff/reversion, neutral staged-disable inventory, and bounded serial backpressure.
Publication tests additionally reject old input and completions, reserve the last
lease capacity against intervening announcements, handle offline return and late
boot renewal, preserve monotonically increasing assignments, and diagnose lease
exhaustion before saving a layout that cannot be activated.

## Action and event routing coverage

Run `python3 firmware/tests/coverage.py` from the repository root with matching
Clang, `llvm-cov` and `llvm-profdata` tools. The runner executes all portable tests
and requires more than 70% line coverage in the CDC parser and module transaction
dispatcher. CI runs the same gate with LLVM 18.

Output-only regression checks valid zero-input profile enrollment, absence of a
HID collection/report ID, Status reachability, rejection of empty Input and Enable,
registry encode/decode bounds, unchanged descriptor activation, and zero↔input
transitions preserving other controllers. The gateway forwards endpoint leases
through ordinary guarded action requests; real-CAN device integration proves
cached claims/renewals cannot extend TTL and expiry/route loss invokes fallback.
Pico debug tests exercise the same handler for output-only/competing writers and
disconnect/offline/reboot. The line gate now also measures `gateway.c` and
`gateway_registry.c`, each required above 70%.
