# Thrustmaster T150 protocol

Every constant in `include/t150/t150.h` comes from one of these sources.
Nothing here is inferred, and the points that remain unconfirmed against
real hardware are called out as such.

Sources:

- [scarburato/t150_driver](https://github.com/scarburato/t150_driver), the
  Linux T150 driver, for the settings and force feedback opcodes.
- [scarburato/hid-tminit](https://github.com/scarburato/hid-tminit) and the
  in-tree `drivers/hid/hid-thrustmaster.c`, for the boot to firmware switch.
- [Kimplul/hid-tmff2](https://github.com/Kimplul/hid-tmff2), for the
  contrast with the newer T300 family.

## USB identity

| State | VID | PID |
| --- | --- | --- |
| Boot, shared by every T-series wheel | `044F` | `B65D` |
| Firmware, T150 | `044F` | `B677` |

## Boot to firmware switch

Two vendor control transfers on endpoint 0.

1. Model query: `bmRequestType 0xC1`, `bRequest 73`, `wValue 0`, `wIndex 0`,
   `wLength 16`. Response byte 6 is the attachment, byte 7 is the model.
2. Mode switch: `bmRequestType 0x41`, `bRequest 83`, `wValue` = the
   per-model switch value, no data stage.

For the T150: model `0x03`, attachment `0x06`, switch value `0x0006`.

The wheel then detaches and re-enumerates at `B677`.

Both requests carry recipient = interface with `wIndex` 0, which is the
interface macOS's HID driver owns. Whether they can be issued at all from
macOS userspace, and with what privilege, is what `probe_ep0` measures.

## USB endpoints in firmware mode

| Direction | Address |
| --- | --- |
| Interrupt IN | `0x81` |
| Interrupt OUT | `0x02` or `0x01`, see below |

**Unresolved.** `t150_driver`'s `hid-t150.c` discovers the OUT endpoint at
runtime rather than hardcoding it, while the same repository's
`traffic/old_caps/t150_test.py` writes to `0x01`. macoswheels recorded
`0x02`. This only matters if the HID `SetReport` path turns out to be closed
and raw pipe access is the fallback, so it is left open until `probe_ep0`
says whether raw access is reachable at all.

## HID report descriptor

Decoded from `t150_driver/traffic/old_caps/hid_report_fw35`, a capture of
firmware 3.5. **Not** captured from current hardware, so `probe_hid` should
confirm it before anything relies on it.

The relevant part is an output report:

```
85 0A           Report ID (0x0A)
06 00 FF        Usage Page (vendor 0xFF00)
09 0A           Usage (0x0A)
75 08 95 0E     Report Size 8, Report Count 14
26 FF 00        Logical Maximum 255
46 FF 00        Physical Maximum 255
91 02           Output (Data, Var, Abs)
```

The descriptor also declares a Generic Desktop Joystick application
collection with a 16-bit X axis, an 8-bit Y, Rz and slider, thirteen buttons
and a hat, which is why macOS already exposes the wheel as a working
joystick with nothing installed. There is no Physical Interface Device
collection anywhere in it, which is why games see no force feedback.

**The contradiction that Phase 0 has to resolve.** The descriptor declares a
14-byte output report, so `IOHIDDeviceSetReport(kIOHIDReportTypeOutput,
0x0A, ...)` has something valid to address. But the Linux driver never uses
the HID layer: it writes 2 to 4 raw bytes straight to the interrupt OUT pipe
with no report id prefix, and the Python capture tool does the same. Whether
the firmware honours the HID-framed form is unknown and is exactly what
`probe_setreport` exists to find out.

For comparison, the newer T300 family does go through the HID layer:
`hid-tmff2`'s `t300rs_send_buf()` ends in `hid_hw_request(hdev, report,
HID_REQ_SET_REPORT)`. So Thrustmaster firmware is capable of accepting HID
output reports; it is the older T150 that is in doubt.

## Settings packets

All except gain share one form, sent on the interrupt OUT endpoint:

```
[0x40, op, arg_lo, arg_hi]      little-endian uint16 argument
```

| op | Meaning | Argument |
| --- | --- | --- |
| `0x03` | Autocenter spring force | 0..100, a hardware percent |
| `0x04` | Autocenter enable | 0 off, 1 on |
| `0x11` | Rotation range | `degrees * 0xFFFF / 1080` |

Gain is two bytes with a different opcode:

```
[0x43, gain]
```

The original `t150_set_gain` assigns a `uint16` into a `uint8` slot, so the
high byte is dropped in C. That narrowing is behaviour, not a bug to fix: it
is what the wheel is known to accept.

Rotation range is clamped by the firmware below 270 degrees. The scaling in
`t150_range_arg()` is checked against recorded values in
`tests/header_check.c`.

## Force feedback upload

Not yet implemented here. Recorded now so the work does not have to be
re-derived.

Each effect uploads as three interrupt OUT packets:

1. `ff_first`, 11 bytes, opcode `0xF0`: envelope and slot keys. Carries
   attack length and level, fade length and level, and two slot-derived
   markers, where `pk_id0 = slot * 0x1C + 0x1C`.
2. `ff_update`, 4 to 8 bytes, the effect-class parameters.
   - `0x03` constant: one `int8` level.
   - `0x04` periodic: magnitude, offset, phase, period
     (`int8`, `int8`, `uint8`, `uint16` LE).
   - `0x05` condition: right and left coefficient, centre, deadband, right
     and left saturation.
3. `ff_commit`, 15 bytes: correlates the first two through `pk_id0`, and
   declares the effect type code, duration and start delay. Type codes:
   `0x4000` constant, `0x4022` sine, `0x4023` sawtooth up, `0x4024` sawtooth
   down, `0x4040` spring, `0x4041` damper.

An effect control packet then starts or stops it:

```
[0x60, slot, control, repeats]
// control: 0x00 stop, 0x01 play, 0x41 play and loop
```

Note that `ff_commit` is 15 bytes while the declared output report is 14. If
the HID framing turns out to be the one the firmware honours, the report id
byte accounts for the difference and the payload is 14; if the raw framing
wins, the packet is 15 bytes on the wire. Another thing Phase 0 settles.

## Effect coverage

| DirectInput effect | T150 hardware | Plan |
| --- | --- | --- |
| Constant | yes | pass through |
| Square, sine, triangle, sawtooth up and down | yes | pass through |
| Spring | yes | pass through |
| Damper | yes | pass through |
| Friction | no | refusing risks a game disabling force feedback entirely, so downgrade |
| Inertia | no | downgrade to damper |
| Ramp | not in the protocol | synthesize as a time-sliced constant |
