# Architecture

## The constraint

Apple only signs DriverKit entitlements for paid teams granted DriverKit
case by case, so a system extension is not a path that ends anywhere useful.
Every route that re-exposes the wheel as a force feedback device to *macOS*
is closed for reasons that have nothing to do with signing:

- `ForceFeedback.framework`, the API SDL and native Mac games use, can only
  reach a device whose driver published an `IOCFPlugInTypes` plug-in in the
  IOKit registry, and `IORegistryEntry::setProperties` returns
  `kIOReturnUnsupported`, so userspace cannot inject one.
- `IOHIDUserDeviceCreate` needs `com.apple.developer.hid.virtual.device`,
  which is the same restricted-entitlement wall as DriverKit.

So the force feedback device cannot be presented to macOS. It has to be
presented inside the bottle.

## What already works

Two things are already true with nothing installed, and the design leans on
both:

- macOS enumerates the wheel as an ordinary joystick once it is in firmware
  mode, so axes, pedals, buttons and the hat already reach games.
- CrossOver carries it into the bottle without any driver, and it opens the
  device without seizing it, so a second process can still write to it.

Only the force feedback output channel is missing. On macOS the wheel reaches
the bottle through winebus's SDL backend, not its IOHID one (RESEARCH.md B8),
and that backend synthesises a PID descriptor only for a device SDL considers
haptic. SDL's macOS haptic backend is `ForceFeedback.framework`, which only
reaches devices whose driver published a plug-in, so it considers no
driverless wheel haptic (B9). DirectInput therefore reports no force feedback,
because `dlls/dinput/joystick_hid.c` sets `DIDC_FORCEFEEDBACK` and
`guidFFDriver` only from PID collections it finds in the descriptor.

## The shape

```
game (in the bottle)
  |  DirectInput 8
  v
t150-dinput8.dll  (PE, built with mingw-w64, installed into the bottle)
  |  normalized effects, TCP on 127.0.0.1
  v
t150d             (macOS, unprivileged, no entitlement)
  |  IOHIDDeviceSetReport
  v
the wheel
```

The proxy forwards everything it does not care about to a renamed copy of
CrossOver's builtin `dinput8.dll` and wraps only the force feedback surface:
`GetCapabilities`, device enumeration under the `DIEDFL_FORCEFEEDBACK`
filter, `EnumEffects`, `CreateEffect`, `IDirectInputEffect::SetParameters`,
`Start` and `Stop`, `SetProperty` for `DIPROP_FFGAIN` and
`DIPROP_AUTOCENTER`, and `SendForceFeedbackCommand`.

The wheel keeps arriving in the bottle through the normal path for input, so
there is no synthetic device, no descriptor to splice, and no duplicate wheel
to hide.

### Why not a bus driver inside the bottle

The obvious alternative is a WDM bus driver in the bottle publishing a
synthetic HID device carrying a PID descriptor, letting Wine's own
DirectInput do the force feedback work. It covers more (DirectInput 7,
RawInput, in-bottle SDL) but it was rejected for this project because the
seams are real and were confirmed against CrossOver's published sources:

- `hidclass.sys` always marks the write IRP pending and DirectInput's
  `WriteFile` waits `INFINITE` on winedevice's single request thread, so any
  blocking send freezes the game rather than degrading.
- `hidclass.sys` silently drops input reports shorter than the length its
  descriptor parse declared.
- `hid_device_thread` abandons pending `IOCTL_HID_READ_REPORT` IRPs without
  cancelling them, so the FDO buffer is freed while a driver still holds it.
- Nothing tells the driver when a game exits, because `hidclass` consumes
  `IRP_MJ_CLOSE` at the PDO.
- Hiding the native copy of the wheel needs three registry values working
  together, and one of them costs every other controller in that bottle its
  rumble.

The proxy removes all five. Should a target game turn out not to use
DirectInput 8, the bus driver is the escalation, not the starting point.

### Latency and the watchdog

The proxy is ordinary user-mode code, so an effect update is a COM call plus
a loopback socket write, with no wineserver round trip and no IRP path.

Nothing in the stack learns that a game exited. A crashed or force-quit game
sends no reset, which would leave the last commanded force latched on a wheel
that pulls hard. The daemon therefore treats silence as a fault: see
`T150_WATCHDOG_MS` in `include/t150/proto.h`.

## Layout

```
include/t150/     the contracts, portable, compiled and tested on Linux
  t150.h            wire constants, every one traced to a cited source
  effect.h          the normalized effect model the DLL sends
  proto.h           the DLL to daemon wire protocol
  encode.h          normalized effects to wheel packets
src/lib/          the portable half: encoders and the protocol codec
src/t150d/        the daemon: session logic, backends, the socket loop
src/dll/          the in-bottle proxy, cross built to an x86_64 PE
src/probe/        the Phase 0 measurement tools, macOS only
tests/            what CI can run without a Mac or a wheel
docs/             HANDOFF.md, RESEARCH.md, PROBES.md, PROTOCOL.md, this file
```

The daemon is split so that `session.c` holds every rule and touches neither
a socket nor a clock: it is handed a frame and the current time. That is what
lets the watchdog and the ramp slicer be tested in simulated time rather than
by sleeping, and it keeps the part that has to be right small enough to read.

## Status

Implemented: the three probe tools, the shared headers, the encoders, the
protocol codec, the daemon's logic, the proxy, the build and CI.

Not implemented: the daemon's macOS HID backend, `t150boot`, `t150ctl`. Until
the backend exists the daemon writes its packets to a log, which is enough to
drive the whole stack from a test without a Mac.

The proxy has never been executed. It cross builds and CI loads it on
Windows, but there is no Wine on the development machine, so whether a bottle
resolves its chain-load is unverified.

The build order is Phase 0 first (see [PROBES.md](PROBES.md)), because a
single measurement decides whether any of the rest is worth writing: whether
an unprivileged `IOHIDDeviceSetReport` physically moves the wheel.
