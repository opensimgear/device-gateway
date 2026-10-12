# Gateway module profiles

The gateway learns each module's control counts, axis usages, neutral values and
profile revision from its CAN announcement. Registering a module stores that
profile against its permanent identity and HID slot. Applying a changed registry
rebuilds the USB descriptors and reconnects USB; an unexpected profile change
keeps the existing slot neutral until explicitly applied.

There is no compiled gateway device profile. Lite and Pro share the firmware and
PCB. Unit identities belong to physical modules, independently of enclosure
edition. USB VID/PID allocation remains pending; board builds require identifiers
the builder is authorized to use.
