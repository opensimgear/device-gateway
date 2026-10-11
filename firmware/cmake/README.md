# Build and dependency configuration

The planned build uses CMake, Ninja, and Arm GNU. Add dependencies.cmake when integrating working firmware: pin firmware-common to a full commit and support an explicit local-checkout override. Common firmware owns the protocol, TinyUSB, and libcanard pins.

No dependency pins or CMake board builds are active yet. A gateway build must fail clearly until a real five-port hardware target exists. Do not copy common source into this repository.
