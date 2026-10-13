# Gateway assembly and enclosure

The [v0.1.0 gateway PCB](../electronics/boards/gateway/0.1.0/README.md) is shared electronics for possible Lite/Pro enclosures. An enclosure has not been designed. Use the native board's mounting holes and exact component body/connector drawings as the mechanical source; do not infer dimensions from a copper preview or a generic RJ11 model.

Provide access to USB-C, both 4P4C cable latches, the 5.5 × 2.1 mm centre-positive 12 V socket, reset and the SWD header. Retain room for a removable 120 Ω endpoint plug in the unused CAN socket when the gateway is at an end. Leave clearance above the switching regulator, polarized capacitor and modular jacks, and keep screw heads away from copper and solder joints. No conductive enclosure or chassis-ground connection has been qualified.

## First assembly

1. Inspect the exact BOM and manufacturer-derived footprints. Compare connector hole locations, contact numbers, panel position and body heights against purchased samples before ordering boards. Check all proposed substitutions independently.
2. Inspect isolation, polarity, solder bridges and passive port-to-port continuity with power off. Contacts 1–4 must map identically between the two sockets; contacts 2/3 have no fitted onboard terminator. Check that the barrel centre reaches the protected 12 V path and its sleeve reaches ground.
3. Initially use a regulated 12 V current-limited bench supply, no modules and no USB host. Verify local 5 V/3.3 V rails, current draw and temperature. Test the protected injection path with controlled loads according to the electrical review.
4. Test USB-only and simultaneous-source operation, rail handover, USB backfeed and detach while externally powered. USB-only must leave BUS_12V unpowered. Complete the firmware guide's SWD and connect-under-reset recovery checks.
5. Connect one matching CAN peer using verified wiring and exactly two endpoint terminators. Confirm discovery, registration, reports, unplug neutralisation and direct-USB ownership before expanding the chain. Keep total current within the provisional bench limit; increase limits only after documented cable/connector/thermal qualification.

The board is a prototype candidate; these are required physical acceptance steps, not recorded test results. Keep measured conditions, instruments and results with the PCB's validation record. An enclosure revision must record its compatible PCB revision and firmware board target.

- [Lite](variants/lite/README.md): reserved 3D-printed edition.
- [Pro](variants/pro/README.md): reserved alternative construction.
