# Protocol Notes

These notes record behavior verified against the current Logitech G Pro Wireless receiver workflow. They are implementation evidence, not a promise that every Logitech device behaves the same way.

## HID++ transport

The receiver is exposed through the Windows HID++ interface and answers HID++ 4.2 requests. The implementation uses the long HID++ report format and enumerates the Feature Set before addressing the paired device.

## Features observed

- `0x8070` Color LED Effects
- `0x8100` Onboard Profiles
- `0x8110` Mouse Button Filter
- `0x8060` Report Rate
- `0x2201` Adjustable DPI

The receiver also exposes device-specific feature IDs. Feature presence, index, and version are read at runtime; the implementation does not assume that an index is universal.

## Onboard profile format

The active sector is read through `0x8100`, validated with CRC-16/CCITT-FALSE, and written back only after a same-size backup has been created. For the observed format-3 profile, button records begin at offset 32 and the active lighting records occupy the profile lighting area beginning at offset 208.

The current device uses profile lighting record ID `0x00` for Disabled. In this format, an intensity byte of zero is not equivalent to disabled; the record ID and profile format must be interpreted together.

## Lighting ownership

`0x8070` live zone-effect reads are capability-dependent. When the device does not expose the required live effect capability, profile operations preserve the current raw onboard lighting records instead of fabricating live state. This is why restore merges the current lighting area into the saved profile backup.

## References

- [libratbag HID++ 2.0 definitions](https://github.com/libratbag/libratbag/blob/master/src/hidpp20.h)
- [libratbag HID++ 2.0 driver](https://github.com/libratbag/libratbag/blob/master/src/driver-hidpp20.c)
- [OpenLogi Color LED Effects](https://github.com/AprilNEA/OpenLogi/blob/main/crates/openlogi-hidpp/src/feature/color_led_effects.rs)
- [Solaar HID++ profile implementation](https://github.com/pwr-Solaar/Solaar/blob/master/lib/logitech_receiver/hidpp20.py)
