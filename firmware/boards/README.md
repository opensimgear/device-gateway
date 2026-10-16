# Board targets

## Development status and disclaimer

This project is heavily under development and is not ready for use. Use it entirely at your own risk. To the fullest extent permitted by applicable law, the authors and contributors disclaim all liability for any damage, loss, or other issues arising from its use.

`gateway_g0b1cb_v0_1_0` implements the STM32G0B1CBT6 gateway with USB-C,
two parallel 4P4C chain connectors and a 12 V center-positive barrel input.
There is one physical CAN FD bus, one transceiver and one node-allocation domain.

See [v0.1.0 pin assignment and bring-up](gateway_g0b1cb_v0_1_0/README.md).
The linker reserves the final two 2 KiB flash pages for the registry journal.
A different MCU density or connector revision requires an explicit new board
port and matching memory map; the build rejects other board names.
