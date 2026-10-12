# Gateway electronics

[Gateway Rev A](boards/gateway/rev-a/README.md) is the editable STM32G0B1 USB/CAN gateway prototype with USB-C, two parallel **4P4C** modular jacks and a center-positive 12 V barrel socket. It carries one CAN trunk; the ports are not independent CAN channels.

The board includes protected 12 V trunk injection and separate MCU branch protection, USB/bus supply selection with reverse blocking, SWD and reset. CAD, BOM, static checks and fabrication exports live together in the revision directory. Physical qualification and fabrication approval are separate gates.
