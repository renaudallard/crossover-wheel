# Start here

Instructions for continuing this project in a fresh session with no prior
context. Read this file, then [RESEARCH.md](RESEARCH.md). Everything else is
detail.

---

## 1. The goal

Give a **Thrustmaster T150** working force feedback in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval, and ideally no root.

The target is **macOS 26 or newer on Apple Silicon**. Bottles are x86_64
under Rosetta 2 today, so the proxy is an x86_64 PE. CrossOver 27 is Apple
Silicon only, drops 32-bit bottles and uses ARM64EC, which still loads an
x86_64 PE, so ARM64EC is a later second target and i386 is never one.

In scope: force feedback for in-bottle DirectInput 8 games, in-bottle SDL
games, and the wheel's own settings through a command line tool.

Out of scope, both deliberately: other wheels, and in-bottle XInput. XInput
is not a judgement call, it is closed. winebus only tags a device XInput
capable when `is_xbox_gamepad()` matches, which requires vendor `0x045e`, so
a T150 can never appear there; and XInput carries two rumble motors and no
force feedback effects at all. Faking it would give a game a phantom gamepad
that duplicates the steering axis and lures it away from the DirectInput
device that is the only real path.

Native macOS games are a footnote rather than a scope. No public macOS API
drives an arbitrary HID wheel: `ForceFeedback.framework` reaches only devices
whose driver published a plug-in, `GCRacingWheel` has no haptics at all, and
library validation blocks injecting into signed games. The one hook that
exists is the SCS telemetry SDK in Euro Truck Simulator 2 and American Truck
Simulator, where forces would be synthesized from telemetry rather than sent
by the game. That is an optional last milestone, and the README must say
plainly that those forces are invented.

## 2. Why the obvious approaches are not available

A DriverKit system extension is not a path: Apple signs DriverKit
entitlements only for paid teams granted DriverKit case by case. That was the
predecessor project (`macoswheels`, same author) and it only ever loaded with
SIP disabled and `amfi_get_out_of_my_way=0x1`.

Presenting a force feedback device to *macOS* by any other route is also
closed, for reasons unrelated to signing: see RESEARCH.md D1 and D2. So the
force feedback device is presented **inside the CrossOver bottle** instead.

## 3. The design

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

The wheel keeps reaching the game through the normal path for input, because
that already works (RESEARCH.md A1, B1). Only the force feedback output
channel is missing, because the wheel's descriptor has no PID collection and
Wine adds none of its own on macOS (RESEARCH.md B3, B4). A wheelbase that
does carry a PID collection gets native force feedback through CrossOver's
hidraw path with no help from anyone (B12); this project exists for the
wheels that do not.

Consequences worth internalising, because they are what make this design
small: there is no synthetic HID device, no report descriptor to build or
splice, no PID report parsing anywhere, and no duplicate wheel to hide.

## 4. Decisions already made

Do not relitigate these without new evidence. Each has its reasoning in
RESEARCH.md.

| Decision | Where |
| --- | --- |
| Proxy DLL, not an in-bottle bus driver | D5 |
| Never seize the HID device | B6 |
| No PID descriptor anywhere in this project | B4, D6 |
| C throughout, no Swift | user's call |
| T150 only | user's call |
| Downgrade unsupported effects rather than refusing them | PROTOCOL.md |
| The proxy installs into the bottle's `system32`, not a game directory | section 7, M4 |
| It exports `DllGetClassObject` as well as `DirectInput8Create` | section 7, M4 |
| x86_64 PE now, ARM64EC later, i386 never | section 1 |
| No in-bottle XInput proxy | section 1 |

## 5. Current state

**The goal is met.** A game drives the wheel: Assetto Corsa, on hardware, with
force feedback its tester compares to the same wheel on Windows under
Thrustmaster's own drivers. Nothing is installed system wide, nothing needs
root, and `crossover-wheel.app` does the whole install from one download.

What is left is short and is listed in the README under "What is left". This
section is the map of the parts.

Implemented and working:

- `src/probe/` : four macOS measurement tools, see section 6. `probe_intr`
  is the newest and writes on the interrupt OUT pipe rather than through the
  HID layer; it needs root because that means capturing the wheel.
- `include/t150/` : wire constants, normalized effect model, wire protocol.
  Portable, compiled and self-tested on Linux.
- `src/lib/encode.c` : M1, the wire encoders. Pure functions, golden-vector
  tested by `tests/encode_check.c` on any machine.
- `src/lib/proto.c` : M2, the DLL to daemon codec.
- `src/t150d/` : M2, the daemon. Socket, session, slot table, downgrades,
  ramp slicing and the watchdog. Driven by `tests/daemon_check.c` in
  simulated time and by `tests/socket_check.c` over a real socket.
- `src/dll/` : M4, the proxy. Cross builds to an x86_64 PE with the right
  exports and no import of `dinput8` to recurse into. It loads in a real
  bottle, wraps the wheel, and a game drives force feedback through it.
- `src/mac/t150menu.m` : `crossover-wheel.app`, the menu bar item and
  installer. Deliberately thin: it runs `install.sh` rather than
  reimplementing it, and runs the daemon as a child rather than speaking its
  protocol, which would displace a running game. **Compiled by CI and never
  clicked** — nothing on a build machine can press a menu bar item, so every
  fault in it so far was found by the person using it.
- `install.sh`, `dist/update.sh` : the install and the self-update, both
  shell because both can destroy a working setup, and both tested against
  synthetic trees and fake bundles.
- `Makefile`, CI, `README.md`, man pages, docs.

The daemon's macOS HID backend, `t150ctl` and `t150boot` have all run against
hardware. `t150d -n` still logs its packets rather than sending them, which is
what every test drives and the only behaviour off macOS.

The probe tools compile clean on `macos-26` with `-Werror` against the
real CoreFoundation and IOKit headers, and they have now been run against a
real T150. What they found is in RESEARCH.md A4 to A21: the wheel identifies
itself, the endpoint 0 mode switch works and needs no privilege, every HID
write returns success, **and the wheel physically obeys them**. Section 6 has
the measurement.

The protocol record was corrected against `scarburato/t150_driver` before any
encoder was written: three of PROTOCOL.md's force feedback values did not
exist in the driver it cites. Square and triangle were then recorded as
effects the wheel has no type code for, which RESEARCH.md A40 falsified by
taking both codes, 0x4020 and 0x4021, from Thrustmaster's own effect type
table. Both are native and neither is downgraded. If you are holding notes
older than that, discard them.

## 6. The gate, and its answer

One measurement decided whether the whole design was viable: does an
unprivileged, non-seizing `IOHIDDeviceSetReport` physically move the wheel?

**It does.** `probe_setreport` sets the autocenter to full and the wheel
becomes hard to turn; `probe_setreport -a 0` releases it and it turns freely.
No root, no device capture, and CrossOver keeps reading the wheel throughout.
RESEARCH.md A19.

This section originally said not to write the daemon or the DLL until it was
answered. Both were written anyway, deliberately, because everything in them
is testable without a wheel and the encoders turned out to be what the gate
itself needed to send a real effect by hand. That gamble came good.

It was genuinely uncertain rather than merely unconfirmed, and worth the
caution: the T150's Linux driver bypasses the HID layer entirely and writes
raw on the interrupt OUT pipe, while the newer T300 family goes through
`hid_hw_request(SET_REPORT)`. See RESEARCH.md C3 and C5, and C7, which
predicted the answer would be no.

The procedure, what to record and what each outcome means are in
[PROBES.md](PROBES.md). It needs the Mac, the wheel and about half an hour.
Read its prerequisites first: on an Apple Silicon laptop the accessory needs
approving before it appears at all, and `setReport` is gated on the console
user, so none of this can be done over SSH.

**Some of it is now measured.** A T150 has been in front of the probes. The
endpoint 0 model query succeeds unprivileged, the mode switch lands, the
wheel reaches `b677`, no node carries `ProtectedAccess`, and writes still
return success with a game running. RESEARCH.md A4 to A8 has the detail and
PROBES.md says which steps no longer need repeating.

**The one that decides the project is answered yes.** `probe_setreport` set
the autocenter to full and the wheel became hard to turn; `probe_setreport
-a 0` released it and it turned freely. Unprivileged, no device capture, and
CrossOver keeps the wheel throughout. `probe_intr` does the same on the
interrupt OUT pipe, so both transports reach the firmware. RESEARCH.md A19.

The earlier runs that concluded the opposite were all sending `-A`, the
autocenter enable flag, which is a no-op on macOS: the effect is active
whenever no application has the wheel's input open, so the flag changes
nothing observable and only the force releases the wheel. RESEARCH.md A15.
C7, which predicted that the HID path could not work at all, is measured
false for this wheel.

So there is no ownership conflict for settings, `t150ctl` needs no
privilege, and section 8's fallbacks are not needed for that half.

**Force feedback works.** That paragraph used to say it did not: with the
autocenter cleared and the gain set, an effect moved nothing on either pipe.
The missing piece was one two-byte packet, `42 04`, which opens the wheel's
input; the firmware renders nothing until something does. RESEARCH.md A26 and
A28. Everything after that is history rather than an open question.

Two traps, and the second one cost this project six sessions:

- A `kIOReturnSuccess` return is **not** the answer. macOS can accept a
  report the firmware then discards. What settles it is whether the wheel
  physically reacted.
- **Release the wheel with `-a 0`, never with `-A`.** The wheel rests holding
  a full autocenter and feels locked, and `-A` only clears a flag that says
  whether the autocenter survives an application opening the input. It does
  nothing while nothing has the input open, which on macOS is always. Every
  run that concluded the firmware ignores us was sending it. A wrong "no"
  here nearly killed a viable project.

## 7. Build order after the gate

Each milestone ends in something checkable. Do not start the next until the
previous one is verified.

**M1. Encoder, on Linux. Done.** `src/lib/encode.c` and
`include/t150/encode.h`. Pure functions, no I/O, no allocation: the caller
supplies the buffer and the function returns the byte count, or 0 if it
refuses. All DirectInput to wire conversion lives here and nowhere else,
which is what keeps the DLL free of wheel knowledge and the daemon free of
DirectInput knowledge.

Read the flagged conversion in [PROTOCOL.md](PROTOCOL.md) before trusting the
output: the constant level ceiling is derived rather than transcribed. The
envelope level scaling and the start delay unit were on that list and are not
any more, both settled by A40 against the vendor's descriptor: the envelope
levels are 0 to 127, and the delay is a whole 16-bit millisecond field rather
than a byte counting 256s.
`t150_effect_downgrade()` is here too, so M2 does not have to invent it.
*Done:* `make test` is green with golden vectors for every packet, derived
from PROTOCOL.md independently of the encoder, including the `0x43` gain
narrowing, the `0x40 0x11` range scaling and full uploads of a constant, a
periodic and both conditions.

**M2. Protocol and daemon core, on Linux. Done.** `src/lib/proto.c` and
`src/t150d/`. The session holds every rule and touches neither a socket nor a
clock, which is what makes the watchdog testable without waiting; `main.c`
owns the socket and the endpoint file; `backend_fake.c` logs what a wheel
would have received.

Two decisions worth knowing before extending it. A per-effect gain is folded
into the magnitude, because the wheel has one device gain and no per-slot
one. A ramp is uploaded as a constant at its start value and re-sent every
20ms as it slides, which is the only way the wheel can express one.
*Done:* `daemon_check` drives every rule in simulated time against the
golden vectors, and `socket_check` runs the real daemon, speaks the protocol
over loopback, then goes quiet with the socket still open and watches the
wheel get released.

**M3. macOS HID backend. Written.** `src/t150d/hid_darwin.c` does the device
matching, the non-seizing open, the output writes and the hot plug, and
replays the wheel's input open after a re-acquire because a replugged wheel
forgets. `src/probe/common.c` was left where it is rather than promoted: the
probes enumerate, read and drop, while the daemon has to hold a device across
a replug, so sharing the code would have meant sharing the wrong lifecycle.

**`t150ctl` and `t150boot` are written too**, in `src/tools/`. Both take the
unprivileged path and share the probes' enumeration, which fits because they
have the probes' lifecycle: find the wheel, do one thing, let go.

`t150boot` does the two endpoint 0 transfers and nothing else. It does not
send the five interrupt OUT initialisation packets, because those need the
device captured and therefore root, and because whether they matter is
unsettled: they were adopted to explain a wheel that came back apparently
blocked, and that turned out to be the autocenter. `probe_intr -I` remains
the tool for them.

Neither claims a USB interface, deliberately: an endpoint 0 device request
needs no claim, and claiming one on a HID-owned interface is both refused and
currently reported to panic macOS 26.

What is left is packaging. `t150boot` should ship as a user LaunchAgent
matching the boot product id so it fires on every plug-in, because sleep,
wake and replug all drop the wheel back, and `t150d` as another so a game
never has to be told to start it.
*Done when:* on the Mac, `t150ctl range 270` visibly shortens lock to lock
and `t150ctl autocenter 0` releases the spring, with no password prompt,
while CrossOver still reads the wheel.

**M4. Proxy DLL. Done.** `src/dll/`, cross built with mingw-w64 to an x86_64
PE. `make dll` builds it and its test. Assetto Corsa creates its effects
through it on real hardware and the wheel renders them.

It wraps `IDirectInput8`, `IDirectInputDevice8` and `IDirectInputEffect`, and
only for the one device whose product id is the wheel's: everything else is
handed to the builtin unwrapped, so nothing else in the bottle pays for this
DLL existing. `GetCapabilities` gains the force feedback flags, `EnumDevices`
under `DIEDFL_FORCEFEEDBACK` enumerates a second time to smuggle the wheel in
when Wine left it out, and `CreateEffect` returns an effect that talks to the
daemon. `DIPROP_FFGAIN`, `DIPROP_AUTOCENTER`, `SendForceFeedbackCommand` and
`Unacquire` all reach the daemon. With no daemon answering, nothing is
wrapped and the proxy is transparent.

Two things it does that are easy to get wrong. It exports `DllGetClassObject`
as well as `DirectInput8Create`, because SDL only ever uses the COM door; that
is also why it installs into `system32` rather than a game directory. And a
one-axis DirectInput effect is normalized to due east, because DirectInput
carries the side in the sign of the magnitude and the encoder would otherwise
project a northward direction onto no force at all.

**What is checked, and what is not.** `tests/dll_check.c` runs on Windows in
CI: it unit tests the DIEFFECT conversion, then loads the DLL with a copy of
the system `dinput8` beside it and confirms both entry points chain-load.
And the chain-load in a real bottle is now verified too: test 14 loaded the
proxy as `native` in a CrossOver 26 bottle, chain-loaded the builtin from
`dinput8_orig.dll`, and forwarded `DllRegisterServer` into it, with the
builtin's imports visibly resolving (RESEARCH.md A33). The force feedback
path has since run end to end, first from the in-bottle probe (A43) and then
from a game: **Assetto Corsa creates its effects through the proxy and the
wheel renders them.**

**M5. Done: force feedback in a real game, and an installer.** The installer
exists twice over, as `install.sh` and as the application that runs it, and it
finds the bottle under `~/Library/Application Support/CrossOver/Bottles`,
copies the DLL into `system32` and sets the override
with `wine --bottle <name> --cx-app reg.exe`. Scope the override to
`AppDefaults\<game>.exe` when the bottle holds more than the target game.
The `dinput8_orig.dll` it puts beside the proxy has to come from
`$CX_ROOT/lib/wine/x86_64-windows/dinput8.dll` and never from the bottle's own
`system32`, which holds a placeholder Wine will not load as an implementation.
The README section on installing the proxy has the rest of what CrossOver 26
does with overrides, tracing and the environment.
*Done when:* a real title's force feedback settings are live and the wheel
pushes back.

**M6. Robustness.** The watchdog under real crash conditions, reconnect on
both ends, hot plug, and docs. Measure the latency and jitter of the whole
COM call to loopback to daemon to USB path under Rosetta while you are here,
because a wheel wants updates near 500 Hz and nobody has measured it.
*Done when:* unplug and replug mid-game recovers; killing the daemon leaves
the wheel limp rather than latched.

**Optional N. Native macOS games.** Only after M5, and only if still wanted.
An SCS telemetry plugin for Euro Truck Simulator 2 and American Truck
Simulator driving the same daemon. See section 1 for why this is the only
native title reachable and why the forces are invented.

The watchdog is not optional and is easy to forget. Nothing in the stack
learns that a game exited: Wine's `hidclass.sys` consumes `IRP_MJ_CLOSE` at
the PDO and DirectInput sends a reset only on a graceful unacquire. A crashed
or force-quit game would otherwise leave the last commanded force latched on
a wheel that pulls hard. Treat silence as a fault: see `T150_WATCHDOG_MS`.

## 8. If the gate fails

In order of preference.

1. **Wrong framing.** Exhaust section 6's list first. This is the most likely
   explanation for a failure.
2. **Wrong node.** If the wheel publishes a separate vendor-usage node, the
   output report may only be reachable there (RESEARCH.md E6). Note that
   winebus ignores vendor-usage nodes, so this affects only the daemon, not
   the bottle.
3. **Raw interrupt OUT.** Needs USB device capture, which needs root
   (RESEARCH.md D4), and capture terminates every other client of the device
   including CrossOver's view of it. This is a different architecture, not a
   degraded mode. Reassess before building it.
4. **Stop.** Say so plainly. A wheel whose firmware ignores HID output
   reports and whose interface cannot be captured without breaking CrossOver
   cannot be driven this way, and the honest outcome is to report that rather
   than build something that does not work.

## 9. Working rules

The author's global instructions apply. The project-specific ones:

- Development happens on Debian arm64. The Mac is for installing and testing
  only, and builds are CI-only. Do not assume a Mac is reachable.
- Keep as much as possible buildable and testable on Linux. `include/` and
  the encoders are portable by design; that is deliberate, not incidental.
- `make strict` is what CI runs. Keep it clean.
- The probe sources cannot be compiled on Linux. If you change them,
  type-check against stub headers before claiming they build, and say plainly
  that CI is the first real compile. `make check-mac` does exactly that for
  the two that matter most, `src/mac/t150menu.m` and
  `src/t150d/hid_darwin.c`, against `tests/stubs`; it needs clang. The rest,
  `src/probe/*` and `src/mac/bootswitch.c`, still have nothing but reading,
  because their IOKit surface would have to be guessed at and a wrong guess
  gives a false pass.
- Never claim the wheel works without a physical observation from the Mac. A
  successful return code is not an observation.
- Update `README.md` and the man pages in the same commit as the change they
  describe.
- One commit per change, human-readable message, no attribution lines.
- Never push unless asked.
- KISS and UNIX principles. OpenBSD-quality C: no buffer overflows, no use
  after free, no undefined behaviour, no leaks, no duplicated code.

## 10. Map

| File | What it is |
| --- | --- |
| `docs/HANDOFF.md` | this file |
| `docs/RESEARCH.md` | every verified fact with its citation, the dead ends, the open questions |
| `docs/PROBES.md` | how to run the gate and what each outcome means |
| `docs/PROTOCOL.md` | the T150 wire protocol, and what in it is still unconfirmed |
| `docs/ARCHITECTURE.md` | the design and the rejected alternative |
| `include/t150/t150.h` | wire constants, each traced to a cited source |
| `include/t150/effect.h` | the normalized effect model the DLL sends |
| `include/t150/proto.h` | the DLL to daemon wire protocol |
| `include/t150/encode.h` | normalized effects to wheel packets |
| `src/lib/` | the portable half: the encoders and the protocol codec |
| `src/t150d/` | the daemon, and `man/t150d.8` for how to run it |
| `src/probe/` | the three gate measurement tools |
| `tests/` | what CI can check without a Mac or a wheel |
