# Build and dependency configuration

`native` builds only portable gateway behavior and flash journal tests. It needs
no external source checkout. `gateway` builds the STM32G0B1CB Rev A firmware and
requires an Arm GNU toolchain plus explicit authorized `OSG_USB_VID` and
`OSG_USB_PID` values. The default firmware board is `gateway_g0b1cb_rev_a`;
unknown targets fail at configure time.

`common-revision.cmake` pins firmware-common to a full commit. That repository
owns the exact TinyUSB, STM32 HAL, STM32 CMSIS, Arm CMSIS, libcanard and protocol
source commits. CMake clones dependencies under workspace
`.research/gateway-deps`, configurable with `FETCHCONTENT_BASE_DIR`.

An explicit local common checkout is supported through standard FetchContent:

```sh
cmake --preset gateway \
  -DOSG_USB_VID=<your-vid> -DOSG_USB_PID=<your-pid> \
  -DFETCHCONTENT_SOURCE_DIR_FIRMWARE_COMMON=/absolute/path/to/.research/firmware-common
```

Other cached clones can be supplied with `FETCHCONTENT_SOURCE_DIR_TINYUSB`,
`FETCHCONTENT_SOURCE_DIR_STM32_HAL`, `FETCHCONTENT_SOURCE_DIR_STM32_CMSIS`,
`FETCHCONTENT_SOURCE_DIR_CMSIS_CORE`, `FETCHCONTENT_SOURCE_DIR_LIBCANARD`, and
`FETCHCONTENT_SOURCE_DIR_OSG_PROTOCOL`. Overrides deliberately use the specified
checkout; verify its HEAD against the common pins when reproducing release
builds. Do not copy third-party source into the gateway repository.
