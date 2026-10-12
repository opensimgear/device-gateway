#!/usr/bin/env python3
"""Copy the verified gateway 4P4C land pattern into another local KiCad library.

This does not change a schematic, net assignments, placement or routing. See
FOOTPRINTS.md for the required electrical and mechanical migration steps.
"""
import argparse
import hashlib
from pathlib import Path
import shutil

NAME = 'RJ4P4C_Evercom_5301-4P4C_Horizontal.kicad_mod'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('project', type=Path, help='Destination KiCad project directory')
parser.add_argument('--replace', action='store_true', help='Replace an existing, different footprint')
args = parser.parse_args()
source = Path(__file__).resolve().parents[1] / 'OpenSimGear.pretty' / NAME
destination = args.project.resolve() / 'OpenSimGear.pretty' / NAME
if destination.exists() and destination.read_bytes() != source.read_bytes() and not args.replace:
    parser.error('Destination differs. Review it before using --replace.')
destination.parent.mkdir(parents=True, exist_ok=True)
if source != destination:
    shutil.copy2(source, destination)
print(destination)
print('SHA256:', hashlib.sha256(destination.read_bytes()).hexdigest())
print('Use OpenSimGear:RJ4P4C_Evercom_5301-4P4C_Horizontal in a registered project-local library.')
print('Reconnect 1 BUS_12V, 2 CAN_H, 3 CAN_L, 4 GND; update placement/routing and run ERC/parity/DRC.')
