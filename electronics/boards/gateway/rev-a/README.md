# Gateway Rev A

Open `gateway.kicad_pro` in **KiCad 10**. This revision develops the gateway around the reviewed STM32G0B1 starter core, adding protected 12 V injection and two physical **4P4C** modular CAN/power sockets. The board is a prototype; no assembly has been measured or fabricated. [Validation](VALIDATION.md) records actual static results and remaining qualification gates. [Fabrication](FABRICATION.md), [footprint review](FOOTPRINTS.md), [BOM](bom.csv), [schematic](review/schematic.pdf) and [copper preview](review/pcb.svg) and [1:1 assembly drawings](review/assembly.pdf) accompany the editable CAD.

The connectors are four-position/four-contact jacks, often sold as RJ9/RJ10/RJ22 handset connectors. They are smaller than six-position RJ11/RJ12 sockets. They implement the user's final **4P4C everywhere** requirement. These powered sockets must never connect to telephone equipment. The former six-contact template ecosystem must be migrated before connecting a module; adapters or casual telephone-cord substitutions are not assumed safe.

| Socket / header | Contacts |
| --- | --- |
| J1 USB-C | USB 2.0 device, external 5.1 kΩ Rd on CC1/CC2, no USB PD |
| J2/J3 powered CAN | 1 protected BUS_12V; 2 CAN_H; 3 CAN_L; 4 GND |
| J5 12 V barrel | 5.5 mm OD / 2.1 mm ID mating plug, center positive; sleeve GND; switched sleeve unused |
| J4 SWD | 1 target 3.3 V reference, 2 GND, 3 SWDIO, 4 SWCLK, 5 NRST |
| J6 CAN test | 1 CAN_H, 2 CAN_L, 3 GND; no power output |

Contact numbering comes from the exact manufacturer's drawing and local footprint; see [FOOTPRINTS.md](FOOTPRINTS.md). Qualify a straight-through four-conductor cable whose center pair carries CAN_H/L as a twisted pair. Most handset cords reverse contacts end to end and many are flat, untwisted cable. A reversed cord swaps both power polarity and CAN polarity. Use continuity checks at every contact before energizing any new cable. No hot-plug or cable-current qualification has been established.

J2 and J3 are electrically parallel on a single CAN FD segment. Copper passes CAN and protected trunk power through independently of MCU reset or firmware. Use a linear chain. Fit a removable **120 Ω, 1%, ≥0.25 W** resistor between contacts **2 and 3 only** inside an endpoint plug in the unused gateway port, and a second endpoint plug in the final module's unused port. Remove termination on every intermediate module. There is no permanent PCB terminator; J6 is a bench test connection. Do not form a ring.

The barrel input feeds F2 (350 mA hold / 750 mA trip, 30 V PPTC), D6 SS14 reverse-polarity diode, then D7 SMBJ15A TVS at BUS_12V. Both jacks and the MCU branch receive this protected voltage. F1 (200 mA hold / 30 V) separately protects the MCU branch, followed by D1, the local D5 TVS and RECOM 5 V converter. U6/U7 LM66100 combine regulated bus power and USB VBUS into SYS_5V with TI's Figure 17 continuous-output priority topology: the buck-side status output drives USB-side enable through OR_STATUS, with R6 pulling up to BUCK_5V, followed by the 3.3 V LDO. USB never has a path that generates 12 V. The power-source selection is passive and independent of firmware; leakage, backfeed and source transitions still require measurement. Use one 12 V source per trunk; do not join independently powered gateways.

Use a **current-limited regulated 12 V bench supply, set ≤0.5 A**, and provisionally budget **≤250 mA total** at the barrel input at 20–40°C, including the gateway and all chain devices. The connector's 1.5 A/contact rating does not establish a cable/system current budget. F2's hold rating derates with temperature (300 mA at 40°C and 150 mA at 85°C); a PPTC is a slow fault element, not an accurate electronic current limiter. This prototype has no sustained-overvoltage cutoff. TVS and fuse behaviour under actual faults, wire heating, diode drop and connector temperature must be qualified before extending these limits.

| Firmware signal | MCU pad | LQFP48 pin |
| --- | --- | --- |
| USB D− / D+ | PA11 / PA12 | 33 / 34 |
| USB VBUS presence | PB0, 10 kΩ / 15 kΩ divider | 19 |
| CAN RX / TX | PB8 / PB9, AF3 | 47 / 48 |
| CAN standby, high = standby | PB5, external 10 kΩ pull-up | 44 |
| LED, active high through 1 kΩ | PA5 | 16 |
| SWDIO / SWCLK | PA13 / PA14 | 35 / 36 |
| Reset | PF2-NRST | 10 |

Clocks use internal oscillators. VDD, VDDA, VREF+ and VBAT use 3.3 V; the LQFP48 package internally connects USB supply to VDD. Leave PA11/PA12 remapping disabled and disable UCPD dead-battery pull-downs on unused PA9/PA10. The transceiver is in standby during reset; CAN traffic and chain power continue through the passive connectors. External control headers from the device template are absent.

The four-layer board retains the 65 × 55 mm outline and reviewed USB geometry: 0.2474 mm main track width / 0.1501 mm pair gap above a continuous L2 reference plane. Request the documented JLC04161H-7628 construction, 90 Ω differential ±10%, final geometry approval and coupon/TDR; the calculation is not a measured impedance. See [fabrication instructions](FABRICATION.md). Reproduce static checks and exports with `tools/check-and-export.sh` after every CAD edit.

`KiCad.kicad_sym` / `KiCad.pretty` retain selected upstream library entries under [KiCad's license](LICENSE-KICAD.md). `OpenSimGear` contains local manufacturer-derived connector, PPTC and other reviewed footprints. Upstream 3D paths require KiCad's model installation; newly authored parts deliberately omit unverified models.
