# Volatile CAN controller publication validation — 2026-10-17

The four production native C suites pass under AddressSanitizer/UndefinedBehaviorSanitizer
and again under GCC coverage. Every gated source exceeds 70% line coverage:
gateway 97.45%, parser 95.24%, configuration 91.18%, layout 98.36%, publication
95.81%, registry 100%. `firmware/tests/publication_validation.py` reproduces this
without fetching dependencies, opening devices or flashing hardware.

The tests compare configured flash bytes/counters across override and restoration,
actual HID descriptor collections (including CDC-only zero HID), enabled and
disabled originals, unrelated persisted arrivals, saved-arrival cancellation,
immediate delivery revocation, stale callbacks/transitions, unknown/newer-state
preservation (including an independent configured writer before release or IO),
lost native confirmation, wrap-safe autonomous expiry, bounded
expected reconnect, true transport/module-boot revocation and all 32 compact
mask targets within the existing command bound. Read-only owner inspection
rejects a foreign owner's indistinguishable same-revision/masks claim after a
lost reply, without extending its TTL. Temporary-enabled held input appears in
actual snapshot reports even when persistent intent is disabled.

Canonical real-C cross-firmware integration passes the existing three-seed mixed
fault/stress corpus, input/ACK guards and panel-event/output contracts with
sanitizers. The isolated production gateway simulator boundary passes 7/7 tests.
Evidence is under `.research/publication-native/`; production formatter vectors
are in the protocol repository's `test-vectors/controller-publication-override-v1.json`.
This run did not cross-build the Arm board, enumerate actual USB controllers,
flash hardware or change any physical qualification status.

## Radio contract validation — 2026-10-14

All three sanitizer native suites pass with generic string/axis envelope checks.
Independent gateway/device process acceptance passes 15 checks, adding actual
firmware port buffers, ASCII/UTF-8 geometry/bounds, callback failures, maintained
switches/encoders and independent calibrated/stale axes under disabled/direct HID.
Reads and output writes leave flash, descriptors and reconnect counters unchanged.
The starter exposes no unfitted LCD. Canonical snapshot/CDC conformance remains
unchanged; display/axis metadata is additive within the existing sealed envelopes.

Arm GNU 15.3.1 cross-builds with 54,976 bytes application flash and 107,968 bytes
RAM including reserved stack/heap. Evidence lives under workspace
`.research/panel-outputs/`. No hardware was flashed or bench tested.

## Panel-event routing validation — 2026-10-13

The separate event CDC/CAN dispatcher passes all three native suites with
AddressSanitizer/UndefinedBehaviorSanitizer. Its production parser/forwarder
coverage is 96.97%/90.41%, above the maintained 70% threshold. Independent
process acceptance passes 14 checks, including short press/release retention,
immutable retries, fresh event pages, stale boot/stream guards and disabled/direct
USB event access. Canonical integration runs the real common collector and
generic handler over sealed Cyphal envelopes, including ring overflow and
configuration activation. No settings or descriptor mutation is caused by reads.

Arm GNU 15.3.1 produced ELF/bin/HEX/map with 54,936 bytes application flash and
107,968 bytes RAM including reserved 16 KiB stack and 512-byte heap. Evidence
lives under workspace `.research/panel-events/`; no hardware was flashed or
electrically tested.

## Automatic controller lifecycle validation — 2026-10-13

The CDC3 automatic-enrollment implementation passes the checks below. The older
baseline follows for historical context; no assembled gateway or Windows game
binding qualification has been performed.

| Check | Result |
| --- | --- |
| Portable gateway behavior, flash journal and shared Cyphal transport | 3/3 passed, including AddressSanitizer/UndefinedBehaviorSanitizer |
| Native Cyphal peer and independently parsed current C snapshots | 10/10 passed |
| Native gateway boundary, AddressSanitizer/UndefinedBehaviorSanitizer | 7/7 passed, including 33rd-peer capacity diagnosis and CDC-only empty state |
| Independent production gateway/JSON-device processes, sanitizers | 12 checks passed: auto append, routed configuration, save failure, input/ACK, collision, offline ghosts, disable restart, re-enable append, forget/rediscovery and zero-HID CDC configuration |
| Maximum 32-device CDC snapshot | 16,003 bytes before optional 4 KiB configuration response; 32 KiB writer |
| STM32G0B1CB cross-build, Arm GNU 15.3.1 | ELF/bin/HEX/map produced; application warnings treated as errors |
| Linker bounds | 65,156/126,976 bytes flash; 108,304/131,072 bytes RAM, including reserved 16 KiB stack and 512-byte heap |

Behavior tests preserve existing descriptor prefixes while appending, retain
explicit report IDs through sparse v1/v2 migration, neutralize staged-disabled
inventory, and check failed-save reversion, frozen transaction generations,
batching/backoff and bounded unread-host grace. Journal fault injection covers
every interrupted operation for the current 1,556-byte registry, legacy 500-byte
records and maximum 1,984-byte payloads. Disabled metadata changes persist without
reconnecting USB. Publication tests cover old input/completion rejection, offline
return and late boot renewal after save, reserved capacity and lease exhaustion
before persistence.

Evidence lives in workspace `.research/gateway-auto/`. The independent acceptance
result is `e2e/runs/gateway-76vojdmw/result.json`; native boundary results are under
`boundary/gateway-8v33wevv/`. These establish software behavior only. Windows
single/multiple collection transitions, actual game bindings, bus/power limits,
flash brownout/endurance and stack watermark remain physical acceptance.

## Previous baseline — 2026-10-12

The current gateway source passes the following software checks. No assembled
gateway, physical CAN adapter or USB host compliance equipment was used.

| Check | Result |
| --- | --- |
| Native Debug build, AppleClang 21, actual generated DSDL and libcanard transport linked | Passed |
| CTest gateway behavior, flash journal and actual Cyphal transport suites | 3/3 passed |
| Python native Cyphal peer tests, SocketCAN retry and fresh machine snapshots | 10/10 passed |
| Native transport round trips | All seven application types, FD/Classic fragmentation, source metadata, transfer-ID duplicates and maximum profiles passed |
| Production-C native emulator, AddressSanitizer/UndefinedBehaviorSanitizer | Five acceptance flows passed: configuration/calibration/layout, maximum 128-button/8-axis/4-hat settings, first-user-save interruption, process persistence and initial-baseline recovery |
| Adjacent gateway/module integration, AddressSanitizer/UndefinedBehaviorSanitizer | Ten clients, ownership/collision/reboot cases and three fixed stress seeds passed |
| STM32G0B1CB cross-build, Arm GNU 15.3.1, MinSizeRel | ELF, binary, HEX and map generated |
| Linker memory bounds | 51,528/126,976 bytes application flash; 76,672/131,072 bytes RAM |

RAM usage includes the reserved 16 KiB stack and 512-byte heap. The final 4 KiB
of flash is reserved for two journal pages and is absent from application image
data. Stack-usage reports are generated; dynamic stack watermark measurements
remain a board test.

The cross-build used clean local source clones under the workspace `.research`
directory, using the shared canonical Cyphal snapshot and local board dependencies recorded
in `THIRD_PARTY.md`. USB IDs
`0xFFFF:0xFFFF` were compile-only placeholders; this build was neither flashed
nor distributed. Configure an actual device with USB identifiers you are
authorized to use.

Behavior tests exercise discovery and bounded allocation, registry slot
stability, layout application, 128 buttons/four packed hats/eight axes, neutral input, ownership, session/lease/sequence
rejection, independent CAN source binding, source migration/collision, wraparound,
timeouts, legacy registry upgrades and corrupt registry data.
Delivery tests capture immutable USB submissions and reject completion after
run/ownership/freshness/session/lease changes, or with incorrect callback
report IDs, lengths or bytes. Machine commands test correlation parsing, decimal
overflow, stale mutation epochs, persistence failure and bounded JSON.
Fresh C snapshots cover discovery, staged registration, maximum active input,
direct USB, offline retained slots, stale epoch and observed profile changes. Journal tests interrupt
every erase/program operation across several payload lengths, verify the commit
marker and CRC guards, and reject corrupt nonblank storage. These simulate flash
operations; they do not establish actual MCU power-loss behavior or endurance.

To reproduce the native gateway and peer checks from the gateway directory:

```sh
cmake -S firmware -B ../.research/gateway-native \
  -DOSG_BUILD_BOARD=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build ../.research/gateway-native --parallel
ctest --test-dir ../.research/gateway-native --output-on-failure
OSG_GATEWAY_TEST=../.research/gateway-native/osg_gateway_test \
  python3 -m unittest discover -s tools -p 'test_*.py' -v
```

Physical acceptance still includes boot/reset and fault recovery; actual flash
interruption; CAN bit timing, ACK, termination and hot-plug behavior; USB VBUS
attachment and suspend/resume; and joystick collection handling on Windows,
Linux, macOS and target games. The matching generic module and native Cockpit adapter now share the documented
Cyphal/CAN application types and CDC v2 snapshots; assembled end-to-end testing remains
outstanding. Runtime calibration/settings now run through the same gateway CDC,
Cyphal forwarding and module C handlers in native emulation. Tests cover draft
ownership/expiry, invalid mappings, Unicode names, descriptor activation,
calibrated reports, retained slots, interrupted settings/registry writes,
stale input/completion rejection, reply replay without extra flash writes,
bounded timeout recovery and restoration across process restart.
Use the [reference peer](../tools/README.md) for the first CAN bench test.

Source review fixed the DTR-open race: the first incoming machine command is
preserved rather than flushed on open. The close transition still flushes input.
USB completion checks freshness at callback time and gates VBUS/mount/suspend
before acknowledging. CAN recovery explicitly resets the peripheral because
the pinned HAL’s weak MSP deinitialization does not clear hardware FIFOs.
These adapter details compile; their electrical/timing behavior is not measured.

The active hardware adapter accepts extended Cyphal frames broadly and preserves
Classic/FD/BRS metadata. TX queue admission requires an exact legal DLC so driver
padding cannot move the Cyphal tail byte. Transfer deadlines propagate into the
board queue; expiry cancels pending hardware transmission with a separate bounded
abort timer. Node-ID migration flushes software and controller queues and revokes
leases before fresh input resumes. These HAL paths cross-compile successfully;
actual cancellation, collision recovery and bus-off timing remain bench tests.
