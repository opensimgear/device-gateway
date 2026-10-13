#!/usr/bin/env python3
"""Record native CAD and matching manufacturing/review export hashes."""
import hashlib
from pathlib import Path
root = Path(__file__).resolve().parents[1]
files = [root / p for p in ('gateway.kicad_pro', 'gateway.kicad_sch', 'gateway.kicad_pcb',
                           'gateway.kicad_dru', 'bom.csv', 'fp-lib-table', 'sym-lib-table',
                           'KiCad.kicad_sym', 'OpenSimGear.kicad_sym')]
files += list(root.glob('*.md'))
files += [p for p in (root/'tools').glob('*') if p.is_file()]
for directory in ('KiCad.pretty', 'OpenSimGear.pretty', 'review', 'fabrication'):
    files += [p for p in (root / directory).rglob('*') if p.is_file() and p.name != 'SHA256SUMS.txt']
text = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(root).as_posix()}\n'
               for p in sorted(set(files)))
(root / 'fabrication/SHA256SUMS.txt').write_text(text)
print(f'Recorded {len(set(files))} source/export hashes in fabrication/SHA256SUMS.txt.')
