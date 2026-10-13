#!/usr/bin/env python3
"""Record native CAD and matching manufacturing/review export hashes."""
import argparse
import hashlib
import os
from jlcpcb import select_version
from pathlib import Path
repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--version", help="Board version; defaults to highest numeric version")
args = parser.parse_args()
root = select_version(repo, "gateway", args.version)
files = [root / p for p in ('gateway.kicad_pro', 'gateway.kicad_sch', 'gateway.kicad_pcb',
                           'gateway.kicad_dru', 'bom.csv', 'fp-lib-table', 'sym-lib-table',
                           'KiCad.kicad_sym', 'OpenSimGear.kicad_sym')]
files += list(root.glob('*.md'))
files += [repo / 'tools' / name for name in (
    'bundle-jlcpcb.sh', 'jlcpcb.py', 'test_jlcpcb.py', 'README.md',
    'audit-netlist.py', 'check-and-export.sh', 'install-4p4c-footprint.py', 'write-manifest.py')]
for directory in ('KiCad.pretty', 'OpenSimGear.pretty', 'review', 'fabrication'):
    files += [p for p in (root / directory).rglob('*') if p.is_file() and p.name != 'SHA256SUMS.txt']
text = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {Path(os.path.relpath(p, root)).as_posix()}\n'
               for p in sorted(set(files)))
(root / 'fabrication/SHA256SUMS.txt').write_text(text)
print(f'Recorded {len(set(files))} source/export hashes in fabrication/SHA256SUMS.txt.')
