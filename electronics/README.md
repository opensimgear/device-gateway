# Electronics

Add schematics, PCB source projects, electrical BOMs, pinouts, and wiring guides under boards/<board-name>/<revision> when implemented. Keep a shared PCB once when Lite and Pro use the same circuitry. An edition-specific harness can have its own wiring guide without copying the board project.

Modules require USB-C plus two parallel powered-CAN connectors for linear daisy chaining. CAN carries data; a separate assigned supply pair carries the external 12 V feed. Local regulation, protected power feeds, reverse-current protection, and USB power-budget handling require electrical validation. Exact connector/cable/pinout selection remains pending qualification.

Gateway electronics require five independent CAN controller/transceiver paths and separately protected 12 V port feeds. Gateway hardware selection is pending.

This scaffold contains no selected PCB, circuitry, approved power limits, or manufacturing-ready files.
