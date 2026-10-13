# Native gateway simulation

This directory runs the gateway's production C firmware on macOS or Linux. It owns the simulated CAN bus and presents the normal gateway discovery and CDC protocol to Cockpit. Device instances are separate processes built by the `device-template` repository; Cockpit can already be running when the gateway and devices start.

The production gateway registry, flash journal, configuration forwarding, CAN transfer handling, HID descriptor generation, and layout detach/attach lifecycle are linked directly into the executable. The simulation supplies a monotonic clock, file-backed flash, loopback CAN transport, and a local CDC connection. HID delivery completes inside the native process. It does not publish devices to the host operating system or emulate a physical USB bus, so simulated controllers do not appear in `hidutil list`.

## Run independently

From this repository:

```sh
python3 simulation/run.py
```

The first run builds `.simulation/build/osg-gateway-sim`. Python 3, Git, and a C11 compiler are required; `CC` can select the compiler. The build clones `firmware-common` at the revision in `firmware/cmake/common-revision.cmake` into `.simulation/deps/firmware-common` and initializes its submodules. It uses the Cyphal sources already maintained in this repository. It needs neither Cockpit nor a sibling checkout.

The default gateway identity is `00112233445566778899aabb`. To run another gateway:

```sh
python3 simulation/run.py --identity 1234567890abcdef12345678
```

Start device instances using the commands and JSON examples in the [device-template simulation guide](https://github.com/opensimgear/device-template/tree/main/simulation). Their `--gateway` identity selects which local gateway they join. A device may also start first and wait for its gateway.

Cockpit discovers the gateway normally, then displays devices as they announce themselves. New devices automatically enroll, save and publish their controllers even with Cockpit closed. Cockpit configures modules and explicitly enables/disables or forgets remembered devices. Enabled offline ghosts retain neutral reports; disabled identities remain disabled across announcements and restarts. Closing Cockpit leaves the gateway and devices running. `Ctrl+C` stops this gateway only.

## Build and state

```sh
python3 simulation/build.py --sanitize
python3 simulation/run.py --rebuild --state-dir .simulation/state/my-gateway
```

The wrapper stores persistent gateway registry and epoch data in `.simulation/state/<identity>/`. Reusing this directory restores the registry and advances the epoch after restart. Different running gateways must have different identities and state directories; file locks reject duplicate instances before flash data can be changed.

Build automation can supply dependency sources and output directories explicitly:

```sh
python3 simulation/build.py \
  --common-source /path/to/firmware-common \
  --output-dir /path/to/build \
  --sanitize
/path/to/build/osg-gateway-sim \
  --identity 1234567890abcdef12345678 \
  --state-dir /path/to/state
```

An explicit dependency source is used as supplied. The normal build checks that its cached dependency remains at the firmware pin; remove that cached checkout to fetch a changed pin. `run.py --common-source` is used only when a build is needed; add `--rebuild` to force recompilation after source changes.

## Gateway diagnostics

The process accepts diagnostic commands on its own stdin. These commands are unavailable through the CDC endpoint used by Cockpit:

```text
sim 1 inspect
sim 2 drop_reply 1
sim 3 drop_reply 0
sim 4 fault 0
sim 5 reboot
```

Each reply is one JSON line with the request `id`, `ok`, `error`, and a `simulation` object. `inspect` includes the current gateway epoch, descriptor bytes, report bytes, registry generations, CAN frame count, and flash operation counts.

`drop_reply 1` suppresses configuration responses for timeout tests. `fault N` interrupts the registry's next erase/program operation after N successful operations. `reboot` reloads the persisted registry and advances the epoch. `disconnect 1` pauses the gateway's firmware running state, and `disconnect 0` resumes it. `advance N` waits N milliseconds while the native clock and CAN handling continue.

## Tests

```sh
python3 simulation/test_gateway.py
```

`test_gateway.py` builds with AddressSanitizer and UndefinedBehaviorSanitizer, then tests gateway discovery and CDC validation, independent client/process lifetimes, restart persistence, duplicate identity and state locks, explicit 32-peer CAN admission/overflow diagnostics, malformed bus packets, and unread-client backpressure. Logs and test state live below `.simulation/test-runs/`. An existing build can be selected with `--binary /path/to/osg-gateway-sim`; `--artifacts-dir /path/to/research` keeps test scratch in a chosen directory.

Run `python3 tests/e2e/run.py --sanitize` from the repository root for full integration;
add `--with-cockpit` to clone and test Cockpit too. See the [E2E guide](../tests/e2e/README.md).
The repository's E2E runner separately provisions the real device simulation and exercises production CAN discovery, configuration, layouts, input delivery and restart recovery. E2E dependency checkouts, builds and logs belong to this repository's ignored `.e2e/` directory.

## Local transport contract

Runtime discovery lives in the current user's private `/tmp/opensimgear-<uid>/gateways/` directory. `<gateway-identity>.json` advertises protocol 3, role `gateway`, serial number, process ID, and a loopback TCP port. CDC lines and gateway responses use the same protocol as hardware. Discovery is removed while applying a layout and on orderly exit; clients reject advertisements belonging to dead processes.

`<gateway-identity>.can.json` advertises a protocol-1 loopback UDP CAN broker. Devices send a 92-byte wire packet, independent of host struct layout:

| Bytes | Contents |
|---|---|
| 0–3 | ASCII `OSGB` |
| 4 | Transport format `1` |
| 5 | Kind: hello `1`, admission reply `2`, CAN frame `3` |
| 6 | Frame flags: extended `1`, FD `2`, BRS `4` |
| 7 | CAN payload length, at most 64 |
| 8–11 | Little-endian CAN identifier, or gateway epoch in a reply |
| 12–27 | Device identity in hello packets |
| 28–91 | CAN bytes; hello starts with the little-endian process ID; reply starts with admission status |

Admission status is `0` for accepted, `1` for duplicate identity, and `2` for a full bus. Devices renew admission every 500 ms. Peers expire after 2 seconds or process exit. Frames are delivered to the production gateway core and relayed to other peers, allowing normal Cyphal node collision handling. Transmit deadlines are checked against each process's local clock before transmission.
