#!/usr/bin/env python3
"""Check v0.1.0 gateway electrical intent in the KiCad XML netlist and BOM.

Run from any directory; only the Python standard library is required. Optional
paths allow the same checks to run against a fresh export before replacing it.
This checks exported connectivity and selected parts, not PCB copper, footprint
dimensions, electrical tolerances, or measured hardware behavior.
"""

import argparse
import csv
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


class Audit:
    def __init__(self):
        self.count = 0
        self.failures = []

    def equal(self, description, actual, expected):
        self.count += 1
        if actual != expected:
            self.failures.append(
                f"{description}: expected {expected!r}, got {actual!r}"
            )


def load_design(netlist_path, bom_path):
    root = ET.parse(netlist_path).getroot()
    if root.tag != "export":
        raise ValueError("expected a KiCad XML <export> netlist")
    components = {}
    for component in root.findall("./components/comp"):
        ref = component.attrib["ref"]
        if ref in components:
            raise ValueError(f"duplicate netlist component {ref}")
        components[ref] = component
    if not components:
        raise ValueError("netlist has no components")

    nets, pin_nets = {}, {}
    for net in root.findall("./nets/net"):
        name = net.attrib["name"]
        if name in nets:
            raise ValueError(f"duplicate net name {name}")
        pins = set()
        for node in net.findall("node"):
            ref, number = node.attrib["ref"], node.attrib["pin"]
            pin = f"{ref}.{number}"
            if ref not in components:
                raise ValueError(f"net {name} references missing component {ref}")
            if pin in pin_nets:
                raise ValueError(f"pin {pin} occurs more than once in netlist")
            pins.add(pin)
            pin_nets[pin] = name
        nets[name] = pins
    if not nets:
        raise ValueError("netlist has no nets")

    bom = {}
    with bom_path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {"Reference", "Value", "Manufacturer part", "Footprint"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError(f"BOM must have columns {sorted(required)}")
        for row in reader:
            ref = row["Reference"]
            if not ref or ref in bom:
                raise ValueError(f"empty or duplicate BOM reference {ref!r}")
            bom[ref] = row
    return components, nets, pin_nets, bom


def component_part(component):
    if component is None:
        return None
    mpn = next(
        (p.get("value", "") for p in component.findall("property")
         if p.get("name") == "MPN"),
        component.findtext("./fields/field[@name='MPN']", default=""),
    )
    return component.findtext("value", ""), mpn, component.findtext("footprint", "")


def audit_design(components, nets, pin_nets, bom):
    audit = Audit()
    audit.equal("BOM/netlist reference coverage", set(bom), set(components))
    for ref, component in sorted(components.items()):
        row = bom.get(ref, {})
        actual = (row.get("Value"), row.get("Manufacturer part"), row.get("Footprint"))
        audit.equal(f"{ref} BOM matches netlist", actual, component_part(component))

    # Pin numbering below is specific to these manufacturer parts/packages.
    parts = {
        "J1": ("USB4105-GF-A", "KiCad:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"),
        "J2": ("5301-4P4C", "OpenSimGear:RJ4P4C_Evercom_5301-4P4C_Horizontal"),
        "J3": ("5301-4P4C", "OpenSimGear:RJ4P4C_Evercom_5301-4P4C_Horizontal"),
        "J5": ("PJ-102AH", "OpenSimGear:BarrelJack_SameSky_PJ-102AH_Horizontal"),
        "D1": ("SS14-E3/61T", "KiCad:D_SMA"),
        "D6": ("SS14-E3/61T", "KiCad:D_SMA"),
        "D5": ("SMF15A-E3-08", "KiCad:D_SMF"),
        "D7": ("SMBJ15A-E3/52", "KiCad:D_SMB"),
        "F1": ("1206L020/30YR", "OpenSimGear:Fuse_Littelfuse_1206L020-30YR"),
        "F2": ("1206L035/30WR", "OpenSimGear:Fuse_Littelfuse_1206L035-30WR"),
        "U1": ("STM32G0B1CBT6", "KiCad:LQFP-48_7x7mm_P0.5mm"),
        "U2": ("USBLC6-2SC6", "KiCad:SOT-23-6"),
        "U3": ("TCAN3413D", "KiCad:SOIC-8_3.9x4.9mm_P1.27mm"),
        "U4": ("R-78E5.0-0.5", "KiCad:Converter_DCDC_RECOM_R-78E-0.5_THT"),
        "U5": ("TLV75533PDBVR", "KiCad:SOT-23-5"),
        "U6": ("LM66100DCKR", "OpenSimGear:TI_DCK0006A_SC70-6_LM66100"),
        "U7": ("LM66100DCKR", "OpenSimGear:TI_DCK0006A_SC70-6_LM66100"),
    }
    for ref, expected in parts.items():
        part = component_part(components.get(ref))
        audit.equal(f"{ref} selected MPN/package", part[1:] if part else None, expected)
    values = {
        "F1": "0.20A hold / 30V", "F2": "0.35A hold / 30V",
        "R1": "5.1k 1%", "R2": "5.1k 1%", "R3": "10k 1%",
        "R4": "15k 1%", "R5": "10k", "R6": "10k", "R7": "10k",
        "C11": "100nF 25V X7R",
    }
    for ref, expected in values.items():
        part = component_part(components.get(ref))
        audit.equal(f"{ref} selected value", part[0] if part else None, expected)
    resistor_mpns = {
        "R1": "RC0805FR-075K1L", "R2": "RC0805FR-075K1L",
        "R3": "RC0805FR-0710KL", "R4": "RC0805FR-0715KL",
        "R5": "RC0805FR-0710KL", "R6": "RC0805FR-0710KL",
        "R7": "RC0805FR-0710KL",
    }
    for ref, expected in resistor_mpns.items():
        part = component_part(components.get(ref))
        audit.equal(f"{ref} selected resistor MPN", part[1] if part else None, expected)

    modular = {ref for ref, c in components.items() if component_part(c)[1] == "5301-4P4C"}
    audit.equal("exactly two selected 4P4C jacks", modular, {"J2", "J3"})
    for ref in ("J2", "J3"):
        component = components.get(ref)
        pins = set() if component is None else {
            pin.get("num") for pin in component.findall("./units/unit/pins/pin")
        }
        audit.equal(f"{ref} has exactly four contact pins", pins, {"1", "2", "3", "4"})

    # Exact membership prevents extra shorts as well as missing connections.
    # SS14 and the selected unidirectional TVS parts: pad 1 cathode, pad 2 anode.
    # Both jacks share 1=BUS_12V, 2=CAN_H, 3=CAN_L, 4=GND passively.
    critical_nets = {
        "/DC12_RAW": "J5.1 F2.1",
        "/DC12_FUSED": "F2.2 D6.2",
        "/BUS_12V": "D6.1 D7.1 F1.1 J2.1 J3.1",
        "/BUS_FUSED": "F1.2 D1.2",
        "/BUCK_IN": "D1.1 D5.1 C2.1 U4.1",
        "/BUCK_5V": "U4.3 C3.1 R6.1 U6.1",
        # TI LM66100 Figure 17: U6 ST controls U7 /CE through a buck pull-up.
        # Direct cross-coupled /CE with both ST grounded can shut off both paths.
        "/OR_STATUS": "R6.2 U6.5 U7.3",
        "/SYS_5V": "U6.6 U7.6 C4.1 U5.1 U5.3",
        "/USB_VBUS": "J1.A4 J1.A9 J1.B4 J1.B9 C11.1 R3.1 U2.5 U6.3 U7.1",
        "/VBUS_SENSE": "R3.2 R4.1 U1.19",
        "/CAN_H": "J2.2 J3.2 J6.1 U3.7",
        "/CAN_L": "J2.3 J3.3 J6.2 U3.6",
        "/CAN_RX": "U1.47 U3.4",
        "/CAN_TX": "U1.48 U3.1",
        "/CAN_STB": "R5.2 U1.44 U3.8",
        "/CC1": "J1.A5 R1.1",
        "/CC2": "J1.B5 R2.1",
        "/USB_DM": "J1.A7 J1.B7 U1.33 U2.1 U2.6",
        "/USB_DP": "J1.A6 J1.B6 U1.34 U2.3 U2.4",
        "/NRST": "U1.10 R7.2 C10.1 J4.5 SW2.1",
        "/SWDIO": "J4.3 U1.35",
        "/SWCLK": "J4.4 U1.36",
        "/LED_A": "U1.16 R8.1",
    }
    for name, expected in critical_nets.items():
        audit.equal(f"{name} connectivity", nets.get(name), set(expected.split()))

    # Explicit supply/return pins complete the MCU, transceiver, divider,
    # standby pull-up, USBLC bias capacitor and separate USB-C Rd circuits.
    supply_pins = {
        "+3V3": "U1.4 U1.5 U1.6 U3.3 U3.5 U5.5 R5.1 R7.1 J4.1",
        "GND": "U1.7 U2.2 U3.2 U4.2 U5.2 U6.2 U7.2 J2.4 J3.4 J5.2 "
               "J6.3 J4.2 J1.A1 J1.A12 J1.B1 J1.B12 J1.SH R1.2 R2.2 "
               "R4.2 D5.2 D7.2 C11.2 C10.2 SW2.2 C1.2 C13.2",
    }
    for name, pins in supply_pins.items():
        for pin in pins.split():
            audit.equal(f"{pin} supply/return", pin_nets.get(pin), name)
    for pin in ("C1.1", "C13.1"):
        audit.equal(f"{pin} transceiver bypass supply", pin_nets.get(pin), "+3V3")

    for pin in ("J5.3", "U5.4", "U6.4", "U7.4", "U7.5"):
        name = pin_nets.get(pin, "")
        isolated = name.startswith("unconnected-") and nets.get(name) == {pin}
        audit.equal(f"{pin} intentionally unconnected", isolated, True)
    return audit


def main():
    directory = Path(__file__).resolve().parents[1] / "electronics/boards/gateway/0.1.0"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--netlist", type=Path, default=directory / "review/netlist.xml")
    parser.add_argument("--bom", type=Path, default=directory / "bom.csv")
    args = parser.parse_args()
    try:
        audit = audit_design(*load_design(args.netlist, args.bom))
    except (OSError, ValueError, KeyError, ET.ParseError, csv.Error) as error:
        print(f"FAIL: cannot read design: {error}", file=sys.stderr)
        return 2
    if audit.failures:
        for failure in audit.failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"FAILED: {len(audit.failures)} of {audit.count} checks", file=sys.stderr)
        return 1
    print(f"PASS: {audit.count} netlist/BOM electrical intent checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
