# PCB versioning

PCB designs use **MAJOR.MINOR.PATCH** semantic versions. PATCH is the assembly/BOM **subrevision**.

| Part | Increment after launch when |
| --- | --- |
| **MAJOR** | Form factor, board size, mechanical mounting, connector positions or I/O voltages change, requiring housing or mechanical updates. |
| **MINOR** | Schematic or layout edits, trace cuts, bodge wires or stackup changes require ordering a new physical PCB. |
| **PATCH / SUBREVISION** | Only the BOM, component values or assembly variants change, without altering the base copper layout. |

Apply the highest affected category; a major increment resets minor and patch to zero, and a minor increment resets patch to zero. If a component substitution changes the footprint or copper, it is not a patch.

**Pre-live policy:** the current design is **0.1.0**, work in progress. Do not increment any part while we are not live, even for changes that would warrant an increment after launch. Update the existing design and its matching firmware, BOM, review exports and validation record in place. Do not create parallel revision folders, release tags or releases. Version increments start only once the project is declared live.

Keep one current design per board under `boards/<board>/0.1.0/`. Earlier circuits are available in Git history, not maintained alongside the current design. Gateway and device boards have independent versions; matching numbers do not imply identical hardware. Firmware targets identify their matching PCB version, and pin-map compatibility must be verified separately from firmware or protocol versions.

The device's four-layer production layout and two-layer home-milling layout are fabrication variants of the same current circuit. Their different sizes, stackups, footprints and layer-join construction are explicitly documented; do not mix their manufacturing artifacts or assume mechanical interchangeability. A BOM-only assembly subrevision must identify its base copper and fabrication variant. Once live, changes to either physical variant follow the same rules above.
