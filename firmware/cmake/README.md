# Build and dependency configuration

`native` builds portable gateway behavior, flash journal, actual Cyphal transport
tests and the SocketCAN peer shared library. It needs no external source checkout.
Native output goes to repository `.build/native`; board output to
`.build/gateway`. `gateway` builds the STM32G0B1CB v0.1.0 firmware and
requires an Arm GNU toolchain plus explicit authorized `OSG_USB_VID` and
`OSG_USB_PID` values. The default firmware board is `gateway_g0b1cb_v0_1_0`;
unknown targets fail at configure time.

`common-revision.cmake` pins firmware-common to a full commit. That repository
owns the exact TinyUSB, STM32 HAL, STM32 CMSIS, Arm CMSIS, libcanard and protocol
source commits. CMake clones dependencies under the build directory’s
`_deps/`, configurable with `FETCHCONTENT_BASE_DIR`.
The active generated Cyphal types and libcanard transport are maintained in
`shared-cyphal/`, a self-contained snapshot of canonical `protocol/cyphal/`;
the older fetched common protocol/libcanard scaffolding does not implement the
current CAN wire path.

An explicit local common checkout is supported through standard FetchContent:

```sh
cmake --preset gateway \
  -DOSG_USB_VID=<your-vid> -DOSG_USB_PID=<your-pid> \
  -DFETCHCONTENT_SOURCE_DIR_FIRMWARE_COMMON=/absolute/path/to/firmware-common
```

Other cached clones can be supplied with `FETCHCONTENT_SOURCE_DIR_TINYUSB`,
`FETCHCONTENT_SOURCE_DIR_STM32_HAL`, `FETCHCONTENT_SOURCE_DIR_STM32_CMSIS`,
`FETCHCONTENT_SOURCE_DIR_CMSIS_CORE`, `FETCHCONTENT_SOURCE_DIR_LIBCANARD`, and
`FETCHCONTENT_SOURCE_DIR_OSG_PROTOCOL`. Overrides deliberately use the specified
checkout; verify its HEAD against the common pins when reproducing release
builds. Preserve the maintained shared Cyphal snapshot and its upstream notices.
