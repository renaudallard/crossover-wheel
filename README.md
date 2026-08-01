# crossover-wheel

Force feedback for the **Thrustmaster T150** in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval.

> **Status: pre-implementation.** Only the Phase 0 measurement tools, the
> shared headers and the build exist. The project is deliberately gated on
> one experiment, described in [`docs/PROBES.md`](docs/PROBES.md), because
> if it fails nothing else here is worth writing.

**Picking this up?** Read [`docs/HANDOFF.md`](docs/HANDOFF.md) first. It is
written for someone starting with no context: what is decided, what is
verified, what is still unknown, and what to build in what order.
[`docs/RESEARCH.md`](docs/RESEARCH.md) is the evidence behind every claim,
including the routes that were investigated and are dead.

## Why this exists

A T150 on a Mac is already half working, and it is worth being precise about
which half.

**Already works, with nothing installed.** macOS enumerates the wheel as an
ordinary joystick once it is in firmware mode, and CrossOver passes it into
the bottle. Axes, pedals, buttons and the hat reach games today.

**Does not work.** Force feedback. Wine's DirectInput sets
`DIDC_FORCEFEEDBACK` only from a Physical Interface Device collection it
finds in a descriptor, and nothing puts one there. On macOS the wheel arrives
through winebus's SDL backend rather than its IOHID one, because winebus
discards the IOHID copy of any Generic Desktop joystick that is not on its
hidraw allow-list. The SDL backend would synthesise a PID collection, but
only for a device SDL calls haptic, and SDL's macOS haptic backend is
`ForceFeedback.framework`, which reaches only devices whose driver published
a plug-in. No wheel vendor ships one.

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
| `src/lib/encode.c` wire encoders | written, golden-vector tested on Linux |
| `src/lib/proto.c` DLL to daemon protocol | written, round-trip tested on Linux |
| `t150d` protocol, slots, downgrades, watchdog | written and tested on Linux |
| `t150d` macOS HID backend | not started |
| build, CI, docs, man pages | working |
| `t150-dinput8.dll`, `t150boot`, `t150ctl` | not started |

The encoders turn a normalized effect into the wheel's packets and are the
only code that knows both DirectInput units and wheel units. They do no I/O,
so `make test` checks every byte they produce against vectors derived from
`docs/PROTOCOL.md`, on any machine.

The daemon is complete except for the part that touches a wheel. It listens,
speaks the protocol, keeps the slot table, downgrades the effects the wheel
cannot render, slides ramps, and runs the watchdog; the packets go to a log
rather than to hardware. That is enough to drive the whole stack from a test
without a Mac, which is what `socket_check` does, including holding a socket
open and going quiet to prove the wheel gets released.

None of this says the wheel agrees with the bytes. That is
[`docs/PROBES.md`](docs/PROBES.md), and it has not been answered.

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

## Running the daemon

`make` builds `build/bin/t150d`. It drives nothing yet and says so:

```sh
./build/bin/t150d -v
t150d: listening on 127.0.0.1:49713, endpoint .../t150ffb/endpoint
t150d: backend fake, no wheel is being driven
```

Every packet it would have sent to the wheel is printed instead, in the same
form `probe_setreport` prints, so the two can be compared directly. See
[`t150d(8)`](man/t150d.8) for the endpoint file, the watchdog and the effect
downgrades.

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

The T150 only, on macOS 26 or newer, on Apple Silicon. Other Thrustmaster and
Logitech wheels use different wire protocols and are out of scope by choice,
not by accident.

In the bottle, DirectInput 8 games and SDL games both count: SDL implements
Windows force feedback over DirectInput 8, so the same proxy serves both, as
long as it is installed where SDL's `CoCreateInstance` will find it.

XInput does not count, and cannot. winebus only marks a device XInput capable
for vendor `0x045e`, so a T150 never appears there, and XInput carries two
rumble motors rather than force feedback effects.

Native macOS games are close to unreachable: no public API drives an
arbitrary HID wheel, and library validation blocks injecting into signed
games. The one exception is Euro Truck Simulator 2 and American Truck
Simulator, which load third-party telemetry plugins, and even there the game
sends no forces, so any plugin has to invent them from telemetry.

The T150 renders constant force, sine, both sawtooths, spring and damper in
hardware. Square and triangle are not in its protocol despite being periodics,
and neither are friction, inertia or ramp. Those are downgraded rather than
refused, because a game that gets a refusal from `CreateEffect` may disable
force feedback altogether.

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
