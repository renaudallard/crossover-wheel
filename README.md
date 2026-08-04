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
> **What is not finished.** Everything between a game and the wheel is
> written, and none of it has been run end to end: the daemon's macOS
> backend, `t150ctl`, `t150boot` and the in-bottle proxy all compile and none
> has touched hardware. Only a constant force and one periodic have ever been
> played, so springs, dampers, envelopes and per-effect gain are still
> arithmetic derived from a Linux driver rather than measured. And CrossOver
> registers none of the wheel's buttons, which is a separate input-path
> problem (A21, A25).

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
the bottle, so the steering and pedals reach games today.

**Does not work: the buttons.** Measured, and not the wheel's fault. It puts
all thirteen buttons and the hat on the wire, and CrossOver's controller
panel registers none of them in either its DirectInput or its XInput view.
Something between macOS HID and winebus drops them. This is a separate
problem from the one the project was built for, and it is not yet diagnosed.

**Does not work: force feedback.** Wine's DirectInput sets
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
| `probe_hid`, `probe_ep0` | working, macOS only |
| `probe_setreport`, the HID writer | working, macOS only, no root. **The wheel obeys it** |
| `probe_intr`, the interrupt OUT writer and pipe reader | working, macOS only, needs root. The wheel obeys it too |
| `include/t150/*.h` shared contracts | written, compiled and tested on Linux |
| `src/lib/encode.c` wire encoders | written, golden-vector tested on Linux |
| `src/lib/proto.c` DLL to daemon protocol | written, round-trip tested on Linux |
| `t150d` protocol, slots, downgrades, watchdog | written and tested on Linux |
| `t150d` macOS HID backend | written, compiles on macOS, never yet driven a wheel |
| `t150-dinput8.dll` the in-bottle proxy | written, cross builds, never yet run |
| build, CI, docs, man pages | working |
| `t150ctl`, `t150boot` | written, macOS only, never yet run against a wheel |

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

**0. Let the wheel find its centre again.** Its end stops have been measured
about ten degrees apart, roughly 170 degrees one way and 190 the other, after
many capture and release cycles. Everything below is read through that, and a
symmetric force about a displaced centre looks asymmetric. Unplug it from
mains and USB, let it sweep, and start from there.

**1. Play the effects nobody has played yet.** A constant and one periodic
have moved the wheel. Springs, dampers, envelopes, ramps and per-effect gain
have never touched hardware, and every one of them is arithmetic this project
derived from a driver rather than measured. `probe_setreport -x` plays any of
them in a line, and the encoders' own golden vectors say what the bytes
should be.

Settle which waveform `0x4020` is while you are there. It oscillates, so it
is real, and PROTOCOL.md still calls it a guess; comparing it against
`0x4021` by feel says whether square and triangle can stop being downgrades.

**2. Where CrossOver loses the buttons.** Answered as far as the wheel is
concerned: `probe_intr -R` proved all thirteen buttons reach the wire, and
the mask it printed matches the report descriptor field for field. CrossOver
registers none of them, in either its DirectInput or its XInput view, so the
loss is in macOS HID, SDL or winebus. That makes it an input problem rather
than a force feedback one, and `docs/RESEARCH.md` B8 is where to start.

One thing to pin down first: the latest report says the **axes** stop working
too, where earlier ones had them working. Replug the wheel, switch it with
`probe_intr -I`, touch nothing else, and open the controller panel, so that
whatever is measured is measured on a wheel that has not been captured and
released a dozen times.

**3. Try the proxy in a bottle**, step by step under
[testing it today](#testing-it-today). The DLL has never run under Wine, and
finding out whether it loads and whether a game's effects reach the daemon
needs no working force feedback at all.

Then, whichever way those went:

**4. Package what is written.** `t150boot` wants to be a user LaunchAgent
matching the boot product id, because sleep, wake and every replug drop the
wheel back, and `t150d` wants to be another so a game never has to be told to
start it. Both run unprivileged, so neither needs an admin prompt at install.
That is the last thing between this and something a person could just use.

**5. Robustness, then a real game.** Reconnect on both ends, hot plug, and
the watchdog under real crash conditions. Measure the latency and jitter of
the whole path under Rosetta while you are there: a wheel wants updates near
500 Hz and nobody has measured it.

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

**A client connecting opens the wheel's input and disconnecting closes it.**
The firmware renders no effect while no input is open, which is what
[`docs/RESEARCH.md`](docs/RESEARCH.md) A28 established, so the daemon sends
`42 04` on hello and `42 00` when the client goes.

See [`t150d(8)`](man/t150d.8) for the endpoint file, the watchdog and the
effect downgrades.

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
declared size, both together, the other HID node if there is one, the boot
mode product id, and then the rotation range instead of the spring, since a
firmware that silently drops one opcode may accept another. `-r 270` should
make lock to lock obviously short and `-r 1080` put it back.

Watch for `no HID node matches`. `probe_setreport` defaults to the T150's
firmware product id, so against any other wheel, or against one still in boot
mode, every one of these does nothing until `-p` names the id `probe_hid`
actually reported.

**A6. Prove the force feedback protocol.** Two runs have tried and neither
could have worked, so this is a fresh start rather than a repeat. Run both
forms below.

**On the HID path**, which A19 showed reaches the firmware. The last attempt
here left the autocenter at full strength fighting the effect, so the first
packet clears it; the second sets the gain, because nothing has established
what the wheel powers up with:

```sh
./build/bin/probe_setreport \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

**Hold the wheel or keep a hand on the plug**, then stop it:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

**On the interrupt OUT pipe, holding the wheel and hands off.** `-H` keeps the
session open, so the effect is not destroyed by the release, and `-N 32` pads
every packet to the endpoint size, which is what Akellacom's working T300RS
driver does. **Take your hands off the wheel while it runs.** An idle T150
emits about four reports a second and never changes them, so anything that
moves is the wheel moving itself:

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
those are the two concrete reasons force feedback has never worked. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A26 and A27.

Each prints the wheel's own reports for those fifteen seconds and then hands
the wheel back on its own. `nothing ever changed` at the end means the effect did
nothing; a run of changing steering bytes with your hands off is the answer we
are looking for.

**If both are silent, the missing open command is the leading suspect.** Try
the `probe_setreport` form again with the wheel's input held open, by starting
a game in a bottle or leaving CrossOver's controller panel showing the wheel.
The driver's comment says the autocenter is active "while no input are open",
so the firmware distinguishes the two, and the T300RS driver sends an explicit
`60 01 05` before any effect, and the T150's own equivalent is `42 04`. Send
it inline with `-H`, as the interrupt OUT block above does, because the open
lasts only as long as the session. See
[`docs/RESEARCH.md`](docs/RESEARCH.md) A20, A24 and A26.

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

**B2. Start the daemon** and leave it running where you can see it:

```sh
./build/bin/t150d -n -v          # first pass: log only
./build/bin/t150d -v             # second pass: drive the wheel
```

**Hold the wheel or keep a hand on the plug during the second pass.** A game
that asks for a strong constant force will get one.

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

Getting to 4 with `t150d -n` means every layer works as far as the log.
Getting to 4 with the daemon on its real backend means **the wheel should
move**, and that is the end to end path this project was built for. Nobody
has run it yet.

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
