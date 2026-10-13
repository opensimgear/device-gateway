#!/bin/sh
# Run from any directory with KiCad 10 on PATH. Static checks gate refreshed exports.
set -eu
cd "$(dirname "$0")/.."
mkdir -p review fabrication/gerbers fabrication/drill
kicad-cli sch erc gateway.kicad_sch --exit-code-violations -o review/erc.rpt
kicad-cli pcb drc gateway.kicad_pcb --refill-zones --save-board --schematic-parity --all-track-errors --exit-code-violations -o review/drc.rpt
kicad-cli sch export netlist gateway.kicad_sch --format kicadxml -o review/netlist.xml
kicad-cli sch export bom gateway.kicad_sch -o bom.csv --fields Reference,Value,MPN,Footprint,Datasheet --labels 'Reference,Value,Manufacturer part,Footprint,Datasheet'
python3 tools/audit-netlist.py
kicad-cli sch export pdf gateway.kicad_sch -o review/schematic.pdf
kicad-cli pcb export svg gateway.kicad_pcb --mode-single --layers F.Cu,F.Silkscreen,F.Fab,Edge.Cuts --page-size-mode 2 --exclude-drawing-sheet -o review/pcb.svg
kicad-cli pcb export svg gateway.kicad_pcb --mode-single --layers B.Cu,B.Silkscreen,B.Fab,Edge.Cuts --page-size-mode 2 --exclude-drawing-sheet -o review/pcb-back.svg
kicad-cli pcb export gerbers gateway.kicad_pcb -o fabrication/gerbers/ --layers F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,Edge.Cuts --subtract-soldermask
kicad-cli pcb export drill gateway.kicad_pcb -o fabrication/drill/ --format excellon --excellon-units mm --excellon-separate-th --generate-report --report-path fabrication/drill/drill-report.txt --generate-map --map-format pdf
kicad-cli pcb export pdf gateway.kicad_pcb -o review/assembly.pdf --mode-multipage --layers F.Fab,B.Fab --common-layers Edge.Cuts --sketch-pads-on-fab-layers --black-and-white --no-property-popups
kicad-cli pcb export pos gateway.kicad_pcb -o fabrication/positions.csv --format csv --units mm --side both
python3 tools/write-manifest.py
