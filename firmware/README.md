# Firmware

This folder is initialized for device-specific C firmware. It contains source, include, board-target, capability-profile, dependency-configuration, and test locations; no flashable firmware or build target is implemented yet.

Reuse firmware-common and the separately versioned protocol definitions. Module prototype baseline is STM32G0B1 with TinyUSB (HID plus CDC) and libcanard (Cyphal/CAN FD); exact tested dependencies and reference-board details will be added during implementation. Gateway hardware remains undecided.
