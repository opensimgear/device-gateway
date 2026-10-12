# Firmware validation — 2026-10-12

The current gateway source passes the following software checks. No assembled
gateway, physical CAN adapter or USB host compliance equipment was used.

| Check | Result |
| --- | --- |
| Fresh native Debug build, AppleClang 21, AddressSanitizer and UndefinedBehaviorSanitizer | Passed |
| CTest gateway behavior and flash journal suites | 2/2 passed |
| Python reference peer tests, including mocked live transmit recovery and fresh machine snapshots | 7/7 passed |
| Fresh C codec output against stored fixtures and Python encoding | Five message types and maximum-profile frames match byte for byte |
| STM32G0B1CB cross-build, Arm GNU 14.3.1, MinSizeRel | ELF, binary, HEX and map generated |
| Linker memory bounds | 34,528/126,976 bytes application flash; 33,824/131,072 bytes RAM |

RAM usage includes the reserved 16 KiB stack and 512-byte heap. The final 4 KiB
of flash is reserved for two journal pages and is absent from application image
data. Stack-usage reports are generated; dynamic stack watermark measurements
remain a board test.

The cross-build used clean local source clones under the workspace `.research`
directory, matching every dependency revision in `THIRD_PARTY.md`. USB IDs
`0xFFFF:0xFFFF` were compile-only placeholders; this build was neither flashed
nor distributed. Configure an actual device with USB identifiers you are
authorized to use.

Behavior tests exercise discovery and bounded allocation, registry slot
stability, layout application, 128 buttons/four packed hats/eight axes, neutral input, ownership, session/lease/sequence
rejection, wraparound, timeouts, legacy registry upgrades and corrupt registry data.
Delivery tests capture immutable USB submissions and reject completion after
run/ownership/freshness/session/lease changes, or with incorrect callback
report IDs, lengths or bytes. Machine commands test correlation parsing, decimal
overflow, stale mutation epochs, persistence failure and bounded JSON.
Fresh C snapshots cover discovery, staged registration, maximum active input,
direct USB, offline retained slots, stale epoch and observed profile changes. Journal tests interrupt
every erase/program operation across several payload lengths, verify the commit
marker and CRC guards, and reject corrupt nonblank storage. These simulate flash
operations; they do not establish actual MCU power-loss behavior or endurance.

To reproduce the native sanitizer check:

```sh
cmake -S firmware -B /tmp/osg-gateway-native \
  -DOSG_BUILD_BOARD=OFF -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/osg-gateway-native --parallel
ctest --test-dir /tmp/osg-gateway-native --output-on-failure
OSG_GATEWAY_TEST=/tmp/osg-gateway-native/osg_gateway_test \
  python3 -m unittest discover -s tools -p 'test_*.py' -v
```

Physical acceptance still includes boot/reset and fault recovery; actual flash
interruption; CAN bit timing, ACK, termination and hot-plug behavior; USB VBUS
attachment and suspend/resume; and joystick collection handling on Windows,
Linux, macOS and target games. The matching generic module and native Cockpit adapter now share the documented
CAN FD v2 codec and snapshot fixtures; assembled end-to-end testing remains
outstanding. Remote calibration/settings and firmware updates remain outside
this inventory/registration contract.
Use the [reference peer](../tools/README.md) for the first CAN bench test.

Source review fixed the DTR-open race: the first incoming machine command is
preserved rather than flushed on open. The close transition still flushes input.
USB completion checks freshness at callback time and gates VBUS/mount/suspend
before acknowledging. CAN recovery explicitly resets the peripheral because
the pinned HAL’s weak MSP deinitialization does not clear hardware FIFOs.
These adapter details compile; their electrical/timing behavior is not measured.
