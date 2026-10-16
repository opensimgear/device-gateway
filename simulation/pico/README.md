# Pico W debug gateway

## Development status and disclaimer

This project is heavily under development and is not ready for use. Use it entirely at your own risk. To the fullest extent permitted by applicable law, the authors and contributors disclaim all liability for any damage, loss, or other issues arising from its use.

The RP2040 Pico W is a self-contained USB debug gateway for Cockpit, hosting up to eight virtual devices. It reuses production gateway registry/lifecycle/HID logic and shared device acquisition, calibration, configuration, ordered events and actions. Its internal application-message bus exercises software behavior; CAN electrical and Cyphal frame transport remain separate native/STM32 tests.

The current firmware uses CDC protocol 3 with an explicit Pico debug capability. Cockpit retains compatibility with older protocol-1 images. The old parser and USB-layout tests remain compatibility fixtures; `build.mjs` builds the current target.

## Build and flash

Install Node.js, Git, CMake and an Arm GNU embedded toolchain with C and C++
headers/libraries. On Ubuntu, the compiler/Newlib packages also need
`libstdc++-arm-none-eabi-dev` and `libstdc++-arm-none-eabi-newlib` because the Pico
SDK compiles its C++ allocation support even for this C application. Then run from this repository:

```sh
node simulation/pico/firmware/build.mjs
```

The builder provisions full-commit pinned Pico SDK, firmware-common and device-template checkouts in `.research/deps/`, initializing SDK TinyUSB automatically. `PICO_SDK_PATH`, `OSG_COMMON_SOURCE`, `OSG_DEVICE_SOURCE`, `OSG_CMAKE` and `ARM_GNU_BIN` select explicit development sources/tools. Default caches must match their pins.

Outputs are `.research/pico/build/osg-pico-gateway.uf2`, its SHA-256 checksum, `firmware-manifest.json` and `memory-audit.json`. Eight virtual devices fit the measured memory budget. The linker and builder guard the reserved 72 KiB journal region; the target reserves a 24 KiB stack. Building never flashes hardware.

Hold BOOTSEL while attaching a Pico W with a USB data cable and copy the UF2 to `RPI-RP2`. BOOTSEL provides recovery. This target is Pico W, not Pico 2 W. The SDK-derived USB serial is preserved exactly, including casing; the protocol UID is separately canonicalized. The new single HID interface uses report IDs and controller collections. Upgrading the older multi-interface fixture changes descriptors and can require rebinding games.

Gateway registration/controller order, virtual preset inventory and saved device configuration survive ordinary resets and UF2 updates preserving the reserved flash region. Raw samples, temporary output actions and endpoint leases reset. Full flash erase discards saved state. Corrupt nonblank journals fail closed; interrupted writes retain the previous committed record.

## Cockpit controls

Cockpit discovers the board automatically. Its gateway card provides **Pico virtual devices**, including Add, online/offline and reboot controls. Initial devices are shifter, pedals, handbrake and button panel. Additional presets are master caution, radio panel, all input types and output-only rev lights. The all-inputs preset fits 128 logical buttons, eight axes, four hats, two encoders, a selector and expansion contacts. The output-only preset has zero inputs.

Open a device's Inputs or Setup view to use **Pico debug controls**:

- Hold/release momentary contacts or toggle maintained switches.
- Move raw ADC axes from 0–4095 with sliders/numeric values and inspect calibrated 0–65535 samples. Inputs work during Setup.
- Turn encoders by ordered detents, operate hats and select low/center/high positions.
- Reset inputs, simulate direct USB ownership or toggle expansion-bank availability.
- Inspect applied LED/display readback and invoke ordinary advertised device actions.

Pending axes coalesce; button edges and encoder commands retain order. Reset releases inputs, cancels partial turns and accounts for inverted calibration. Pointer/keyboard cancellation, blur, page exit, serial loss and watchdog expiry provide release/neutral recovery. Acquisition/events remain active with HID disabled. Controller Enable/Disable/Remove, offline ghosts and profile updates use the normal lifecycle. Forgotten offline virtual devices can be revived from the debug roster.

## DCS-BIOS

Add a DCS master-caution or radio panel and open Cockpit's DCS-BIOS view. Enter the real installed `doc/json` catalogue directory, refresh the selected panel's capabilities and choose its A-10C test preset. Named commands/interfaces are checked against that catalogue; presets supply no binary feedback addresses. Save and Start are explicit.

Disable each input panel's HID controller and wait for publication to finish. Other joystick devices can remain enabled. End setup sessions and turn off simulated direct USB ownership for bound inputs. Presets currently target A-10C/A-10C II and use the existing editable profile workflow.

Master caution connects a momentary acknowledgement to `UFC_MASTER_CAUTION` and LED feedback to `MASTER_CAUTION`. Radio covers `UHF_TEST`, `UHF_SQUELCH`, `UHF_PRESET_SEL`, `UHF_10MHZ_SEL`, `UHF_VOL`, frequency/preset displays, formatted volume text and a caution LED. Its outputs are 7-cell and 2-cell ASCII displays plus a 16-cell UTF-8 display. Cockpit shows desired/applied values and command/traffic status.

Live testing requires Windows DCS and the aircraft module. Software acceptance uses Cockpit's authored UDP peer and synthetic catalogue, whose addresses must not be used with a live aircraft. Physical Windows HID/bindings and live DCS qualification are separate from software-peer results.

## Tests and host fixture

After initializing build dependencies:

```sh
node simulation/pico/firmware/test.mjs
node --test simulation/pico/pico-bridge.test.mjs simulation/pico/pico-integration.test.mjs simulation/pico/pico-hid-names.test.mjs simulation/pico/pico-hid-names-helper.test.mjs
```

Tests cover legacy compatibility, UF2 encoding, interrupted storage, production inputs/config/lifecycle/actions/events, stale boots, partial-turn cancellation and failed-add recovery with sanitizers. Clang and matching LLVM tools are required for new coverage gates, each enforcing line coverage above 70%. Set `OSG_COVERAGE_CLANG`, `OSG_LLVM_COV` and `OSG_LLVM_PROFDATA` for versioned Linux tools. Host binaries and test outputs live in `.research/pico/`.

For ordinary Cockpit development without a board:

```sh
python3 simulation/pico/debug-fixture.py --namespace pico-debug
```

Set `OSG_SIMULATION_NAMESPACE=pico-debug` on Cockpit's native service. The fixture owns the actual firmware host process and advertises a private loopback endpoint; Cockpit only connects. It does not enumerate OS HID. Host settings are volatile, so use **Apply** for calibration. Persistent journals are exercised by the C tests and physical board firmware.

For isolated browser acceptance also set `OSG_LOCAL_GATEWAYS_ONLY=1` on the debug native service and `OSG_DCS_BIOS_CONFIG` to an absolute scratch settings file. Release builds ignore local-only mode. Tests then avoid unrelated physical gateways and application settings.

Cross-process acceptance starts an isolated fixture, native service and DCS peer:

```sh
python3 simulation/pico/debug-acceptance.py --service /absolute/path/to/osg-gateway-service --cockpit-source /absolute/path/to/cockpit
```

It verifies discovery, raw/max-capacity inputs, configuration, HID-disabled events, DCS commands, LED/display feedback, stale fallback and reboot guards. Reports live in `.research/pico/acceptance/`. Build the service with `CARGO_TARGET_DIR=/absolute/research/path cargo build --manifest-path /absolute/path/to/cockpit/src-tauri/Cargo.toml --bin osg-gateway-service`.

## Debug protocol

ASCII `hello` advertises Pico role, CDC/application protocol versions, canonical UID, preserved serial, epoch and capacity. Normal `cockpit ...` lifecycle/config/action/event commands retain production contracts. Every response includes a bounded debug roster with raw samples and active definitions.

Injection uses `debug <id> <epoch> <device-identity> <boot> <operation> [arguments]`. Gateway-scoped Describe/Add use identity `-` and boot `0`. Operations are:

| Operation | Arguments |
| --- | --- |
| `describe` | None; gateway-scoped |
| `add` | Preset name; gateway-scoped |
| `contact` | Physical index 0–151, closed 0/1 |
| `axis` | Channel 0–7, raw 0–4095 |
| `turn` | Advertised encoder ordinal, nonzero detents -32–32 |
| `hat` | Advertised ordinal, direction -1 neutral or 0–7 clockwise from north |
| `selector` | Advertised ordinal, low/center/high 0/1/2 |
| `expansion` | Bank 0–7, available 0/1 |
| `direct` / `online` | Boolean 0/1 |
| `neutral` / `reboot` | None |

Firmware validates epoch, identity and boot before mutation. Cockpit validates advertised arguments and sends uncertain mutations once. Templates retain fixed fixture identities scoped by gateway identity.

## Exclusive output leases

The `output-only` preset uses the same native/physical device RGB and boolean
action callbacks with no inputs. It enrolls in normal inventory with no HID report
ID or controller order. Existing input devices keep their collections. All virtual
registries expose shared endpoint claims/renewal/owned invocation/release and
paged public ownership; normal action aliases and diagnostic overrides respect
the endpoint lock. Claims use private UUID tokens and 250–5000 ms TTLs.

Independent virtual ticks expire leases even when a device is offline or HID is
disabled. Gateway CDC loss, virtual route changes and offline transitions
invalidate CAN-scoped leases and attempt their validated fallback once; reboot
clears claims and restores volatile defaults. Claims never write journals or
change controller layouts. The software core tests cover competing owners,
independent LEDs, expiry, offline/reboot/disconnect and output-only enrollment.
The [shared ownership contract](https://github.com/opensimgear/protocol/blob/main/serial/output-ownership.md)
also applies to ordinary STM32 devices and native process simulations.
