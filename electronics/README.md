# Gateway electronics

[Gateway v0.1.0](boards/gateway/0.1.0/README.md) is the editable STM32G0B1 USB/CAN gateway prototype with USB-C, two parallel **4P4C** modular jacks and a center-positive 12 V barrel socket. It carries one CAN trunk; the ports are not independent CAN channels.

The board includes protected 12 V trunk injection and separate MCU branch protection, USB/bus supply selection with reverse blocking, SWD and reset. CAD, BOM, static checks and fabrication exports live together in the revision directory. Physical qualification and fabrication approval are separate gates.

PCB version **0.1.0** stays fixed while pre-live; see [PCB versioning](VERSIONING.md).
