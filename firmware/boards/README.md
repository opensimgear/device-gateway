# Board targets

`gateway_g0b1cb_rev_a` implements the STM32G0B1CBT6 gateway with USB-C,
two parallel 4P4C chain connectors and a 12 V center-positive barrel input.
There is one physical CAN FD bus, one transceiver and one node-allocation domain.

See [Rev A pin assignment and bring-up](gateway_g0b1cb_rev_a/README.md).
The linker reserves the final two 2 KiB flash pages for the registry journal.
A different MCU density or connector revision requires an explicit new board
port and matching memory map; the build rejects other board names.
