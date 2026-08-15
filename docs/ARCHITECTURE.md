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
  mode, and CrossOver carries it into the bottle, with one measured caveat:
  the SDL that CrossOver 26 bundles drops Thrustmaster wheels unless
  `SDL_JOYSTICK_HIDAPI=0` is set in the bottle's environment (RESEARCH.md
  B11, A35). With it set, the wheel reaches games again.
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

That write is bounded. Every wrapped call is a blocking send and two blocking
receives on the thread the game is waiting on, and winsock waits forever by
default, so the first of the five seams above would have come back through the
socket: a daemon that stopped reading without closing would park the frame
indefinitely. The socket carries a send and receive timeout of half the
watchdog, and a timeout drops the connection rather than retrying on it,
because a late reply would arrive against the following call. The reconnect is
rate limited, so the worst a stalled daemon costs is force feedback rather
than the game.

Both ends set `TCP_NODELAY`. Nagle has nothing to hold back while one frame is
outstanding at a time and each reply carries the acknowledgement, which is the
shape this protocol has; it has something the moment a frame or a reply does
not leave in one segment, and then the tail waits for a delayed acknowledgement
rather than for the wheel. Tens of milliseconds on a path whose whole budget is
four is worth two lines to rule out.

The proxy does not send the frame at all when it would carry what the daemon
already has. A `Start` uploads before it starts, so a game that starts an
effect as often as it draws a frame pays two round trips for one of them to do
anything; the upload is skipped when the packed effect matches the last one the
daemon acknowledged. Never the start, which is the one that does anything. It is skipped only while the connection is up and is the
one the effect was uploaded to, the game has not reset the device since, no
start or stop has been refused since, the effect is not a ramp, and the
acknowledgement is under `ASSUME_MS` old. The first of those is also what keeps
the reconnect reachable: `t150_client_call` is where a restarted daemon is
noticed, so a skip that bypassed a dead socket would leave a game repeating one
steady force with nothing to restore it.

The last two are the ones that cost measurement to find. A ramp is never
skipped because the wheel has no ramp: the daemon renders one as a constant it
re-computes as the ramp slides, and an upload is the only thing that puts that
level back to the start, so a skipped upload left a restarted ramp replaying
whatever level it had slid to. For a ramp that has run its course that is its
end value, which was full scale in the case measured.

The age bound exists twice over: the daemon's watchdog clears
its slots with the connection still open, and a write the wheel refuses is
reported on the next `EFFECT_UPLOAD` and nothing else, so skipping uploads is
also how long a failed write can go unreported.

The daemon does not write to the wheel from the frame that arrives. A frame
records what a slot should hold, and a pass sends whatever differs from the
bytes that slot last put on the wire. A pass runs at most once every
`T150_EMIT_MS`, so a game updating faster than that has the superseded values
dropped rather than queued: the wheel holds one value per slot and an
intermediate one was replaced before it could be felt.

That floor is skipped while the backend's writer thread has no backlog.
It was put there when every write was a synchronous IOKit call on the thread
the game was waiting for, and pacing the daemon was the only thing keeping a
frame out of a burst of USB transfers; with `-w` the writer paces itself, so
the floor buys nothing against an empty queue and costs up to a whole period
on a force the wheel could take now. It reasserts itself the moment the queue
has anything in it, which is the case it was really for, and a pass that
failed is never brought forward: the deadline is what stops a wheel that has
gone turning the retry into a spin. Without a writer, `-n` and the fake
backend included, nothing answers the question and the floor always applies.

Only effect parameters are coalesced, because they are state. Starts, stops,
resets, the settings and every path that makes the wheel safe are events, go
out the moment they arrive, and are never merged. A stop drops whatever was
waiting for its slot, so nothing follows an effect that has just stopped.

The split is also why the watchdog is honest: it is evaluated before any
writing the same pass does, so a client's silence is measured against its
last frame rather than against however long the wheel took afterwards.

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
lets the watchdog, the ramp slicer and the emitter's rate cap be tested in
simulated time rather than by sleeping, and it keeps the part that has to be
right small enough to read.

## Status

**The architecture above is what runs.** Assetto Corsa drives a T150 through
it on real hardware: the proxy loads in the bottle, wraps the wheel, forwards
its effects over loopback, and the daemon writes them with
`IOHIDDeviceSetReport` while CrossOver keeps reading the wheel as an ordinary
joystick. Nothing is installed system wide and nothing needs root.

Everything in the diagram is implemented and has run against a wheel: the
probes, the shared headers, the encoders, the protocol codec, the daemon
including its macOS HID backend, `t150ctl`, `t150boot`, and the proxy. The
logging backend is still there and is what lets the whole stack be driven from
a test on a machine with no Mac and no wheel.

`crossover-wheel.app` sits above all of it: a menu bar item that installs the
proxy into a bottle, runs the daemon and updates itself. It is deliberately
thin, because it calls `install.sh` rather than reimplementing it, and it runs
the daemon as a child rather than speaking its protocol, which would displace
a running game.

**What has not been exercised is the application itself.** Nothing on a build
machine can click a menu bar item, so every fault found in it so far was found
by the person using it. That is the one part of this with no automated
coverage, and it is why the risky work stays in the shell script and the
daemon, which do have it.

The build order was Phase 0 first (see [PROBES.md](PROBES.md)), because a
single measurement decided whether any of the rest was worth writing: whether
an unprivileged `IOHIDDeviceSetReport` physically moves the wheel. **It does**,
so the architecture stands as drawn and the daemon never has to take the wheel
away from CrossOver. RESEARCH.md A19.
