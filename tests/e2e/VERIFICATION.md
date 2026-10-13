# Independent E2E acceptance — 2026-10-13

`python3 tests/e2e/run.py --sanitize --with-cockpit` passed with explicit local
working-tree source overrides for the unpublished `device-template` and `cockpit`
changes. The runner created isolated clones, initialized their dependencies,
automatically cloned the matching firmware-common pin, and built both native
simulations plus Cockpit’s normal Rust gateway service under this repository’s
`.e2e/` directory.

Nine gateway scenarios passed: device-before-gateway startup and three JSON
profiles; configuration ownership/save and dynamic layout; interrupted saves and
timeout recovery; live input reports; CAN collision repair; duplicate identity and
state locking; unread client backpressure; offline neutral/restart persistence;
and client-independent firmware lifetimes. The cloned Cockpit acceptance also
passed its Cockpit-first three-device configuration, layout and restart scenario.

A follow-up run also passed the cloned Cockpit simultaneous-gateway scenario:
two gateways exposing the same module identity retained independent saved
configurations, claims, layouts and live inputs. A paused gateway did not block
its peer, and disconnect/restart recovery affected only the targeted gateway.
Passing evidence: `.e2e/runs/gateway-2y7n0ujt/`, `.e2e/runs/cockpit-PInEoZ/`
and `.e2e/runs/cockpit-multi-gateway-NGh2yF/`.

Independent HID parsing verified report lengths 6/3/1 before the profile change
and 7/3/1 afterward. Saved gateway registration and module settings survived
process crashes. Four local-only bootstrap regression tests passed.

Results and CDC logs are retained locally under `.e2e/runs/`; exact clone revisions
and local snapshot hashes are recorded in `.e2e/resolved-dependencies.json`.
[The Cockpit UI acceptance record](https://github.com/opensimgear/cockpit/blob/main/docs/verification/2026-10-13-repository-owned-simulation.md)
also documents the visible native application test using independently launched
processes and a separate saved profile. The [simultaneous gateway record](https://github.com/opensimgear/cockpit/blob/main/docs/verification/2026-10-13-multiple-gateways.md)
adds four concurrently connected gateways in the native UI.

These are native protocol/report tests. They do not establish physical USB/HID
enumeration or electrical CAN acceptance. Default remote-ref runs require the
coordinated repository changes to be published first.
