# Firmware validation — 2026-10-12

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
bounded timeout recovery and restoration across process restart. Firmware updates
remain future work.
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
