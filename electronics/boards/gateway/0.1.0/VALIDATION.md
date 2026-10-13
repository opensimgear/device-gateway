# Gateway v0.1.0 validation

This is an unassembled prototype design. Static CAD checks pass; no hardware measurement, cable qualification or manufacturing approval has been performed.

| Check | Actual result |
| --- | --- |
| KiCad 10.0.6 schematic ERC | 0 violations; [report](review/erc.rpt). |
| Native PCB DRC, filled zones, all track errors | 0 violations and 0 unconnected items; [report](review/drc.rpt). |
| Schematic/PCB parity | 0 issues in native DRC. |
| Independent circuit review | Exact MCU/USB/CAN pins, 4P4C numbered drawing, supply diode/TVS direction and source-OR checked. LM66100 equal-input shutdown risk corrected using TI Figure 17 status/pull-up priority topology; F1 changed to 30 V rated 1206L020/30YR. |
| Netlist/BOM intent audit | 149 checks pass with `python3 tools/audit-netlist.py` from the repository root, including connector mapping, protection, supply OR, MCU and reset defaults. |
| Exports | Matching schematic PDF, front/back SVG, 1:1 assembly PDF, Gerber X2 copper/mask/paste/silk/outline, separate PTH/NPTH Excellon and PDF drill maps, BOM and positions CSV. |

Run `sh tools/check-and-export.sh 0.1.0` from the repository root with KiCad 10 available to repeat checks and refresh exports. Zone filling and schematic parity are required after every routing change. On this macOS host KiCad's zone-update CLI calls require screen access: the sandboxed wxWidgets call otherwise crashes at `wxGetMousePosition`. Native DRC was run with local display access, while ordinary file edits/export operations stayed in the workspace. The complete pre-routing revision was backed up and the final source is the editable native CAD, not a rendered image.

Inherited ignored checks cover missing courtyards (mechanical holes), endpoint centering on vias, tuning-profile geometries, footprint filter matches and footprint/pad technology classification. They are listed in the report. Electrical shorts, spacing, holes, unrouted items, dangling copper, silkscreen and schematic parity are checked; no DRC issue exclusions were added to hide routing faults.

USB main-pair width 0.2474 mm and gap 0.1501 mm are inherited calculated geometry for the documented 90 Ω stackup, not a measured impedance. CAN H/L form one passive net pair through both jacks with a local transceiver/test tap; this prototype has not established controlled CAN differential impedance, maximum bus/cable length or an error-rate limit. Copper and connector continuity do not prove CAN FD signal integrity. Keep bench cables short and verify two endpoint resistors and actual CAN FD error rates before choosing cable length or data-rate limits.

Required physical evidence before release: all supply combinations and transitions; USB backfeed/leakage/suspend draw; reverse-polarity and current-limited short tests; fuse/diode/converter/connector temperatures; purchased-connector and plated-slot fit; straight-through cable mapping and voltage drop; endpoint-plug resistance; USB orientation/enumeration; CAN FD errors/recovery; and the intended ESD/transient/hot-plug environment. Use a regulated 12 V supply, ≤0.5 A current limit and provisional ≤250 mA total chain budget at 20–40°C. F1/F2 hold and trip ratings are temperature-dependent fault parameters, not active current limits or a validated system rating.

## Version consolidation checks — 2026-10-13

The single current design is PCB version **0.1.0**, frozen until launch under [PCB versioning](../../../VERSIONING.md). CAD title blocks, silkscreen, firmware target names, documentation and review/manufacturing exports now identify this version. Source comparison against the preceding design confirms copper and circuit connectivity are unchanged. Native KiCad 10.0.6 ERC and all-track DRC with schematic parity pass with zero violations and unconnected items. The matching gateway Arm firmware builds, and all 3 native tests pass. Cross-builds use compile-only USB ID placeholders; physical qualification remains pending.
