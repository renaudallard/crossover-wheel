# Phase 0: the measurements

Nothing else in this repository was worth writing until these questions were
answered on real hardware. Most now are. This file is what remains open, and
the procedure for rerunning what is settled when something changes.

**The questions that decide the project are answered yes.** The wheel obeys
settings sent through `IOHIDDeviceSetReport`, unprivileged and without taking
the device from CrossOver, and it renders force feedback the same way once
`42 04` has opened its input. What is left is whether this project's own code
can reach it, which is questions 6b, 7 and 8.
[What is already settled](#what-is-already-settled) says which is which, so
nothing here has to be run twice.

**Do them in this order.** The wheel rests holding a full autocenter, which
makes it feel locked and made question 4 unanswerable for six sessions: a
wheel that cannot turn cannot show a spring being applied. Release it with
`-a 0` before judging anything, and never with `-A`, which does nothing. The
mode switch is likewise a precondition rather than a follow-up.

Build them first. On the Mac:

```sh
make probes tools daemon
```

or take them from the `macos` artifact of the `build` workflow, or from a
release.

Run everything as your normal user first. Only add `sudo` where a step says
to, and only after the unprivileged run has been recorded.

The command blocks below carry no inline `# comments`, deliberately. macOS's
zsh does not treat `#` as a comment when a line is pasted interactively, so a
trailing comment arrives as arguments, and every tool here rejects unexpected
arguments by printing its usage and exiting without touching the device. A
whole session can look like it ran and have done nothing.

Watch for `no HID node matches` too. `probe_setreport` defaults to the T150's
firmware product id `044f:b677`, so against a wheel still in boot mode, or
against a different model, every run does nothing until `-p` names the id
that `probe_hid` actually reported.

## Before you start

**Put the wheel's switch in the PS3 position before plugging it in.** The
T150 has a physical selector and it decides which device macOS sees. In the
PS3 position the wheel is `044f:b65d`, the shared boot identity, and the mode
switch in question 2 takes it to `044f:b677` where everything in
[PROTOCOL.md](PROTOCOL.md) applies. In the PS4 position it is `044f:b66d`
instead, a DualShock 4 shaped device with a different descriptor and a
different protocol, and nothing here applies to it: `probe_setreport` will
match no node at all. See RESEARCH.md A4.

PS4 mode looks attractive because it needs no mode switch and its force
feedback is better documented than the T150's. It was investigated and
rejected: the steering and pedals are buried in a 54-byte vendor blob that
nothing decodes without replacing the report descriptor, which a kernel
driver may do and this project may not. RESEARCH.md D7 has the evidence.

**Watch the wheel start up.** A healthy T150 sweeps counterclockwise, then
clockwise, then back to centre as soon as it has mains power. If it does not,
Thrustmaster attributes that to power: use a wall outlet directly rather than
a strip or extension, and a USB port on the machine rather than a hub. A
wheel that has not swept has not learned its end stops and nothing below
means anything on one.

**A wheel that sweeps and is then rigid is normal and is not a fault.** The
autocenter is active whenever no application has the wheel's input open, so a
T150 sitting on a macOS desktop holds itself by design. `probe_setreport -a 0`
releases it. Do that before judging anything else, and do not use `-A`, which
is a different flag and does nothing here. RESEARCH.md A15 and A19.

Three more things on macOS 26 will otherwise waste a run:

- **Approve the accessory.** On an Apple Silicon laptop, new USB accessories
  need approval before they connect at all. If the wheel appears nowhere, look
  in System Settings, Privacy and Security, Accessories. Desktop Macs have no
  such pane and are unaffected.
- **Be the console user.** `setReport` is gated on
  `kIOClientPrivilegeConsoleUser`, so it fails from an SSH session, from the
  login window, and from a fast-user-switched session. Sit at the machine.
- **No sandbox.** These tools are not sandboxed and must not become so:
  App Sandbox breaks IOKit HID access silently.

## What is already settled

Measured on a T150 on macOS 26, so these do not need redoing. Full detail in
[RESEARCH.md](RESEARCH.md) A4 to A11 and A15 to A32.

| | |
| --- | --- |
| The switch position decides the device | PS3 gives `044f:b65d`, PS4 gives `044f:b66d` and is out of scope |
| The wheel identifies itself as a T150 | model `0x03`, attachment `0x06` |
| Endpoint 0 needs no privilege at all | both the model query and the mode switch itself succeeded as an ordinary user |
| The mode switch lands | the wheel re-runs its boot sequence and comes back at `b677` |
| Firmware mode is a joystick | usage `0x04`, `MaxOutputReportSize` **15**, where boot mode was usage `0x05` and 8 |
| No node carries `ProtectedAccess` | so the restricted-device path in A2 does not apply |
| CrossOver does not seize the wheel | writes still returned success with a game running |
| The wheel is not broken | it performs its startup calibration sweep whenever it has mains power |
| It rests rigid until told otherwise | the autocenter is active whenever no application has the wheel's input open, which on macOS is always |
| **`IOHIDDeviceSetReport` moves the wheel** | unprivileged, no capture, `40 03 64 00` holds it and `40 03 00 00` frees it |
| **The interrupt OUT pipe moves it too** | the same bytes through `probe_intr`, so both transports reach the firmware |
| The wheel puts all thirteen buttons on the wire | `probe_intr -R`, and the change mask matches the report descriptor field for field |
| **Force feedback works** | with `42 04` ahead of it to open the wheel's input, on either pipe. Without it nothing renders at all |
| The open outlives its sender | `42 04` persists across process exits and capture cycles until something sends `42 00`, so every sequence that opens must close (A30) |

**What made this look hopeless for six sessions** was `-A`. The autocenter
enable flag decides only whether the effect survives an application opening
the wheel's input; the effect is active whenever nothing has it open. So `-A`
was always a no-op and every "the firmware ignores us" conclusion drawn from
it was wrong, on both pipes. Only the force, `40 03`, releases the wheel.

RESEARCH.md C7 predicted that the HID path could not work. It is **measured
false for this wheel**, and A19 is the measurement.

## The order to run these in

The unanswered questions first. Everything else is a rerun, worth the time
only when something it depends on has changed, which is how test 13 spent
most of a session re-proving what was already proven.

1. **Question 1**, what macOS publishes. Always.
2. **Question 2**, the mode switch, if question 1 found `B65D`. `t150boot`,
   one command, no root.
3. **The input path**, which test 15 promoted from side quest to the gate:
   the wheel does not appear inside the bottle at all, so nothing below it
   in this list can run. RESEARCH.md A34, B10 and B11.
4. **Question 7**, the daemon on its own backend. Half answered, and the
   precondition for question 8.
5. **Question 8**, a game reaching the wheel. The end to end path, the one
   nobody has run.
6. **Question 6**, the buttons on the wire, if the input path work needs it
   reproduced.

The rest are answered and are kept as rerun procedures. Question 6b, the
shipped tools, all outcomes now observed; question 4, the HID path, on a
new machine or after a macOS update; question 3, the interrupt OUT
cross-check, only if the HID path ever stops working; question 5, the force
feedback packets, after any change to `src/lib/encode.c`, plus its one open
item, telling `0x4020` and `0x4021` apart.

Questions 3 and 4 send the same bytes down different pipes, and both work.
Keeping both is what made question 5 interpretable when it was failing: an
effect that moves nothing on either pipe is a layout problem rather than a
transport one. It turned out to be neither, and to be a missing open.

---

## Question 1: is the wheel already in firmware mode?

The T-series enumerates at the shared boot product id `044F:B65D` and only
reveals its real personality after a vendor control transfer flips it to
`044F:B677`. If your wheel is already at `B677` when you plug it in, the
control transfer is not needed at all and question 2 can be skipped.

```sh
system_profiler SPUSBDataType | grep -A5 -i thrustmaster
./build/bin/probe_hid -o .
```

`probe_hid` never opens a device, so it cannot disturb anything and cannot
raise a privacy prompt. Record:

- the product id, `B65D` or `B677`,
- how many HID nodes appear for the one wheel,
- each node's usage page and usage,
- each node's `MaxOutputReportSize`,
- whether `ProtectedAccess` is present on any node.

The node count matters. If the joystick collection and the vendor collection
land on separate `IOHIDDevice` nodes, the daemon has to open the vendor node
for output while CrossOver keeps reading the joystick node. The `.bin`
descriptor dumps written by `-o .` can be parsed offline on Linux with
`hid-tools` and belong in the bug report if anything looks unexpected.

## Question 2: can the wheel be switched to firmware mode?

**Use `t150boot`, which is the shipped tool for this:**

```sh
./build/bin/t150boot
attachment 0x06, model 0x03  T150
switched, the wheel is at 0xb677
```

No `sudo`. It waits for the wheel to reappear at its firmware id before
saying it worked, because the switch transfer's own result cannot say: the
wheel leaves the bus before it completes. It exits 0 when nothing is at the
boot id too, so it is safe to run on every plug-in, and it refuses a wheel
whose model byte is not the T150's rather than sending it another model's
value.

The probe route is the fallback. `probe_intr -I` does the same two transfers
and also sends the five initialisation packets the Linux driver puts on the
interrupt OUT pipe first, which needs the device captured and therefore root:

```sh
sudo ./build/bin/probe_intr -I
./build/bin/probe_hid -o .
```

**Those five packets look unnecessary.** They were adopted to explain a
wheel that came back from the switch apparently blocked, and that turned out
to be the autocenter holding at full strength. Test 13 then switched with
`t150boot` alone, no initialisation packets, and everything afterward
worked: settings, force feedback, buttons on the wire. If a wheel misbehaves
after `t150boot`, try `t150ctl autocenter 0` before reaching for `-I`.

`probe_ep0` remains the tool for asking what a wheel is without touching it,
and `t150boot -n` does the same. Its `-w` switch is the same pair of
transfers `t150boot` sends, minus the wait for the wheel to come back, so
there is no reason left to run it; what it established, that the transfers
need no privilege and that `kIOReturnNotResponding` from the switch is the
expected answer rather than a failure, is in the settled table, in
RESEARCH.md A6, and built into `t150boot`.

## Question 3: does the wheel listen on the interrupt OUT pipe?

**The cross-check on question 4, not a replacement for it.** RESEARCH.md C7
predicted that Thrustmaster firmware acknowledges the control SET_REPORT pipe
and ignores it, and that the pipe it listens to is interrupt OUT. Measured
false for this wheel: both pipes work. Keeping this question is still worth
it, because two independent transports agreeing is what makes a force
feedback failure interpretable. `probe_intr` writes on interrupt OUT.

Reaching that pipe means capturing the wheel from macOS, so this one **needs
root**, unlike everything else here. It captures, writes, and hands the wheel
straight back.

```sh
sudo ./build/bin/probe_intr -a 0
```

That sets the autocenter force to zero, which is what actually frees a
gripped wheel. **Not `-A`**: `0x04` only decides whether the autocenter
survives an application opening the input, and since nothing on macOS does
that, the autocenter is always on and the flag changes nothing. This has been
measured both ways. A wheel that becomes turnable has proven the pipe; one
that does not has a layout problem rather than a transport one.

Its packets come from `src/lib/encode.c`, the same encoders the daemon uses,
so whatever this proves about the wheel it proves about them too. The
settings sweep that used to follow here is question 6b now: `t150ctl` sends
the same bytes with no root and no capture, so running them through a
captured device answers nothing extra.

If it is killed between the capture and the release, the wheel stays gone
from macOS until it is unplugged and replugged. That is the cost of this
route, and it is why it can configure a wheel but cannot drive effects during
a game: holding the pipe means owning the device, and CrossOver cannot read a
wheel this tool owns.

## Question 4: does an unprivileged SetReport move the wheel?

**Answered: yes, and it decided the project.** The wheel obeys an
unprivileged `IOHIDDeviceSetReport` with the device left to macOS, which is
E1 answered and the transport the daemon uses. RESEARCH.md A19. Three
commands rerun it on a new machine or a new macOS.

**Establish the baseline first.** Unplug the wheel, plug it back in, do
question 2, and before sending a single byte turn the wheel by hand and note
what you feel. A freshly plugged wheel holds a full autocenter and feels
locked; that is normal. Without a before there is no after: a wheel obeying
perfectly and a wheel ignoring everything are both immovable, which is
exactly how a whole session was once lost.

```sh
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport
./build/bin/probe_setreport -a 0
```

Expect free, then hard to turn, then free. The autocenter is used rather
than the rotation range because its effect is unmistakable and immediate,
with no need to hunt for the end stops.

**A success return is not the answer.** macOS can accept a report the
firmware then ignores; what settles a run is whether the wheel physically
changed. The framing this sends, report id 0 and the payload raw, is the one
the wheel was measured obeying. The declared id `0x0A` report in the
descriptor is a red herring, and `probe_setreport` keeps `-i`, `-P` and `-n`
for a wheel that ever behaves differently.

## Question 5: does the force feedback protocol work too?

**Answered: it works, and `42 04` was the missing packet.** The same upload
moves the wheel with the open ahead of it and does nothing without, on either
pipe, replicated across two sessions and four runs, and test 13 played the
periodic through the HID pipe as well. RESEARCH.md A28, A29 and A31.

What follows is the procedure that established it. It is worth rerunning
after any change to `src/lib/encode.c`, because it is the only thing that
checks those bytes against hardware rather than against golden vectors.

**The open is not optional, and it outlives the tool that sends it.** Nothing
on macOS opens the wheel's input, so `42 04` has to lead every sequence here.
It is not undone by the tool exiting, by the device being captured and
released, or by another process opening and closing the wheel's HID node:
only `42 00` ends it, which is why every run here has to finish with the
cleanup block below. Test 13 left a sine playing for half a session because
nothing sent the close. RESEARCH.md A28 and A30. `probe_intr -O` sends the
open on its own and `-C` the close.

An effect uploads as three packets that correlate through slot keys, then a
fourth starts it. `-x` is repeatable and every packet goes out on one open
handle.

**On the HID path**, no root. The open, the autocenter cleared, the gain set,
then slot 0, a constant force at half level, endless:

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

**On the interrupt OUT pipe, holding the wheel and hands off.** `-H` keeps
the session open for fifteen seconds and reads the IN pipe while it waits, so
the wheel's own reports show it moving. `-N 32` pads every packet to the
endpoint size, which is what the T300RS driver does. **Take your hands off
the wheel while it runs**: an idle T150 sends about four reports a second and
never changes them, so anything that moves is the wheel moving itself:

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

The `43 60` is the gain. Without it the effect may render at whatever gain
the wheel powers up with, and nothing has established what that is, so a
silent wheel would be ambiguous.

The level byte is `0x20` because a constant appears to top out at `0x40`, not
at `0x7f`: the driver divides a full scale value by `0x1ff`, which lands on
64. A periodic magnitude reaches `0x7f`. If `0x40` and `0x7f` feel the same,
the ceiling is real and `T150_FF_LEVEL_MAX` is right; if `0x7f` is stronger,
raise it.

**Hold the wheel or keep a hand on the plug.** A constant force with no
duration limit does not stop on its own, and it does not stop when the tool
exits either, because the open persists. That includes the `-H` form: the
release hands the wheel back with the effect still rendering.

**Leave the wheel safe when you are done.** Stop the effect and close the
input, one line, no root:

```sh
./build/bin/probe_setreport -x "41 00 00 01" -x "42 05" -x "42 05" -x "42 00"
```

The `42 05` pair is what the vendor driver sends before a close (A26); a bare
`42 00` has also worked. And if the effect drove the wheel into its end
stops, its idea of straight ahead has probably moved with it: it will centre
itself somewhere that is visibly not centre. Unplug it from USB, plug it
back and let it run its sweep; that is the recalibration. RESEARCH.md A32.

**Two corrections are folded into the blocks above.** `42 04` opens the
wheel's input, which no run before ever sent, and `ff_first` is nine bytes
rather than eleven: Thrustmaster's own driver ends it at `fade_level`, and
only a condition carries the two extra `46 54` bytes. RESEARCH.md A26 and A27.


If everything above is silent, the remaining unknowns are in the effect
payloads rather than the packet shapes, since A27 confirmed every shape
against Thrustmaster's own driver. The vendor captures are the place to
look, recovered from the driver repository's git history:

```sh
git clone https://github.com/scarburato/t150_driver.git
cd t150_driver
git show $(git log --all --format=%h --diff-filter=A \
    -- traffic/ffb/windows/constant0.pcapng | head -1):traffic/ffb/windows/constant0.pcapng \
    > windows_constant0.pcapng
tshark -r windows_constant0.pcapng -Y usb.capdata -T fields -e usb.capdata
```

### While you are here: the two missing waveforms

**Answered by test 15.** `0x4021` oscillates smoothly, so it is a real
waveform and by feel not a square; `0x4025` renders nothing, measured the
strong way, by being uploaded over a playing effect and stopping it. What
remains is telling `0x4020` and `0x4021` apart, which needs the two played
back to back and compared by feel; until someone does, square and triangle
stay downgrades. RESEARCH.md A34.

The type code lives in the commit packet, the fifteen-byte one starting
`01 00`. Its third and fourth bytes are the code, little endian: `20 40` is
`0x4020`. One second period, half magnitude:

```sh
./build/bin/probe_setreport \
    -x "42 04" \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "04 0e 00 40 00 00 e8 03" \
    -x "01 00 20 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

Feel it, then stop it:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

For the remaining comparison, run it once with `20 40` and once with
`21 40` as those two bytes, changing nothing else and stopping each one.
The same shape twice means `0x4021` is another sine and both downgrades
stand; a steady linear sweep on one of them means it is the triangle and
the triangle downgrade can go. When you are done, close the input with the
cleanup line above, and remember the wheel may need a replug before its
centre can be trusted.

---

## Question 6: do the wheel's buttons reach the wire?

**Answered, and against the wheel: it sends them.** In firmware mode
CrossOver lists the wheel but registers none of its buttons, though the
wheel's own report descriptor declares thirteen of them in report `0x07`,
after four 16-bit axes and before the hat. This run showed all thirteen
changing on the wire, so the loss is above the USB layer. RESEARCH.md A21.
Rerun it to reproduce that, or against a different wheel.

```sh
sudo ./build/bin/probe_intr -R 15
```

That reads the interrupt IN pipe with the device captured, so nothing sits
between the wheel and the output, and it prints only the reports that differ
from the one before because the wheel streams its state continuously. **Work
every button, the hat and the pedals while it runs**, and move the wheel a
little so you can see the stream is live.

It ends with a mask of every bit that moved at any point. That is the line to
read when an analogue pedal jitters at rest and makes almost every report
different: a byte reading `00` in the mask never changed, whatever you
pressed. Report `0x07` puts the thirteen buttons in the two bytes that follow
its four 16-bit axes, so those are the ones to look at.

| Outcome | Meaning |
| --- | --- |
| A line for each press | **What happened.** The wheel is fine and the loss is in macOS, SDL or winebus, where B8 is the place to look. An input problem, not a force feedback one. |
| No line for any press, stream otherwise changing | Would mean the wheel is not reporting buttons in this mode. Did not happen. |
| Nothing at all | The wheel sends nothing while captured. Check it is still attached, then compare against boot mode, which declares a different report entirely. |
| `the read failed` | The read path itself broke, so the run says nothing either way. Rerun it before concluding anything. |

The reads are asynchronous on a run loop rather than the plain synchronous
call, because IOUSBLib rejects timeouts on an interrupt pipe and the version
without them would block forever on a wheel that reports nothing, which is
one of the outcomes above.

Boot mode puts the buttons first in an unnumbered report, so its layout says
nothing about firmware mode. Run this in whichever mode the question is
about.

---

## Question 6b: do the shipped tools work?

**Answered.** Test 13: `t150boot` switched a real wheel and confirmed it
back at `0xb677`, and `t150ctl status` identified it while it was
mid-effect, through the non-seizing open. Test 15: the range pair was run
and felt, "both runs perfectly", which was the last outcome this question
was waiting on. RESEARCH.md A31 and A34.

Everything before this is a probe, built to answer a question. These two are
what a user actually runs, and they take the same paths the probes proved.

```sh
./build/bin/t150ctl status
./build/bin/t150ctl range 270
./build/bin/t150ctl range 1080
./build/bin/t150ctl autocenter 0
```

| Outcome | Meaning |
| --- | --- |
| `range 270` shortens lock to lock and `1080` restores it | The settings path works through the shipped tool, not just the probe. |
| `autocenter 0` leaves the wheel free | Same, and it is the command a user reaches for when a wheel feels stuck. |
| `no wheel at 044f:b677` | It is not in firmware mode. Run `t150boot`. |
| It asks for a password | Something is wrong. Nothing here needs one. |

`t150boot` is question 2. Run it on a wheel that is already switched too: it
should say there is nothing to switch and exit 0, because that is what a
LaunchAgent firing on every plug-in will mostly see.

## Question 7: does the daemon reach the wheel?

**Half answered by test 13.** The daemon found the wheel, opened it
unprivileged, printed all three lines below, and on Ctrl-C its shutdown
stopped a runaway effect the probes had left playing, which proves its
packets reach the wheel. What it has never done is render an effect a client
asked for, which is question 8. RESEARCH.md A31.

The daemon also scrubs on acquire now: it stops every slot and closes the
input the moment it takes the wheel, so a wheel inherited mid-runaway goes
quiet at startup rather than at shutdown.

Put the wheel in firmware mode first, as in question 2, then:

```sh
./build/bin/t150d -v
```

```
t150d: wheel 044f:b677 open
t150d: listening on 127.0.0.1:<port>, endpoint .../t150ffb/endpoint
t150d: backend macOS HID
```

**No `sudo`.** This path needs no privilege, and being asked for one means
something else is wrong.

| Outcome | Meaning |
| --- | --- |
| The three lines above | The backend found the wheel and opened it. |
| `no wheel yet, will keep looking` | Not an error. It is absent, or still at the boot id. Run `probe_intr -I` and it is picked up within half a second. |
| `cannot open the wheel: ...` with `something has seized it` | Another process holds the wheel exclusively. Nothing in this project does that; find what does. |
| The wheel vanishes from CrossOver when the daemon starts | Stop. The non-seizing open is the assumption the whole design rests on, and it has failed. |

**Nothing moves, and that is correct.** The daemon opens the wheel's input
only when a client says hello, so with no game connected it prints nothing
after those three lines and sends nothing beyond the startup scrub. Silence
is health, not a hang. What connects a client is the proxy in a bottle:
question 8, and the README's Test B is the step by step for it.

## Question 8: does a game reach the wheel?

The end to end path, and the one nobody has run. Its first rung is climbed:
test 14 proved the proxy loads in a real bottle and chain-loads CrossOver's
builtin (A33), so what remains starts at the game. It needs the proxy
installed in a bottle, which is the procedure in the README under testing it
today.

**Currently gated on the input path.** Test 15 ran the game with the daemon
in both modes and it saw no wheel, because the wheel does not appear inside
the bottle at all (A34): no device, so nothing for the proxy to wrap. Solve
that first; B10 in RESEARCH.md is where the investigation stands.

Run it twice. First with `t150d -n`, which prints every packet instead of
sending it, so a fault in the proxy cannot be confused with a fault at the
wheel. Then without, which is the real thing.

**Hold the wheel or keep a hand on the plug on the second pass.** A game
asking for a strong constant force will get one, and a constant force does
not stop on its own.

| Outcome | Meaning |
| --- | --- |
| `write ...` lines appear as the game's force feedback starts, with `-n` | Every layer above the wheel works. |
| The wheel moves, without `-n` | The project does what it was built to do. |
| Packets appear but the wheel does nothing | Compare them against question 5's, which are known to move it. The difference is the bug. |

---

## What the answers decide

Struck through where the wheel has already answered.

| Outcome | Consequence | |
| --- | --- | --- |
| Wheel already at `B677` | No control transfer, no root, anywhere. | not seen, it boots at `B65D` |
| `probe_setreport -a 0` frees a rigid wheel | **E1 is answered yes.** The architecture works unprivileged and CrossOver keeps the wheel. Build it. | **measured** |
| `probe_intr -a 0` frees it as well | Both transports reach the firmware, so the interrupt OUT route is a fallback rather than the plan. | **measured** |
| Neither frees it | The transport is not the problem and the packet layout is. | did not happen |
| The switch works without `sudo` | The finished tool never needs a password. | **measured**, and `t150boot` is that tool |
| It moves on an idle desktop but not with a game running | Something in the bottle is seizing the device. Find out what before writing anything. | still untested |
| Settings work but the force feedback packets do nothing | The transport is fine and the packet layout is wrong. | superseded: it was the missing `42 04`, not the layout (A28) |
| A contiguous type code plays a waveform | Square or triangle stops being a downgrade. Record which code. | `0x4020` oscillates (A29); `0x4021` and `0x4025` untried |
| Buttons change on the IN pipe but CrossOver sees none | An input path problem above USB, in macOS HID, SDL or winebus. Not a force feedback problem. | **measured** |
| The daemon opens the wheel and CrossOver keeps it | The non-seizing open holds, which is the assumption the whole design rests on. | half: the daemon's open is measured (A31), CrossOver alongside it is not |
| A game's effects reach the wheel and it moves | The project does what it was built for. | untested, and the only thing left |
| the switch needs root | One `sudo` per plug-in, and sleep, wake or a replug drops the wheel back to boot mode, so it is a mid-race failure too. | not needed |
| the switch needs `-s` | Reconsider the design before writing more code. | not needed |
