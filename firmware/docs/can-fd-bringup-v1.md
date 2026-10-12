# Archived CAN FD bring-up v1

Version 1 was the initial gateway-only prototype. Current firmware rejects its
wire frames and uses [Cyphal/CAN](cyphal.md), including
128 buttons, four hats, eight axes and actual USB-delivery acknowledgments.
The historical `tests/can-fd-v1-vectors.json` remains for reference. Persisted
version 1 registries remain compatible and load with zero hats.
