# Pico W USB simulation

This optional physical USB fixture belongs to `device-gateway`. Its RP2040 Pico W firmware exposes up to eight joystick interfaces, each with 32 buttons and six axes. Its separate protocol 1 supports inventory and input injection through the tools here. The independently running CAN gateway/device simulations are documented in [the parent guide](../README.md).

Cockpit can discover the flashed Pico, configure module settings, monitor inputs and reconnect its USB layout using its normal Pico adapter. Input injection and device creation are owned by this simulation fixture. Settings and inventory remain in board RAM until reset; this older fixture does not implement the production runtime configuration or CAN transport.

## Build and flash

From the `device-gateway` repository, initialize the Pico SDK dependency:

```sh
mkdir -p .e2e/deps
git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/pico-sdk.git .e2e/deps/pico-sdk
git -C .e2e/deps/pico-sdk submodule update --init --recursive lib/tinyusb
node simulation/pico/firmware/build.mjs
```

Install CMake and an Arm GNU embedded compiler first. Set `PICO_SDK_PATH`, `OSG_CMAKE` or `ARM_GNU_BIN` when using dependencies outside their default locations. The build writes `simulation/pico/firmware/build/osg-pico-gateway.uf2`, its SHA-256 checksum and a firmware manifest. It never flashes connected hardware and does not copy firmware into Cockpit.

Hold BOOTSEL while attaching an RP2040 Pico W with a USB data cable. Copy the UF2 to the `RPI-RP2` drive; the board restarts automatically. Repeating BOOTSEL allows recovery. This firmware targets Pico W, not Pico 2 W.

Cockpit discovers the flashed gateway automatically. USB interfaces share the board serial; match each HID interface to the module inventory. On macOS, Cockpit's ordinary native adapter applies individual controller names through IOKit. Reopen applications that cached old names after a layout change.

## Tests and tools

The host C tests and protocol/bridge tests can run without attached hardware:

```sh
node simulation/pico/firmware/test.mjs
node --test simulation/pico/pico-bridge.test.mjs simulation/pico/pico-integration.test.mjs simulation/pico/pico-hid-names.test.mjs simulation/pico/pico-hid-names-helper.test.mjs
```

The Swift helper checks require macOS command-line developer tools. They validate requests without touching HID hardware. `verify-hid.swift` is an independent physical macOS HID observer. Build and run it from this repository to check an attached board:

```sh
mkdir -p .simulation/pico/module-cache
xcrun swiftc -module-cache-path .simulation/pico/module-cache simulation/pico/verify-hid.swift -o .simulation/pico/verify-hid
.simulation/pico/verify-hid --watch --timeout 15 --serial OSG-PICO-YOUR-BOARD-SERIAL
```

The optional `pnpm --dir simulation/pico verify` command uses the same repository-owned output directory.

For the optional legacy protocol browser fixture, initialize `simulation/pico/package.json` with `pnpm --dir simulation/pico install`. Set `COCKPIT_SOURCE` to an initialized Cockpit checkout (by default `.e2e/deps/cockpit` inside this repository), then run `pnpm --dir simulation/pico ui:fixture`. This isolated fixture uses the production Pico C parser over pipes and never opens physical hardware. The main cross-repository acceptance uses the parent gateway simulation and JSON device instances instead.

CDC protocol 1 carries bounded newline-delimited JSON requests with `id` and `command`. Commands include `hello`, `status`, `start`, `stop`, `input`, `presence`, `configure`, `add`, `register`, `remove` and `reconnect`; replies echo `id` and contain authoritative state. Stop, disconnect, lost serial access and the firmware watchdog neutralize reports. Production CAN settings, flash persistence and physical sensor emulation belong to the independent gateway/device simulations.
