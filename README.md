# crossover-wheel

Force feedback for the **Thrustmaster T150** in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval.

> **Status: the wheel pushes back.** Force feedback works on a T150 under
> macOS, from an unprivileged process, which is what this project exists for.
> A constant force drives the wheel steadily to one side and holds it, firmly
> enough to feel and lightly enough to turn back by hand; a periodic makes it
> oscillate. Both transports carry effects: the interrupt OUT pipe and, with
> no root and no device capture, `IOHIDDeviceSetReport`. Replicated across two
> sessions, four runs, with and without the open packet each time.
>
> **The missing piece was one two-byte packet, `42 04`.** The firmware
> renders nothing until something opens the wheel's input, and nothing on
> macOS does. The same fact had been staring at the project for months in the
> Linux driver's own comment, which says the autocenter "is always active
> while no input are open"; four sessions were spent believing the packet's
> bytes were unrecoverable from the published source. They were in
> `t150_init()` the whole time. See [`docs/RESEARCH.md`](docs/RESEARCH.md)
> A26 and A28.
>
> **What is not finished.** No game has reached the wheel end to end.
> `t150boot`, `t150ctl` and the daemon's macOS backend have all now touched
> hardware and worked (A31), the proxy loads and chain-loads in a real
> bottle (A33), and an effect has crossed the loopback and been felt: a
> constant force and a damper, played through the whole chain by
> `probe_dinput -f` with no game involved (A43). What no game has done yet
> is ask for one. A constant force, a damper and two periodics have been
> played on a wheel, though the encoders
> themselves are no longer guesswork: every packet layout and constant is
> now checked against Thrustmaster's own Windows driver, which corrected
> four of them (A40). The wheel reaches the bottle again with
> `SDL_JOYSTICK_HIDAPI=0` set (A35), and the proxy has loaded inside a
> real game; what has still never happened is the game and the daemon
> running at the same time.

**Picking this up?** Read [`docs/HANDOFF.md`](docs/HANDOFF.md) first. It is
written for someone starting with no context: what is decided, what is
verified, what is still unknown, and what to build in what order.
[`docs/RESEARCH.md`](docs/RESEARCH.md) is the evidence behind every claim,
including the routes that were investigated and are dead.

## Why this exists

A T150 on a Mac is already half working, and it is worth being precise about
which half.

**Already works, with nothing installed.** macOS enumerates the wheel as an
ordinary joystick once it is in firmware mode, and early sessions had
CrossOver passing it into the bottle, steering and pedals working in games.

**Broke, was diagnosed, and works again: the wheel in the bottle.** Tests
13 and 15 measured the wheel absent from the bottle entirely, axes
included. The cause was read out of the shipped software: the SDL that
CrossOver 26 bundles, 2.30.12, is the last release whose HIDAPI layer
still claims every Thrustmaster device as a possible PlayStation pad and
drops it. `SDL_JOYSTICK_HIDAPI=0` in the bottle's environment fixes it,
confirmed on hardware in test 16, and belongs in `cxbottle.conf` so every
launch gets it. On the hidraw route the buttons arrive whole, all
thirteen with their identities, and the one remaining input fault was
measured to its cause: the T150's own descriptor labels the pedals
backwards, which `T150_PEDALS` can correct for a game that needs it.
[`docs/RESEARCH.md`](docs/RESEARCH.md) B10, B11, A35 and A37.

**Does not work: force feedback, because the T150 brings no PID
descriptor.** Wine's DirectInput sets `DIDC_FORCEFEEDBACK` only from a
Physical Interface Device collection it finds in a descriptor, and the
T150's declares none. This is precise, not general: a wheelbase that does
carry a PID collection, a Simucube or a Fanatec, gets native untranslated
force feedback in CrossOver on macOS today through the hidraw path, which
is why those are on CodeWeavers' allowlist (RESEARCH.md B12). The T150
cannot ride that path. On the SDL path it arrives without a PID collection
too: the SDL backend synthesises one only for a device SDL calls haptic,
and SDL's macOS haptic backend is `ForceFeedback.framework`, which reaches
only devices whose driver published a plug-in. No wheel vendor ships one.

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
| `probe_hid`, `probe_ep0` | working, macOS only |
| `probe_setreport`, the HID writer | working, macOS only, no root. **The wheel obeys it** |
| `probe_intr`, the interrupt OUT writer and pipe reader | working, macOS only, needs root. The wheel obeys it too |
| `include/t150/*.h` shared contracts | written, compiled and tested on Linux |
| `src/lib/encode.c` wire encoders | written, golden-vector tested on Linux |
| `src/lib/proto.c` DLL to daemon protocol | written, round-trip tested on Linux |
| `t150d` protocol, slots, downgrades, watchdog | written and tested on Linux |
| `src/t150d/wirequeue.c` the writer's coalescing queue | written and tested on Linux; the threading around it is macOS only |
| `t150d` macOS HID backend | working: opened a real wheel unprivileged, rendered a constant force and a damper that were felt on the wheel, and its shutdown stopped a runaway effect |
| `t150-dinput8.dll` the in-bottle proxy | loads and chain-loads in a real bottle, and a game creates effects through it, test 23b |
| `probe_dinput.exe` the in-bottle probe | run on hardware in test 27: it mapped the wheel, walked all twenty one controls with a person answering, and played two effects that were felt. Also runs under Wine in CI against a wheel shaped uinput device |
| build, CI, docs, man pages | working |
| `t150ctl`, `t150boot` | working on hardware: `t150boot` switched a wheel, `t150ctl` talked to one. The range change has run but nobody felt it yet |

The encoders turn a normalized effect into the wheel's packets and are the
only code that knows both DirectInput units and wheel units. They do no I/O,
so `make test` checks every byte they produce against vectors derived from
`docs/PROTOCOL.md`, on any machine.

The daemon is complete except for the part that touches a wheel. It listens,
speaks the protocol, keeps the slot table, downgrades the effects the wheel
cannot render, slides ramps, and runs the watchdog; the packets go to a log
rather than to hardware. A frame sets what a slot should hold and a
rate-limited pass puts on the wheel whatever actually changed, so a game
re-sending an effect it has not touched costs a comparison rather than three
writes. That is enough to drive the whole stack from a test
without a Mac, which is what `socket_check` does, including holding a socket
open and going quiet to prove the wheel gets released.

The proxy is written and cross builds to an x86_64 PE with the right five
exports and no import of `dinput8` to recurse into. It has now executed on
hardware: it wrapped the wheel, claimed force feedback and carried two
effects to the daemon that were felt (A43). There is no Wine on the
development machine and no Mac here, so
the checks that exist run in CI on Windows: they unit test the DirectInput
conversion, then load the DLL with a copy of the system `dinput8` beside it
and confirm both entry points chain-load. Whether a Wine bottle resolves the
same way is the first thing M5 has to try.

The wheel agrees with the settings bytes and with force feedback, on both
pipes. [`docs/PROBES.md`](docs/PROBES.md) is the procedure that established
it and the thing to rerun after any change to the encoders.

## What needs doing next

**The wheel is answered and the software is not.** Settings and force
feedback both work through an unprivileged `IOHIDDeviceSetReport`, with
CrossOver keeping the wheel throughout, so there is no ownership conflict and
nothing here needs root. What has never happened is a game reaching the wheel
through this project's own code.

What is left needs the Mac. In order:

**0. Let the wheel find its centre again.** Driving it into its end stops
shifts its idea of straight ahead, which test 13 watched happen (A32), and
everything below is read through that: a symmetric force about a displaced
centre looks asymmetric. Unplug it from mains and USB, let it sweep, and
start from there, and again after any run that worked against a stop.

**1. The game and the daemon at the same time.** Everything else has now
happened separately: the wheel is back in the bottle
(`SDL_JOYSTICK_HIDAPI=0`, A35), the proxy loads inside Assetto Corsa,
chain-loads the builtin and logs what it does, and the daemon drives the
wheel. Test 16 missed the join on ordering, the daemon must start first
and stay running, and test 17 missed it on one wrong path, the proxy
guessing the daemon's home from a bottle username that is always
`crossover` (A36), now fixed. Nothing measured blocks the join any more.

**2. Leave the pedals alone unless a game forces the issue.** Test 19
measured the mislabel, the brake on the first pedal axis and the
accelerator on the second where games expect the opposite (A37), which
DirectInput publishes as Y and Z (A43). Correcting it by default regressed
a working setup twice: Assetto Corsa had bound the raw layout correctly by
detection, and both the swap and the mirror re-crossed it (A39, A41). The
descriptor's own usage names are not where DirectInput puts those axes,
and reading them as if they were is what aimed the correction at an axis
this wheel does not have (A44). The proxy forwards
input untouched now, with `T150_PEDALS` there for a game that assumes the
convention and cannot rebind. The buttons are already whole on the
`Hidraw` route, so keep that route on, and the durable fix to offer
CodeWeavers is the allowlist line B12 describes.

**3. Play the effects nobody has played yet.** A constant, a damper and two
periodics have moved the wheel. Springs, envelopes, ramps and per-effect
gain have never touched hardware, and every one of them is arithmetic this
project derived from a driver rather than measured. `probe_setreport -x`
plays any of them in a line, and the encoders' own golden vectors say what
the bytes should be. The one waveform question left is telling `0x4020`
from `0x4021` back to back; `0x4025` is settled, it renders nothing.

Then, whichever way those went:

**4. Package what is written.** `t150boot` wants to be a user LaunchAgent
matching the boot product id, because sleep, wake and every replug drop the
wheel back, and `t150d` wants to be another so a game never has to be told to
start it. Both run unprivileged, so neither needs an admin prompt at install.
That is the last thing between this and something a person could just use.

**5. Robustness, then a real game.** Reconnect on both ends, hot plug, and
the watchdog under real crash conditions. Measure the latency and jitter of
the whole path under Rosetta while you are there. The daemon now caps itself
at one emission every 4 ms and sends only what changed, so what is still
unmeasured is narrower than it was: whether this wheel sustains that rate,
and whether a driver can feel the difference between that cap and a faster
one.

Later, and not blocking: an ARM64EC build for CrossOver 27's bottles, an
installer that does the bottle setup in one step, and optionally an SCS
telemetry plugin, which is the only way any native macOS game can be reached.

The gate said yes for settings, on both pipes. Force feedback has not
followed, and if it turns out not to, the ladder of fallbacks is in
[`docs/HANDOFF.md`](docs/HANDOFF.md) section 8, and the honest last rung is
to say so and stop.

## Using a release

Prebuilt binaries are on the
[releases page](https://github.com/renaudallard/crossover-wheel/releases),
built by CI from the tagged commit. Two archives:

| Archive | Contains |
| --- | --- |
| `crossover-wheel-<v>-macos-arm64.tar.gz` | `t150ctl`, `t150boot`, `t150d`, and the four `probe_*` tools |
| `crossover-wheel-<v>-windows-x86_64.zip` | `t150-dinput8.dll`, the in-bottle proxy |

Each archive carries a short README; those are `dist/README.macos` and
`dist/README.windows` in this repository, packaged verbatim at release time
so they cannot drift from what is written here.

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

CI also runs the in-bottle probe rather than only compiling it. It runs on
`windows-latest` against a real loader, and on `ubuntu-latest` under Wine,
which is what a CrossOver bottle is. The Wine job creates a uinput device
carrying the wheel's own USB ids, `tests/fake_wheel.c`, so the probe has
something to enumerate: without one it stops at "no T150 found" and the half
of it that has actually been wrong never runs. With the device present it
enumerates every object, maps each one into the state struct and walks all
twenty one controls, and the job fails if a control does not land where the
data format puts it. That check exists because the probe first shipped
reading buttons at their enumeration offset, which is a different number and
can be negative.

## Setting the wheel up

Two small tools, both macOS only and neither needing root.

**`t150boot`** takes the wheel out of boot mode, which is where it starts and
where sleep, wake and every replug put it back. Nothing else works until it
has run:

```sh
./build/bin/t150boot
attachment 0x06, model 0x03  T150
switched, the wheel is at 0xb677
```

It exits 0 when there is nothing at the boot id too, because that is the
ordinary state once a wheel has been switched, so it is safe to run on every
plug-in from a LaunchAgent.

**`t150ctl`** sets what the wheel keeps for itself, and does not take it away
from a running game:

```sh
./build/bin/t150ctl range 270          # three quarters of a turn lock to lock
./build/bin/t150ctl autocenter 0       # let go of the wheel completely
./build/bin/t150ctl status
```

Autocenter is a progressive spring rather than a constant pull, so a low
setting stiffens the wheel without recentring it. Only `autocenter 0` releases
it; see [`t150ctl(1)`](man/t150ctl.1) for why the protocol's separate enable
flag releases nothing.

## Running the daemon

`make` builds `build/bin/t150d`. **On macOS it drives the wheel**, through
`IOHIDDeviceSetReport` with a non-seizing open, so CrossOver keeps reading it
throughout:

```sh
./build/bin/t150d -v
t150d: wheel 044f:b677 open
t150d: listening on 127.0.0.1:49713, endpoint .../t150ffb/endpoint
t150d: backend macOS HID
```

It does not need root and it does not need the wheel to be plugged in when it
starts: if the wheel is absent, or still at the boot product id, it keeps
looking and picks one up when it appears.

`-n` drives nothing and prints every packet instead, in the same form
`probe_setreport` prints, so the two can be compared directly. That is the
only behaviour anywhere but macOS.

**A wheel unplugged mid game comes back on its own.** It returns at the boot
product id, which is where sleep, wake and every replug leave it, so the
daemon switches it the same way `t150boot` does and picks it up on the next
scan. Nothing has to be run by hand any more, and nothing needs a password.
Any model but the T150 is refused rather than sent the T150's switch value;
use `t150boot -V` for those.

**Restarting the daemon under a running game recovers by itself.** The proxy
reconnects on its next force feedback call, at most once a second, and every
effect re-uploads and re-starts itself on the first call after that. Before
this, a restarted `t150d` cost the game its force feedback until the game was
restarted too, because nothing reachable from a device the game already holds
ever reconnected.

**A client taking the wheel sets its device gain to full.** The wheel keeps a
gain of its own, nothing here ever set it, and so every force was scaled by
whatever the wheel powered up with or whatever the last process left behind.
Full means do not attenuate, which is also DirectInput's own default, so the
strength ends up where the game's settings put it. A game that wants less
says so with `DIPROP_FFGAIN`, and `t150ctl gain` still overrides it by hand.

**The rotation range is `-r`, because no game can ask for it.** DirectInput
has no property for wheel rotation: on Windows it is set in the vendor's
control panel and the game assumes the wheel is already there, so a game
whose settings say 900 degrees reaches nothing and scales its steering for a
range the wheel is not at.

**Do not use `-r` with a game.** That reasoning was backwards and hardware
said so: a game sends the maximum range and scales its own steering for it, so
a wheel quietly set to 900 underneath one steers differently from what the
game believes, which is worse than leaving it alone. `t150ctl range 900` by
hand, deliberately, is where this belongs. It is the same mistake
[`docs/RESEARCH.md`](docs/RESEARCH.md) A41 records about correcting pedals
unasked.

**An upload is answered when it is accepted, not when it reaches the wheel.**
The daemon keeps what each slot should hold and writes it out from a pass
that runs at most once every 4 ms, so a game updating a force faster than
that has the superseded values dropped rather than queued, and a game
re-sending an effect it has not changed writes nothing at all. Only effect
parameters are treated this way. Starts, stops, resets, the settings and
every path that makes the wheel safe are written the moment they arrive and
are never merged, because those are things that happen rather than values
that hold. A write that fails is reported on the next upload, which is the
only frame that can carry it: an error answered to a keepalive would make the
proxy drop its connection, and nothing reconnects.

One consequence is visible in `-n`: a steady force prints three lines and
then nothing, where it used to print three per update.

**`-w` writes to the wheel from a thread of its own, and that is what the CPU
warning was.** Without it every packet goes out on the thread that also
answers the proxy, so while the daemon sits inside a synchronous
`IOHIDDeviceSetReport` the game is blocked waiting for a reply, on its own
main thread. The tester's overlay showed that thread swinging to 96% while
the physics thread sat flat at 20%, and with `-w` his `CPU OCCUPANCY` warning
is gone.

**The writer's queue coalesces, because the wheel is slower than the
daemon.** The emitter flushes up to four dirty slots every 4 ms and a
constant costs two packets, since Thrustmaster's own driver pairs each update
with a control, so a game holding three effects asks for roughly 1250 packets
a second. The fastest that same driver ever puts two packets on the wire, in
`tmp/oldffb/directX_constforce.pcapng`, is 1.344 ms apart: about 740 a second.
A queue that merely stored the difference spent the session full and delivered
every force a fifth of a second late, which the tester felt as a wheel with no
resistance to a quick turn. So a packet still waiting when a newer one arrives
for the same parameter is replaced by it, where it stands. The wheel holds one
value per parameter and the one it never rendered could not be felt; order is
untouched, and a play and a stop for one slot are different parameters that
never merge. `-v` prints the merged and dropped counts on the way out, and
after coalescing a drop means the wheel stopped taking writes altogether.

That queue is `src/t150d/wirequeue.c`, kept apart from the macOS backend that
uses it so `tests/wirequeue_check.c` can drive every rule in it on a machine
with no wheel and no Mac.

**`-v` says what the game asks for, once a second.** Nothing in this project
recorded that for its whole life; only the bytes going out. A constant gives
its magnitude, direction and gain, and a condition gives its centre,
coefficients, saturations and deadband:

```
t150d: slot 2 constant: magnitude -8670, direction 9000, gain 10000
t150d: slot 3 damper: centre 0, coeff 9998/9998, saturation 10000/10000, deadband 0
```

A condition is named as the **spring** or **damper** it is rather than as a
condition, because the two are different forces and this line could not tell
them apart. Those are Assetto Corsa's real numbers: a damper at 9998 of
10000, which is the maximum, and the wheel is unstable there. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A46. The kind shown is what the wheel
was given, after any downgrade, which is reported on its own line. The
autocentre release is logged the same way the gain and the range are, so a
report can tell a packet that was refused from one that was never sent.

A slot that starts or stops says so too, **once per transition rather than
once per call**, because a game may start an already playing slot on every
frame and Assetto Corsa does. What that answers is whether a slot was ever
started at all: an effect uploaded and never started renders nothing, and from
the daemon's side that looks exactly like one the wheel ignores.

**A condition's coefficient is held at 80% of what the wheel takes, and this
is the one place the daemon knowingly gives a game less than it asked for.**
The T150's own condition loop is unstable at the top of its range. Measured by
hand with no game, no daemon and no proxy: a damper at 100 makes the wheel buzz
wherever it is left standing, 99 still buzzes, 90 buzzes more slowly, 80 is
quiet. An oscillation that slows as the gain falls is the wheel fighting
itself, not a resonance.

The encoding is not what is wrong; Thrustmaster's own divisor reaches 100 for
the same request, so the wheel dislikes a value that is faithfully encoded.
What justifies overriding a game here is that nobody can work around it
themselves: Assetto Corsa asks for 9998 of 10000 and its force feedback page
has no damping control at all. The coefficient is **clamped rather than
rescaled**, so a game asking for half strength still gets half strength and
only the top of the range is flattened. The tester's verdict on the difference
at full request: *"about the same, maybe a little less hard"*. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A46.

**A client connecting opens the wheel's input and disconnecting closes it.**
The firmware renders no effect while no input is open, which is what
[`docs/RESEARCH.md`](docs/RESEARCH.md) A28 established, so the daemon sends
`42 04` on hello and `42 00` when the client goes. The open also outlives the
process that sent it (A30), so the daemon stops every slot, releases the
autocenter and closes the input the moment it takes the wheel: one inherited
from a crashed daemon, or from a probe that never cleaned up, arrives still
rendering its last effect and goes quiet at startup rather than at shutdown.
The autocenter release is part of that, because closing the input re-arms the
wheel's own centring spring and would otherwise leave it stiff (A42).

See [`t150d(8)`](man/t150d.8) for the endpoint file, the watchdog and the
effect downgrades.

## Installing the proxy into a bottle

`make dll` cross builds it, and needs `gcc-mingw-w64-x86-64`. It has to go in
the bottle's `system32` rather than beside a game, because SDL reaches
DirectInput through `CoCreateInstance`, which the loader resolves to an
absolute `system32` path and never to a game directory.

Written against CrossOver 26.3.0, which is Wine 11. The paths and the loader
behaviour below were read out of that release rather than remembered.

**It serves 64-bit games only.** The proxy is an x86_64 PE, and a 32-bit
process has its `system32` redirected to `syswow64`, where this is not, and
Wine skips a file whose machine does not match in any case. `file "<game>.exe"`
says which one you have. There is no i386 build.

Two files and one registry value:

```sh
CX_ROOT="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
SYS32="$HOME/Library/Application Support/CrossOver/Bottles/<name>/drive_c/windows/system32"

# the real implementation, under the name the proxy chain-loads
cp "$CX_ROOT/lib/wine/x86_64-windows/dinput8.dll" "$SYS32/dinput8_orig.dll"

# the proxy, under the name the game and SDL ask for
cp build/bin/t150-dinput8.dll "$SYS32/dinput8.dll"

# the override, once, in the bottle's registry
"$CX_ROOT/bin/wine" --bottle "<name>" --cx-app reg.exe add \
    'HKCU\Software\Wine\DllOverrides' /v dinput8 /t REG_SZ /d native,builtin /f
```

From a release archive, use `t150-dinput8.dll` from the extracted directory
in place of `build/bin/t150-dinput8.dll`. A bottle can move the builtin
directory with `DllPath` in its `cxbottle.conf`, and
`find "$CX_ROOT" -name dinput8.dll` says where it went.

**Copy CrossOver's builtin, and only that, as `dinput8_orig.dll`.** The
`dinput8.dll` already in `system32` is a placeholder carrying no
implementation, and Wine will not fall back to one; copying the proxy twice
is the other measured mistake, and it fails later and more confusingly.
Check what you copied:

```sh
head -c 128 "$SYS32/dinput8_orig.dll" | strings | head -1
```

`Wine builtin DLL` is right; `Wine placeholder DLL` or a fragment of
`This program cannot be run in DOS mode` is the mistake. It has to be
`-c 128`: the signature starts at byte 64, so `-c 64` shows nothing on any
file, right or wrong.

**Override `dinput8` and nothing else.** The copy still carries the builtin
signature, and Wine refuses a builtin file whose load order says native only,
so an override for `dinput8_orig` breaks the chain-load that no entry at all
resolves correctly.

**For one run rather than for good**, pass `--dll dinput8=n,b` instead of
touching the registry. Exporting `WINEDLLOVERRIDES` does nothing: CrossOver's
wine wrapper deletes it from the environment and honours only `--dll`.

**The proxy's environment.** `T150_DEBUG=1` makes it say what it is doing
on stderr, and `T150_ENDPOINT` points it at the daemon's endpoint file.
Unset, the proxy finds the file itself: it tries
`Z:\Users\<USERNAME>\Library\Application Support\t150ffb\endpoint` and then
every home under `Z:\Users`, because a CrossOver bottle's Windows user is
named `crossover` whoever owns the Mac, which is exactly how test 17's
proxy missed a running daemon (A36). A proxy from release 0.1.3 or older
only makes the first guess, so with one of those, set `T150_ENDPOINT`
explicitly.

Stderr goes nowhere when Steam relaunches the game, which test 16 showed is
where the interesting lines vanish. `T150_LOG` names a file the proxy
appends the same lines to, from every process, so a Steam-launched game
leaves a trace:

```ini
[EnvironmentVariables]
"SDL_JOYSTICK_HIDAPI" = "0"
"T150_DEBUG" = "1"
"T150_LOG" = "Z:\\Users\\<you>\\Library\\Application Support\\t150ffb\\proxy.log"
```

That is the bottle's `cxbottle.conf`, which every launch reads, including
from the CrossOver window; from a terminal, `--env` sets the same things for
one run. The `SDL_JOYSTICK_HIDAPI` line is what puts the wheel in the
bottle at all, see the input path section. The proxy's `OutputDebugString`
lines also surface in a terminal run with `--debugmsg +debugstr`.

`T150_PEDALS` turns on the pedal corrections, and the default is none of
them: input is forwarded untouched. The T150 labels its pedals against the
common convention and rests them at maximum, but a game that binds pedals
by asking you to press them resolves both by itself, and correcting them
underneath such a game re-crosses what it got right. Set `swap` for the
labels, `invert` for the rest position or `full` for both, in a game that
assumes the convention and cannot rebind. The proxy logs its version and
these settings when it wraps the wheel.

**Check the install without a game:**

```sh
"$CX_ROOT/bin/wine" --bottle "<name>" --debugmsg +loaddll --env "T150_DEBUG=1" \
    --cx-app regsvr32.exe dinput8.dll
```

`regsvr32` calls `DllRegisterServer`, which the proxy forwards, so this loads
the whole chain and nothing else. Two lines say it worked: `dinput8.dll` as
`native`, which is the proxy, and `dinput8_orig.dll` as `builtin`, which is
the implementation behind it, with the builtin's own imports (`hid.dll`,
`setupapi.dll`, `comctl32.dll`) loading between the two. That is exactly
what a passing run printed on real hardware in test 14.

The failure signature is `dinput8_orig.dll` loading as `native`: the tag
comes from the file's own bytes, so `native` there means the wrong file is
beside the proxy, whatever the file listing claims. And a
`Failed to register` from regsvr32 with both lines present would be a
registration problem, not a chain-load one; without the `dinput8_orig.dll`
line it is the chain-load, and `T150_DEBUG=1` names it on stderr.

Exporting `WINEDEBUG` does nothing either. The wrapper sets `WINEDEBUG=-all`
unless it is given `--debugmsg` or finds `CX_DEBUGMSG` in the environment, so
a silent run is the wrapper and not the loader.

**A CrossOver upgrade leaves the proxy in place**, because Wine only
overwrites `system32` files that are still placeholders. `dinput8_orig.dll`
stays at the Wine version it was copied from, so copy it again after an
upgrade.

None of this has been tried in a real bottle yet. If the chain-load does not
resolve, the fallbacks are in [`docs/HANDOFF.md`](docs/HANDOFF.md) under M4.

## Testing it today

Every step, in order, for the three things that can be tested right now.
[`docs/PROBES.md`](docs/PROBES.md) is the authority on what each outcome
means; this is the sequence to type.

All of them need the Mac. Four things first, each of which otherwise costs a
whole session:

- **Watch it calibrate.** A healthy T150 sweeps counterclockwise, then
  clockwise, then back to centre as soon as it has mains power. If it does
  not, Thrustmaster attributes that to power: wall outlet directly rather
  than a strip, USB into the machine rather than a hub.
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
anything else, not after: until it succeeds the wheel is not the device this
project drives.

```sh
./build/bin/t150boot
```

That is the shipped tool and it is what a user should run. No `sudo`: the two
endpoint 0 transfers were measured working as an ordinary user with the
device unopened. It waits for the wheel to come back at `044f:b677` and only
then says it worked, because the switch transfer's own result cannot tell
you: the wheel leaves the bus before the transfer completes, so success and
half a dozen different failures all look the same.

```
attachment 0x06, model 0x03  T150
switched, the wheel is at 0xb677
```

It exits 0 when nothing is at the boot id too, so it is safe to run on every
plug-in. It refuses a wheel whose model byte is not the T150's rather than
sending it another model's switch value; pass `-V` with the right one from
the Linux driver's table if you know it.

**If that fails, the probe is the fallback**, and it is a different route:

```sh
sudo ./build/bin/probe_intr -I
./build/bin/probe_hid -o .
```

`probe_intr -I` also sends the five initialisation packets the Linux driver
puts on the interrupt OUT pipe first, which needs the device captured and so
needs root. `t150boot` deliberately does not. Whether those packets matter is
**unsettled**: they were adopted to explain a wheel that came back apparently
blocked, and that turned out to be the autocenter holding at full strength
rather than anything about the switch. If a wheel misbehaves after
`t150boot`, try `t150ctl autocenter 0` before reaching for `-I`.

The `-o .` dump from `probe_hid` is worth keeping either way: firmware mode
reports `MaxOutputReportSize` 15 where the protocol document expects a
14-byte report with id `0x0A`, and 15 is exactly the length of the one packet
that never fitted.

**A2b. Check the settings tool while you are here.** It needs no privilege
and does not take the wheel from anything:

```sh
./build/bin/t150ctl status
./build/bin/t150ctl range 270
./build/bin/t150ctl range 1080
./build/bin/t150ctl autocenter 0
```

`range 270` should make lock to lock obviously short and `1080` put it back;
`autocenter 0` should leave the wheel free to turn. If those work, the whole
settings path works, and A3 below is the same thing done by hand.

**A3. Write through the HID layer**, which is the path the project needs and
which needs no root. Turn the wheel by hand first, before sending anything,
so you have a baseline. A freshly plugged wheel holds a full autocenter and
feels locked; that is normal, not a fault.

```sh
./build/bin/probe_setreport -a 0
```

That sets the autocenter force to zero. **Not `-A`**: the enable flag only
says whether the autocenter survives a process opening the wheel's input, and
the effect is active whenever nothing has it open, which on macOS is always.
Only the force releases it. Then put it back and take it off again, because
the change is what proves the point:

```sh
./build/bin/probe_setreport
./build/bin/probe_setreport -a 0
```

Expect free, then hard to turn, then free. That is what was measured, and it
answers the whole gate: the firmware obeys an unprivileged
`IOHIDDeviceSetReport` and CrossOver keeps the wheel throughout.

**A success return is not the answer.** macOS can accept a report the
firmware then discards. What settles it is whether the wheel physically
moved.

**A4. The same bytes on the interrupt OUT pipe**, as the cross-check. This
needs root, because it captures the wheel from macOS, writes, and hands it
straight back:

```sh
sudo ./build/bin/probe_intr -a 0
```

This works too, so the two are not alternatives and the interrupt route is a
fallback rather than the plan. It can configure a wheel but cannot drive
effects during a game: holding that pipe means owning the device, and
CrossOver cannot read a wheel this tool owns.

**A5. Only if A3 returned success and nothing moved.** It did move on the
wheel measured here, so this is for a wheel that behaves differently. Work
the framings before concluding anything:

```sh
./build/bin/probe_setreport -i 0x0a
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -P
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -i 0x0a -P
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -n 1
./build/bin/probe_setreport -r 270
./build/bin/probe_setreport -r 1080
```

**The `-a 0` between each one is not optional.** Every variant here is the
autocenter action, setting the spring to maximum and enabling it. Without
releasing it in between, a wheel that obeyed the first command stays rigid
for the whole run and looks identical to one that obeyed nothing. Turn the
wheel by hand after each pair and watch for a *change*, not for stiffness.

Those are, in order: the declared output report id, zero-padding to the
declared size, both together, the other HID node if there is one, and then
the rotation range instead of the spring, since a firmware that silently
drops one opcode may accept another. `-r 270` should make lock to lock
obviously short and `-r 1080` put it back.

Watch for `no HID node matches`. `probe_setreport` defaults to the T150's
firmware product id, so against any other wheel, or against one still in boot
mode, every one of these does nothing until `-p` names the id `probe_hid`
actually reported.

**A6. Prove the force feedback protocol.** Two runs have tried and neither
could have worked, so this is a fresh start rather than a repeat. Run both
forms below.

**On the HID path**, which A19 showed reaches the firmware. `42 04` opens the
wheel's input, without which nothing renders; the next packet clears the
autocenter so nothing fights the effect; the third sets the gain, because
nothing has established what the wheel powers up with:

```sh
./build/bin/probe_setreport \
    -x "42 04" \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

**Hold the wheel or keep a hand on the plug**, then stop it and close the
input, because the open outlives the tool and an unstopped effect renders
until something sends `42 00` (A30):

```sh
./build/bin/probe_setreport -x "41 00 00 01" -x "42 05" -x "42 05" -x "42 00"
```

**On the interrupt OUT pipe, holding the wheel and hands off.** `-H` keeps
the session open for fifteen seconds and reads the IN pipe while it waits, so
the wheel's own reports show it moving, and `-N 32` pads every packet to the
endpoint size, which is what Akellacom's working T300RS driver does. **Take
your hands off the wheel while it runs.** An idle T150 emits about four
reports a second and never changes them, so anything that moves is the wheel
moving itself:

```sh
sudo ./build/bin/probe_intr -N 32 -H 15 \
    -x "42 04" \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

**Two corrections are already folded into that.** `42 04` opens the wheel's
input, which no run before this ever sent, and `ff_first` is nine bytes rather
than the eleven this project used to send: Thrustmaster's own driver ends it
at `fade_level` and only a condition carries the two extra bytes. Between them
those were the two concrete reasons force feedback had never worked. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A26 and A27.

The interrupt run prints the wheel's own reports for those fifteen seconds
and then hands the wheel back. `nothing ever changed` at the end means the
effect did nothing; a run of changing steering bytes with your hands off is
the answer we are looking for.

**Handing the wheel back does not stop the effect.** The open and the effect
both outlive the tool, so end every session with the stop and close line
above, and if the effect spent time working against an end stop, unplug the
wheel and plug it back so it re-finds its centre. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A30 and A32.

**A7. Three cheap answers while you are set up.** Compare `03 0e 00 40`
against `03 0e 00 7f` in A6: if they feel the same, a constant really does
stop at `0x40`. Try `20 40` and `21 40` in place of `00 40` in the commit
packet, which would mean square and triangle exist after all. And repeat A3
once with a game running in a bottle, because macOS 26 fails `setReport` for
every client the moment anything seizes the device.

**A8. Where the buttons go, if you are chasing that.** Already answered
against the wheel: it puts all thirteen on the wire and CrossOver registers
none of them. Rerun it only to reproduce that, or on a different wheel:

```sh
sudo ./build/bin/probe_intr -R 15
```

That reads the interrupt IN pipe with the device captured, so nothing sits
between the wheel and the output. Only reports that differ from the one
before are printed. **Work every button, the hat and the pedals while it
runs**, and turn the wheel a little so you can tell the stream is live.

It finishes with a mask of every bit that moved at any point, which is the
line to read if an analogue axis jitters at rest and floods the output. A
byte reading `00` in that mask never changed, whatever you pressed.

A line for each press is what happened, so the wheel is fine and whatever
loses them is above the USB layer, in macOS, SDL or winebus. No line for any
press, on a stream that is otherwise changing, would mean the wheel.

### Test C: does the daemon reach the wheel

New, and the shortest of the three. `t150d` now writes to the wheel itself
rather than to a log.

**C1. Put the wheel in firmware mode**, as in A2, then start the daemon:

```sh
./build/bin/t150d -v
```

Expect three lines, in this order:

```
t150d: wheel 044f:b677 open
t150d: listening on 127.0.0.1:<port>, endpoint .../t150ffb/endpoint
t150d: backend macOS HID
```

**No `sudo`.** If it asks for one, something is wrong: this path needs no
privilege at all. `cannot open the wheel: 0x...` with `something has seized
it` means another process took the wheel exclusively; nothing in this project
does that.

If the wheel is not plugged in, or is still at the boot id, the daemon says
so and keeps looking rather than exiting. Plug it in, or run `probe_intr -I`,
and it picks it up within half a second.

**C2. Check CrossOver still sees the wheel** with the daemon running. It
should, because the daemon opens the device without seizing it, and that is
the single assumption the whole design rests on. If the wheel disappears from
CrossOver's controller panel the moment the daemon starts, stop and say so.

**C3. Nothing moves yet, and that is correct.** The daemon opens the wheel's
input only when a client says hello, so with no game connected it sends
nothing. Test B is what connects one.

### Test B: does the proxy load in a bottle

Independent of test A, and worth running even if A fails. Needs CrossOver and
any DirectInput 8 or SDL game with force feedback settings.

**Run it twice.** First with `-n`, which drives nothing and prints every
packet, so a fault in the proxy cannot be confused with a fault at the wheel.
Then without, which is the whole path end to end and where the wheel should
actually move.

**B1. Install the proxy** as [above](#installing-the-proxy-into-a-bottle).

**B2. Check the chain-load with no game and no daemon**, which separates a
broken install from a broken game:

```sh
"$CX_ROOT/bin/wine" --bottle "<name>" --debugmsg +loaddll --env "T150_DEBUG=1" \
    --cx-app regsvr32.exe dinput8.dll
```

`dinput8.dll` as `native` and `dinput8_orig.dll` as `builtin` in the output
means the proxy loads and reaches the implementation behind it. Test 14 ran
this against a real bottle and it passed. `dinput8_orig.dll` as `native`
means the wrong file is beside the proxy; go back to B1. Nothing else below
can work until this passes.

**B3. Start the daemon** and leave it running where you can see it:

```sh
./build/bin/t150d -n -v          # first pass: log only
./build/bin/t150d -v             # second pass: drive the wheel
```

**Hold the wheel or keep a hand on the plug during the second pass.** A game
that asks for a strong constant force will get one.

**B4. Start the game** from a terminal so the loader talks. The wrapper sets
`WINEDEBUG=-all` unless it is told otherwise, so the tracing has to go through
its own options rather than through exported variables:

```sh
"$CX_ROOT/bin/wine" --bottle "<name>" --debugmsg +loaddll --env "T150_DEBUG=1" \
    --cx-app "<game>.exe"
```

**B5. Watch for four things, in order.** Each one that appears rules out a
whole class of failure:

1. `dinput8_orig.dll` loaded as `builtin` in the `+loaddll` output. If it
   says `native`, or does not appear, the chain-load did not resolve and the
   fallbacks are in [`docs/HANDOFF.md`](docs/HANDOFF.md) under M4.
2. `t150-dinput8: connected to the daemon on port ...` from `T150_DEBUG`. If
   this is missing, the proxy says which path it tried; pass the right one
   with `--env "T150_ENDPOINT=Z:\..."`.
3. `t150-dinput8: wrapped the wheel`. If this is missing, the game's device
   is not being recognised as the T150, or the game never created it.
4. `write ...` lines in the daemon's output as the game's force feedback
   starts. Those are the packets a wheel would have received.

Getting to 4 with `t150d -n` means every layer works as far as the log.
Getting to 4 with the daemon on its real backend means **the wheel should
move**, and that is the end to end path this project was built for.

**It has moved.** Test 27 played a constant force and a damper through the
whole chain with no game involved, and both were felt on the wheel
(`docs/RESEARCH.md` A43). What has still not been run is this test, the
same path with a game at the top of it.

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

**A game that does not load `system32`'s `dinput8.dll` is out of reach, and an
absent proxy log is how you find out.** With `T150_LOG` set, a game that never
writes a line never loaded the proxy, and no amount of daemon work will reach
it. That test costs one launch and is worth running before investigating any
game's force feedback.

Dakar Desert Rally produced no proxy log, has no force feedback and no
autocentre, and drives every button and axis through CrossOver's ordinary
input path. What that does **not** establish is why. It was launched through
the Heroic Game Launcher rather than by the bottle directly, so the process
may simply have looked for `dinput8.dll` somewhere the proxy was not
installed, and the game's own store page claims the T150 is supported. An
absent log says the proxy was not loaded; it does not say the game cannot use
it. Settling that means installing the proxy where that launcher's prefix
looks, and nobody has.

Native macOS games are close to unreachable: no public API drives an
arbitrary HID wheel, and library validation blocks injecting into signed
games. The one exception is Euro Truck Simulator 2 and American Truck
Simulator, which load third-party telemetry plugins, and even there the game
sends no forces, so any plugin has to invent them from telemetry.

The T150 renders constant force, square, triangle, sine, both sawtooths,
spring and damper in hardware, all eight named by Thrustmaster's own Windows
driver. Friction, inertia and ramp are not in its protocol. Those are
downgraded rather than refused, because a game that gets a refusal from
`CreateEffect` may disable force feedback altogether.

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
