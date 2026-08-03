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

**Three steps, not two.** The initialisation below has to happen first, and
omitting it is the difference between a wheel that ends up turning freely and
one that ends up blocked.

### 1. Initialisation, on the interrupt OUT pipe

Five packets, sent while the wheel is still at `B65D`, on endpoint `0x01`:

```
42 01 00 00 00 00 00 00 00
0a 04 90 03 00 00 00 00
0a 04 00 0c 00 00 00 00
0a 04 12 10 00 00 00 00
0a 04 00 06 00 00 00 00
```

Source: `drivers/hid/hid-thrustmaster.c`, `setup_0` to `setup_4`, sent by
`thrustmaster_interrupts()` which `thrustmaster_probe()` calls **before** the
model query. Its comment explains them as a crash fix for the T300RS and
notes they harm no other wheel. Akellacom's macOS T300RS driver ships the
same five and states they "MUST be sent before the mode switch".

They are sent on `cur_altsetting->endpoint[1]`, which on the measured T150 is
the interrupt OUT endpoint `0x01`.

### 2 and 3. The control transfers

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

| Direction | Address | Max packet |
| --- | --- | --- |
| Interrupt IN | `0x82` | 16 |
| Interrupt OUT | `0x01` | 32 |

**Measured**, by `probe_intr` enumerating the pipes on interface 0. The wheel
has exactly these two.

This settles the disagreement recorded here before. `t150_driver` discovers
the OUT endpoint at runtime, its own `traffic/old_caps/t150_test.py` writes
to `0x01`, and macoswheels recorded `0x02`: it is **`0x01`**. The IN address
was written down here as `0x81` on nothing better than assumption, and is
**`0x82`**.

The 32-byte maximum matters for the force feedback packets: `ff_commit` is
15 bytes, so everything in this document fits in one transfer.

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
re-derived. Every field below is transcribed from `t150_driver`'s
`hid-t150/forcefeedback.h` structures and the functions in
`hid-t150/forcefeedback.c` that fill them. All structures are `__packed` and
every multi-byte field is little-endian.

Each effect uploads as three packets, sent on the interrupt OUT endpoint in
this order. `slot` below is the driver's `effect->id`.

**1. `ff_first`, 11 bytes.** Envelope and the first slot key.

```
f0  pk_id0  f1  attack_length:u16  attack_level  fade_length:u16  fade_level  f2  f3
```

`f0` is the effect class, not a fixed opcode: `0x02` for constant and for
periodic, `0x05` for spring and damper. `f1` is 0, `f2` is `0x46` and `f3` is
`0x54`, all three unexplained by the driver. `pk_id0 = slot * 0x1C + 0x1C`.
Lengths are milliseconds. The driver's own comment marks the two level fields
as wrong, and it fills `fade_length` from the attack length, which is a bug in
that driver and must not be copied.

**2. `ff_update`, 4, 8 or 11 bytes.** The effect-class parameters.

```
class  pk_id1  f1  <class-specific payload>
```

`f1` is 0 and `pk_id1 = slot * 0x1C + 0x0E`. The class byte here uses a
different set from `ff_first`:

| class | payload | bytes |
| --- | --- | --- |
| `0x03` constant | `level:i8` | 4 |
| `0x04` periodic | `magnitude:i8 offset:i8 phase:u8 period:u16` | 8 |
| `0x05` condition | `right_coeff:i8 left_coeff:i8 center:i16 deadband:i16 right_sat:u8 left_sat:u8` | 11 |

**3. `ff_commit`, 15 bytes.** Correlates the other two through both slot keys
and declares the effect type, duration and start delay.

```
f0  id  effect_type:u16  length:u16  f1:u16  f2  pk_id1  f3  pk_id0  f4  delay  f5
```

`f0` is `0x01`, `id` is the slot, and `f1` through `f5` are 0. `length` is the
duration in milliseconds, with `0xFFFF` meaning endless. `delay` is a single
byte holding the *high* byte of the start delay in milliseconds.

| `effect_type` | Effect |
| --- | --- |
| `0x4000` | constant |
| `0x4022` | sine |
| `0x4023` | sawtooth up |
| `0x4024` | sawtooth down |
| `0x4040` | spring |
| `0x4041` | damper |

The codes are contiguous around the periodics, so `0x4020`, `0x4021` and
`0x4025` may well be the waveforms the Linux driver never implemented. That is
a guess, and `probe_setreport -x` can settle it in a minute.

**Effect control**, 4 bytes, starts or stops an uploaded effect:

```
[0x41, slot, mode, times]
// mode: 0x41 play, 0x00 stop
// times: repeat count when playing, 0x01 when stopping
```

There is no erase packet. The driver's `t150_ff_erase()` sends nothing and
just frees the slot, having observed that the Windows driver does the same.

### Converting DirectInput units to the wire

The driver converts from Linux `ff_effect` units. Ours arrive in DirectInput
units instead, so both are recorded here; the divisors are the wheel's, the
input ranges are not.

| Field | From Linux `ff_effect` | Wire range |
| --- | --- | --- |
| periodic magnitude, offset | `value >> 8` | `int8` |
| periodic phase | `phase / ((360 * 100) / 0xFF)` | 0..255 = 0..360 degrees |
| periodic period | milliseconds, unscaled | `uint16` |
| constant level | direction-projected, then `/ 0x01FF` | `int8` |
| condition coefficients | `/ 0x147` | -100..+100 |
| condition centre | `/ (0x7FFF / 0x01F4)` | -500..+500 |
| condition deadband | `/ (0xFFFF / 0x03E8)` | 0..1000 |
| spring saturation | `/ 0x030C` | 0..0x54 |
| damper saturation | `/ 0x028F` | 0..0x64 |
| autocenter force | `round(force * 100 / 0xFFFF)` | 0..100 |
| gain | low byte only | 0..255 |

Three of those are worth flagging rather than trusting.

The **constant level stops at 64** while a periodic magnitude reaches 127.
The condition divisors all land exactly on maxima the driver's own struct
comments document, which is good evidence the derivation is sound, but no
comment documents a constant's ceiling. It may just be conservative.
`probe_setreport` can compare `0x40` against `0x7f` directly.

The **envelope levels are a guess.** The driver divides by `0x1fff`, which
would give a range of 0 to 4, and its own comment says the field is wrong.
The encoder maps to the full byte instead, which is the least surprising
reading of a one-byte field, and nothing confirms it.

The **start delay carries only the high byte** of a millisecond value, so its
unit is 256 ms. That is what the driver sends. It is coarse enough to be
suspicious, and harmless in practice because almost every effect starts at
once.

One more asymmetry, in the encoder rather than the protocol: the direction is
projected onto the X axis for a constant force and ignored for everything
else, because that is what the driver does. A periodic arguably deserves the
same treatment. Left alone until hardware can say.

Note that `ff_commit` is 15 bytes while the declared output report is 14. If
the HID framing turns out to be the one the firmware honours, the report id
byte accounts for the difference and the payload is 14; if the raw framing
wins, the packet is 15 bytes on the wire. Another thing Phase 0 settles.

## Effect coverage

| DirectInput effect | T150 hardware | Plan |
| --- | --- | --- |
| Constant | yes | pass through |
| Sine, sawtooth up, sawtooth down | yes | pass through |
| Spring | yes | pass through |
| Damper | yes | pass through |
| Square | no | downgrade to sine |
| Triangle | no | downgrade to sine |
| Friction | no | downgrade to damper |
| Inertia | no | downgrade to damper |
| Ramp | not in the protocol | synthesize as a time-sliced constant |

Square and triangle were previously recorded here as native. They are not.
`t150_driver`'s supported effect list is `FF_GAIN`, `FF_PERIODIC`, `FF_SINE`,
`FF_SAW_UP`, `FF_SAW_DOWN`, `FF_CONSTANT`, `FF_SPRING` and `FF_DAMPER`, and
`ff_commit` has no type code for either waveform. Whether the firmware knows
them anyway is the `0x4020`/`0x4021` question above.

Downgrading rather than refusing is deliberate: a game that gets
`DIERR_UNSUPPORTED` from `CreateEffect` may disable force feedback outright,
which is a worse outcome than an approximated waveform.
