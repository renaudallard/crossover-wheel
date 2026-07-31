# t150ffb

Force feedback for the **Thrustmaster T150** in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval.

> **Status: pre-implementation.** Only the Phase 0 measurement tools, the
> shared headers and the build exist. The project is deliberately gated on
> one experiment, described in [`docs/PROBES.md`](docs/PROBES.md), because
> if it fails nothing else here is worth writing.

## Why this exists

A T150 on a Mac is already half working, and it is worth being precise about
which half.

**Already works, with nothing installed.** macOS enumerates the wheel as an
ordinary joystick once it is in firmware mode, and CrossOver's
`dlls/winebus.sys/bus_iohid.c` copies the wheel's real HID report descriptor
into the bottle unchanged. Axes, pedals, buttons and the hat reach games
today.

**Does not work.** Force feedback. The wheel's descriptor contains no
Physical Interface Device collection, and Wine's DirectInput sets
`DIDC_FORCEFEEDBACK` only from PID collections it finds in a descriptor. Wine
adds nothing of its own on macOS either: `bus_iohid.c` implements
`raw_device_vtbl`, not `hid_device_vtbl`, so unlike the Linux evdev backend
it never synthesises a PID descriptor.

**Cannot be fixed at the macOS layer.** Presenting the wheel to macOS as a
force feedback device is closed off for reasons unrelated to code signing.
`ForceFeedback.framework` only reaches devices whose driver published an
`IOCFPlugInTypes` plug-in, and `IORegistryEntry::setProperties` returns
`kIOReturnUnsupported`, so userspace cannot inject one.
`IOHIDUserDeviceCreate` needs `com.apple.developer.hid.virtual.device`, the
same restricted-entitlement wall as DriverKit.

So the force feedback device gets presented **inside the bottle** instead.

## How it will work

```
game (in the bottle)
  |  DirectInput 8
  v
t150-dinput8.dll   proxy, forwards everything except force feedback
  |  normalized effects over 127.0.0.1
  v
t150d              macOS daemon, unprivileged, no entitlement
  |  IOHIDDeviceSetReport
  v
the wheel
```

The wheel still reaches the game through the normal path for input, so there
is no synthetic device, no descriptor splicing and no duplicate wheel to
hide. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), including why an
in-bottle bus driver was considered and rejected.

## What exists today

| Component | State |
| --- | --- |
| `probe_hid`, `probe_setreport`, `probe_ep0` | working, macOS only |
| `include/t150/*.h` shared contracts | written, compiled and tested on Linux |
| build, CI, docs, man pages | working |
| `t150d`, `t150-dinput8.dll`, `t150boot`, `t150ctl` | not started |

## Building

On Linux, which builds and tests everything portable:

```sh
make
```

On macOS, which additionally builds the probe tools:

```sh
make
make probes
```

`make strict` is the same with warnings as errors, and is what CI runs.
`make help` lists the targets.

Development happens on Linux; the Mac is only needed to run the probes.
Because the probe sources cannot be compiled on Linux, CI builds them on
`macos-latest` on every push and attaches them as an artifact, so a Mac is
not needed to get a binary either.

## Running the probes

Read [`docs/PROBES.md`](docs/PROBES.md) first. In short, on the Mac with the
wheel plugged in:

```sh
./build/bin/probe_hid -o .     # what does macOS publish for this wheel
./build/bin/probe_setreport    # does an unprivileged write move it
./build/bin/probe_setreport -A # stop it again
./build/bin/probe_ep0          # does endpoint 0 work, and as whom
```

The second one is the decisive measurement. Note that a success return is
not the answer: macOS can accept a report the firmware then ignores, so what
settles it is whether the wheel physically reacted.

## Scope

The T150 only. Other Thrustmaster and Logitech wheels use different wire
protocols and are out of scope by choice, not by accident.

The T150 renders constant force, the five periodics, spring and damper in
hardware. Friction and inertia are not in its protocol, and neither is ramp;
those will be downgraded rather than refused, because a game that gets a
refusal from `CreateEffect` may disable force feedback altogether.

## Prior art

- [scarburato/t150_driver](https://github.com/scarburato/t150_driver) and
  [hid-tminit](https://github.com/scarburato/hid-tminit), the Linux drivers
  every wire constant here is traced back to.
- [Kimplul/hid-tmff2](https://github.com/Kimplul/hid-tmff2), the newer
  Thrustmaster family, useful for contrast: it drives its wheels through the
  HID layer, which the T150 driver does not.
- [eddieavd/fffb](https://github.com/eddieavd/fffb), which drives Logitech
  wheel force feedback from unprivileged userspace on Apple Silicon with
  `IOHIDDeviceSetReport`. Different wheel, but it is the evidence that the
  macOS half of this design is possible at all.
- [CrossWheel](https://crosswheel.seastian.com/), a commercial macOS product
  that already ships this architecture, a proxy DLL in the bottle plus a
  macOS app. It is not open source and its Thrustmaster support targets the
  T300RS protocol rather than the T150's.

## License

BSD-2-Clause. See [`LICENSE`](LICENSE).
