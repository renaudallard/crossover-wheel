# crossover-wheel

Force feedback for the **Thrustmaster T150** in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval.

> **Status: nothing here drives a wheel yet.** The measurement the whole
> design rests on has never been made, and until it is, the daemon logs its
> packets instead of sending them. It is described in
> [`docs/PROBES.md`](docs/PROBES.md), it needs a Mac and a wheel and about
> half an hour, and if it fails most of this project is wasted rather than
> merely early. Everything that can be built and tested without a wheel has
> been: see [what exists today](#what-exists-today).

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
| `t150-dinput8.dll` the in-bottle proxy | written, cross builds, never yet run |
| build, CI, docs, man pages | working |
| `t150boot`, `t150ctl` | not started |

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

The proxy is written and cross builds to an x86_64 PE with the right five
exports and no import of `dinput8` to recurse into. **No line of it has ever
executed.** There is no Wine on the development machine and no Mac here, so
the checks that exist run in CI on Windows: they unit test the DirectInput
conversion, then load the DLL with a copy of the system `dinput8` beside it
and confirm both entry points chain-load. Whether a Wine bottle resolves the
same way is the first thing M5 has to try.

None of this says the wheel agrees with the bytes. That is
[`docs/PROBES.md`](docs/PROBES.md), and it has not been answered.

## What needs doing next

Everything that can be built without hardware has been. What is left starts
with two experiments, both needing the Mac, and **they are independent of
each other**: the second is worth running even if the first fails, and if the
first succeeds the second is the next thing in the way regardless.

**1. Answer the gate**, step by step under
[testing it today](#testing-it-today). Does an
unprivileged, non-seizing `IOHIDDeviceSetReport` physically move the wheel?
Half an hour, and nothing downstream is worth building until it is answered.
While you are there it also settles three things the encoders currently guess
at: whether a constant force tops out at `0x40` or `0x7f`, whether the
contiguous type codes `0x4020` and `0x4021` are the square and triangle the
Linux driver never implemented, and whether the wheel obeys settings in boot
mode, which would remove the endpoint 0 problem entirely.

**2. Try the proxy in a bottle**, step by step under
[testing it today](#testing-it-today). The DLL has never run under Wine, and
finding out whether it loads and whether a game's effects reach the daemon
needs no working force feedback at all, which is why it does not wait for the
gate.

Then, and only if the gate said yes:

**3. The macOS HID backend.** The one missing piece of the daemon: device
matching, a non-seizing open, output writes and hot plug, behind the backend
interface `backend_fake.c` already implements. `src/probe/common.c` is
already the non-seizing enumeration it needs and should be moved rather than
rewritten.

**4. `t150ctl` and `t150boot`.** Rotation range, gain and autocenter from the
command line; and the boot to firmware mode switch, if the gate found the
wheel needs one. Ship the switch as a LaunchAgent matching the boot product
id, because sleep, wake and replug all drop the wheel back.

**5. Robustness, then a real game.** Reconnect on both ends, hot plug, and
the watchdog under real crash conditions. Measure the latency and jitter of
the whole path under Rosetta while you are there: a wheel wants updates near
500 Hz and nobody has measured it.

Later, and not blocking: an ARM64EC build for CrossOver 27's bottles, an
installer that does the bottle setup in one step, and optionally an SCS
telemetry plugin, which is the only way any native macOS game can be reached.

If the gate says no, the ladder of fallbacks is in
[`docs/HANDOFF.md`](docs/HANDOFF.md) section 8, and the honest last rung is
to say so and stop.

## Using a release

Prebuilt binaries are on the
[releases page](https://github.com/renaudallard/crossover-wheel/releases),
built by CI from the tagged commit. Two archives:

| Archive | Contains |
| --- | --- |
| `crossover-wheel-<v>-macos-arm64.tar.gz` | `probe_hid`, `probe_setreport`, `probe_ep0`, `t150d` |
| `crossover-wheel-<v>-windows-x86_64.zip` | `t150-dinput8.dll`, the in-bottle proxy |

Apple Silicon only, and there is no Intel build. Verify what you downloaded
before running it:

```sh
shasum -a 256 -c SHA256SUMS
tar xzf crossover-wheel-<v>-macos-arm64.tar.gz
cd crossover-wheel-<v>-macos-arm64
```

**The binaries are unsigned and not notarized.** Downloaded through a browser
they are quarantined and macOS will refuse to run them, with a message about
an unidentified developer. Either fetch them with `curl`, which sets no
quarantine attribute, or clear it:

```sh
xattr -d com.apple.quarantine probe_hid probe_setreport probe_ep0 t150d
```

Two more things will otherwise cost you a session, both of them macOS rather
than this project:

- **Sit at the machine.** `IOHIDDeviceSetReport` is gated on being the
  console user, so `probe_setreport` and `t150d` fail over SSH, from the
  login window, and from a fast-user-switched session.
- **Approve the wheel.** On an Apple Silicon laptop, System Settings, Privacy
  and Security, Accessories. Until you do, the wheel appears nowhere and
  `probe_hid` reports no matches.

What to actually do with them is
[what needs doing next](#what-needs-doing-next): the probes answer the gate,
and the daemon and the proxy together let the bottle half be tried even
before it is answered. Bear in mind throughout that `t150d` has no macOS HID
backend yet, so it logs rather than driving: what you can see today is a
game's effects arriving as wheel packets, which is worth seeing and is not
force feedback.

Commands below are written as `./build/bin/...` because that is where a
source build puts them; from a release archive, run them from the directory
you just extracted.

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

Development happens on Linux; the Mac is only needed to run the probes and
the daemon. Because the probe sources cannot be compiled on Linux, CI builds
them on `macos-latest` on every push and attaches them as an artifact, so a
Mac is not needed to get a binary either.

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

## Installing the proxy into a bottle

`make dll` cross builds it, and needs `gcc-mingw-w64-x86-64`. It has to go in
the bottle's `system32` rather than beside a game, because SDL reaches
DirectInput through `CoCreateInstance`, which the loader resolves to an
absolute `system32` path and never to a game directory. Two files and one
registry value:

```sh
CX_ROOT=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver
BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/<name>"

# the real implementation, under the name the proxy chain-loads
cp "$CX_ROOT"/lib*/wine/x86_64-windows/dinput8.dll \
   "$BOTTLE/drive_c/windows/system32/dinput8_orig.dll"
cp build/bin/t150-dinput8.dll \
   "$BOTTLE/drive_c/windows/system32/dinput8.dll"

"$CX_ROOT/bin/wine" --bottle "<name>" --cx-app reg.exe add \
    'HKCU\Software\Wine\DllOverrides' /v dinput8 /t REG_SZ /d native,builtin /f
```

From a release archive, use `t150-dinput8.dll` from the extracted directory
in place of `build/bin/t150-dinput8.dll`.

Set `T150_DEBUG=1` in the bottle to make the proxy say what it is doing, and
`T150_ENDPOINT` to point it at the daemon's endpoint file if the default
guess is wrong. Verify the chain-load with `WINEDEBUG=+loaddll`: the
`dinput8_orig.dll` line must say `builtin`.

None of this has been tried in a real bottle yet. If the chain-load does not
resolve, the fallbacks are in [`docs/HANDOFF.md`](docs/HANDOFF.md) under M4.

## Testing it today

Every step, in order, for the two things that can be tested right now.
[`docs/PROBES.md`](docs/PROBES.md) is the authority on what each outcome
means; this is the sequence to type.

Both need the Mac. Three things first, each of which otherwise costs a whole
session:

- **Put the wheel's switch in the PS3 position** before plugging it in. The
  selector decides which device macOS sees: PS3 gives `044f:b65d`, the shared
  boot identity that the mode switch takes to `044f:b677`, which is what this
  project drives. PS4 gives `044f:b66d`, a DualShock 4 shaped device with a
  different protocol that nothing here applies to.
- **Sit at the machine.** `IOHIDDeviceSetReport` is gated on being the
  console user and fails over SSH.
- **Approve the wheel** on an Apple Silicon laptop, under System Settings,
  Privacy and Security, Accessories.

The command blocks carry no inline `# comments`, deliberately. macOS's zsh
does not treat `#` as a comment when you paste a line interactively, so a
trailing comment arrives as arguments, and every tool here rejects unexpected
arguments by printing its usage and exiting without touching the device. A
run can look like it happened and have done nothing at all.

### Test A: does the wheel obey at all

The decisive one. About half an hour, wheel plugged in, everything as your
normal user unless a step says otherwise.

**A1. See what macOS publishes.** Never opens the device, so it disturbs
nothing:

```sh
system_profiler SPUSBDataType | grep -A5 -i thrustmaster
./build/bin/probe_hid -o .
```

Record the product id (`B65D` is boot mode, `B677` is firmware mode), how
many HID nodes appear for the one wheel, each node's usage page and usage,
each `MaxOutputReportSize`, and whether `ProtectedAccess` is present.

**A2. If A1 said `B65D`, switch the wheel to firmware mode.** Do this before
A3, not after: until it succeeds the wheel is not the device this project
drives, and it may not even turn. Read only by default, so the first three
are safe:

```sh
./build/bin/probe_ep0
sudo ./build/bin/probe_ep0
sudo ./build/bin/probe_ep0 -s
```

In order: as your user, then as root to compare, then seizing the device as a
last resort. Each prints the model and attachment bytes of whatever wheel it
found, which is what identifies the model. Only once a query has succeeded,
and only then, perform the switch and confirm it:

```sh
sudo ./build/bin/probe_ep0 -w
./build/bin/probe_hid
```

If the model line does not say T150, pass that model's switch value with
`-V`; the tool prints the bytes it read so you can look the value up. Record
which of those first succeeded and as whom. That decides whether the
finished tool needs a password once per plug-in or never. If only `-s` works,
stop and reassess: seizing takes the wheel away from CrossOver.

**A3. The measurement.** First check the wheel turns freely by hand. One
measured wheel sat locked rigid in boot mode and stayed locked through every
write, which is not an answer: a wheel already held immovable cannot show an
autocenter spring or a shorter lock to lock, so there is nothing to watch
for. If it will not turn, do A2 first and come back.

Then autocenter, because its effect is unmistakable, the wheel starts pulling
itself to centre:

```sh
./build/bin/probe_setreport
./build/bin/probe_setreport -A
```

**A success return is not the answer.** macOS can accept a report the
firmware then discards. What settles this is whether the wheel physically
moved.

**A4. If it returned success and nothing moved,** work the framings before
concluding anything:

```sh
./build/bin/probe_setreport -i 0x0a
./build/bin/probe_setreport -P
./build/bin/probe_setreport -i 0x0a -P
./build/bin/probe_setreport -n 1
./build/bin/probe_setreport -p 0xb65d
./build/bin/probe_setreport -r 270
./build/bin/probe_setreport -r 1080
```

Those are, in order: the declared output report id, zero-padding to the
declared size, both together, the other HID node if there is one, the boot
mode product id, and then the rotation range instead of the spring, since a
firmware that silently drops one opcode may accept another. `-r 270` should
make lock to lock obviously short and `-r 1080` put it back.

Watch for `no HID node matches`. `probe_setreport` defaults to the T150's
firmware product id, so against any other wheel, or against one still in boot
mode, every one of these does nothing until `-p` names the id `probe_hid`
actually reported.

**A5. If the wheel moved,** prove the force feedback protocol too. Four
packets on one open handle, a constant force at half level, endless:

```sh
./build/bin/probe_setreport -g 0x60
./build/bin/probe_setreport \
    -x "02 1c 00 00 00 00 00 00 00 46 54" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

**Hold the wheel or keep a hand on the plug**, then stop it:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

**A6. Three cheap answers while you are set up.** Compare `03 0e 00 40`
against `03 0e 00 7f` in A5: if they feel the same, a constant really does
stop at `0x40`. Try `20 40` and `21 40` in place of `00 40` in the third
packet, which would mean square and triangle exist after all. And repeat A3
once with a game running in a bottle, because macOS 26 fails `setReport` for
every client the moment anything seizes the device.

### Test B: does the proxy load in a bottle

Independent of test A, and worth running even if A fails. Needs CrossOver and
any DirectInput 8 or SDL game with force feedback settings. No working force
feedback is required, because the daemon logs rather than drives.

**B1. Install the proxy** as [above](#installing-the-proxy-into-a-bottle).

**B2. Start the daemon** and leave it running where you can see it:

```sh
./build/bin/t150d -v
```

**B3. Start the game** from a terminal so the loader talks:

```sh
WINEDEBUG=+loaddll T150_DEBUG=1 \
    "$CX_ROOT/bin/wine" --bottle "<name>" --cx-app "<game>.exe"
```

**B4. Watch for four things, in order.** Each one that appears rules out a
whole class of failure:

1. `dinput8_orig.dll` loaded as `builtin` in the `+loaddll` output. If it
   says `native`, or does not appear, the chain-load did not resolve and the
   fallbacks are in [`docs/HANDOFF.md`](docs/HANDOFF.md) under M4.
2. `t150-dinput8: connected to the daemon on port ...` from `T150_DEBUG`. If
   this is missing, the proxy could not find the endpoint file; set
   `T150_ENDPOINT` to its Windows path explicitly.
3. `t150-dinput8: wrapped the wheel`. If this is missing, the game's device
   is not being recognised as the T150, or the game never created it.
4. `write ...` lines in the daemon's output as the game's force feedback
   starts. Those are the packets a wheel would have received.

Getting to 4 means every layer above the missing HID backend works.

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
