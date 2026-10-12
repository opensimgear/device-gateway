# Gateway STM32G0B1CB Rev A

The port targets STM32G0B1CBT6 in LQFP48, with 128 KiB flash and the guaranteed
128 KiB SRAM region. System and CAN clocks are 64 MHz from HSI16 plus PLL.
USB runs from HSI48 with USB SOF clock recovery. No external crystal is required.

| Function | MCU pin | Configuration |
| --- | --- | --- |
| USB D− / D+ | PA11 / PA12 | Full-speed USB device; package remap disabled |
| USB VBUS presence | PB0 | Digital input from the 10 kΩ / 15 kΩ divider |
| CAN RX / TX | PB8 / PB9 | Alternate function 3, FDCAN1 |
| CAN transceiver standby | PB5 | High at reset/fault/recovery, low in normal operation |
| Status LED | PA5 | Active high |
| SWDIO / SWCLK | PA13 / PA14 | SWD retained |

Both **4P4C** sockets use the same physical net assignment: pin 1 BUS12V,
pin 2 CANH, pin 3 CANL, pin 4 GND. Verify the numbered mating contacts against
the connector drawing and cable continuity, as handset cables can reverse order.
The 12 V input is center positive. USB power supports the gateway logic; it does
not supply 12 V to the chain. Supply, protection, termination and current limits
are defined by the electronics revision.

CAN FD arbitration timing at 64 MHz is prescaler 8, segment 1 = 13, segment 2 = 2,
SJW = 2: 500 kbit/s with 87.5% sampling. Data timing uses prescaler 2 with the
same segments: 2 Mbit/s. Transmitter delay compensation is enabled with an
offset of 28 CAN clock periods (the configured data sample point) and filter 0.
These settings still need oscilloscope and real-bus validation with the chosen
transceiver, termination, connectors and cables.

The LED blinks at one-second intervals without an active USB host and half-second
intervals while mounted; bus-off uses 100 ms intervals. A steady LED indicates a
fatal startup/storage/configuration failure and places the transceiver in standby.

Before powering a prototype, check the board's assembled rails and absence of
VBUS/12 V backfeed. Use an authorized VID/PID build and SWD to flash only the
application region. Start with the chain powered from a current-limited 12 V
source and the appropriate two physical end terminators. Use a CAN FD adapter
and the [reference peer](../../../tools/README.md) for discovery and input.

Required bench observations are USB HID/CDC enumeration; VBUS attach/detach
while 12 V remains; both port placements; CAN ACK/error recovery with the peer
unplugged; configured neutral reports after loss; descriptor stability during
module absence; save/apply persistence; and controlled power interruption during
inactive-page erase, programming and final commit. Confirm the first flash page
and application checksum stay intact during storage writes in the actual bank
mode. Native fault-injection tests cover journal sequencing, not MCU electrical
behavior.

Mass erase or clearing both journal pages resets the lease base. Power-cycle all
modules before reconnecting a factory-erased gateway. Do not alter option bytes
or expand the flash map to the larger-density family members. The memory layout
and HAL adapter reject flash sizes other than 128 KiB.
