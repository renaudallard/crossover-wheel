# Phase 0: the measurements

Nothing else in this repository is worth writing until these questions are
answered on real hardware. Half an hour with the wheel plugged into the Mac.

**The questions that decide the project are answered yes.** The wheel obeys
settings sent through `IOHIDDeviceSetReport`, unprivileged and without taking
the device from CrossOver, and it renders force feedback the same way once
`42 04` has opened its input. What is left is whether a game can reach it,
which is questions 7 and 8.
[What is already settled](#what-is-already-settled) says which is which, so
nothing here has to be run twice.

**Do them in this order.** The wheel rests holding a full autocenter, which
makes it feel locked and made question 4 unanswerable for six sessions: a
wheel that cannot turn cannot show a spring being applied. Release it with
`-a 0` before judging anything, and never with `-A`, which does nothing. The
mode switch is likewise a precondition rather than a follow-up.

Build the tools first. On the Mac:

```sh
make probes
```

or take them from the `probes-macos` artifact of the `build` workflow.

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
[RESEARCH.md](RESEARCH.md) A4 to A11 and A15 to A21.

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

**What made this look hopeless for six sessions** was `-A`. The autocenter
enable flag decides only whether the effect survives an application opening
the wheel's input; the effect is active whenever nothing has it open. So `-A`
was always a no-op and every "the firmware ignores us" conclusion drawn from
it was wrong, on both pipes. Only the force, `40 03`, releases the wheel.

RESEARCH.md C7 predicted that the HID path could not work. It is **measured
false for this wheel**, and A19 is the measurement.

## The order to run these in

1. **Question 1**, what macOS publishes. Always.
2. **Question 2**, the mode switch, if question 1 found `B65D`. It is a
   precondition, not a follow-up.
3. **Question 4**, the HID path, which is what the project actually needs and
   which is now known to work. It needs no root, so it comes first.
4. **Question 3**, the interrupt OUT write, as the cross-check. It answers
   the same question with the device captured.
5. **Question 5**, the force feedback packets, which work as long as `42 04`
   opens the wheel's input first.
6. **Question 7**, the daemon on its own backend. One command, and it is the
   precondition for question 8.
7. **Question 8**, a game reaching the wheel. The end to end path, and the
   one nobody has run.
8. **Question 6**, the buttons, if you are chasing why CrossOver sees none.
   Independent of everything else.

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

**Use `probe_intr -I`, not `probe_ep0 -w`.** The switch is three steps, not
two: five packets go out on the interrupt OUT pipe first, while the wheel is
still at the boot id, and skipping them leaves the wheel switched but
blocked. That is what every earlier session did.

```sh
sudo ./build/bin/probe_intr -I
./build/bin/probe_hid -o .
```

One capture covers the initialisation and the switch, so nothing
re-enumerates between them. Then **turn the wheel by hand**: on Linux, where
the kernel sends the same five packets, the wheel is free at this point with
no force feedback driver loaded at all.

`probe_ep0` remains the tool for asking what a wheel is and what privilege
endpoint 0 needs. Its `-w` performs the switch without the initialisation,
which is now known to be incomplete.

### The old two-step switch

Only needed if question 1 found the wheel at `B65D`, and then it is needed
before anything else: until this succeeds the wheel is not the device
PROTOCOL.md describes, and it may not even turn.

The mode switch is a pair of vendor control transfers directed at interface
0, the interface macOS's own HID driver owns. It was expected to be refused
and it is not: the read-only model query has succeeded as an ordinary user,
with the device unopened, on the first of the three approaches `probe_ep0`
tries. Confirm that on your wheel, then switch it:

```sh
./build/bin/probe_ep0
./build/bin/probe_ep0 -w
./build/bin/probe_hid -o .
```

`probe_ep0` with no arguments only reads. It prints the model and attachment
bytes of whatever it finds: `0x03` and `0x06` is a T150, and any other pair
means passing that model's switch value with `-V`.

**`-w` has only ever been run under `sudo`.** Try it as your user first, as
above. If it needs `sudo` the finished tool needs a password once per
plug-in; if it does not, it never needs one. That is the whole reason to try.

**`kIOReturnNotResponding` from the switch is the expected answer**, not a
failure. The wheel detaches the instant it accepts the switch, so it is gone
before the completion can come back. Only `probe_hid` can say whether it
worked, and the wheel visibly re-runs its power-on sequence when it does.

Record four things:

- which invocation first succeeded, and as whom,
- whether `probe_hid` now reports `B677`,
- **whether the wheel turns freely by hand afterwards**, which is what makes
  questions 3 and 4 measurable at all,
- the descriptor `-o .` just dumped. Firmware mode reports
  `MaxOutputReportSize` **15** where PROTOCOL.md expects a 14-byte report
  with id `0x0A`, and 15 is exactly the length of `ff_commit`, the one packet
  that never fitted. That dump settles which framing the wheel actually
  declares, and nothing else can.

If the read-only query ever does fail, `sudo ./build/bin/probe_ep0` and then
`sudo ./build/bin/probe_ep0 -s` are the escalations. Be aware of what `-s`
costs if it is the only thing that works: seizing takes the device away from
every other client, so the wheel would vanish from CrossOver for as long as
the daemon holds it, which is not a degraded mode but a different design.

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
measured both ways. **If the wheel
becomes turnable, three questions are answered at once**: the wheel is
healthy, the bytes in PROTOCOL.md are right, and the pipe was the whole
problem. If it does not, the layout is wrong rather than the transport, which
is a different and more tractable failure.

Then the settings, which are what a working `t150ctl` would send:

```sh
sudo ./build/bin/probe_intr -r 270
sudo ./build/bin/probe_intr -r 1080
sudo ./build/bin/probe_intr -g 5000
sudo ./build/bin/probe_intr -a 10000
sudo ./build/bin/probe_intr -a 0
```

Its packets come from `src/lib/encode.c`, the same encoders the daemon uses,
so whatever this proves about the wheel it proves about them too.

If it is killed between the capture and the release, the wheel stays gone
from macOS until it is unplugged and replugged. That is the cost of this
route, and it is why it can configure a wheel but cannot drive effects during
a game: holding the pipe means owning the device, and CrossOver cannot read a
wheel this tool owns.

## Question 4: does an unprivileged SetReport move the wheel?

This is the one that decides the project. It only applies once the wheel is
in firmware mode.

**Establish the baseline first, because the last two runs did not.** Unplug
the wheel, plug it back in, do question 2, and then, before sending a single
byte, turn the wheel by hand and write down what you feel. Everything below
is a comparison against that.

This matters more than it sounds. A measured wheel sat rigid through every
write, which reads like a negative result and is not one: the run set the
autocenter spring to maximum and never turned it off, so a wheel obeying
perfectly and a wheel ignoring everything both ended up immovable. Without a
before, there is no after.

If it will not turn even at `B677` with nothing sent, say so: that is a
separate and more interesting failure than the one this question is looking
for.

Then the single most informative command, on a wheel you have just found to
be rigid, is the one that releases the spring:

```sh
./build/bin/probe_setreport -a 0
```

If the wheel frees up, the firmware has been obeying all along and E1 is
answered yes.

```sh
./build/bin/probe_setreport
```

The autocenter spring is used rather than the rotation range because its
effect is unmistakable: the wheel starts pulling itself back to centre
immediately, with no need to hunt for the end stops. Switch it back off with:

```sh
./build/bin/probe_setreport -a 0
```

**A success return is not the answer.** macOS can accept a report that the
firmware then ignores. What settles this is whether the wheel physically
reacted. If every call returned `kIOReturnSuccess` and the wheel did nothing,
work through the framings before concluding anything:

```sh
./build/bin/probe_setreport -i 0x0a
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -P
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -i 0x0a -P
./build/bin/probe_setreport -a 0
./build/bin/probe_setreport -n 1
```

**The `-a 0` between each one is not optional.** Every variant here is the
autocenter action, which sets the spring to maximum and enables it. Without
releasing it in between, a wheel that obeyed the first command is held rigid
for the rest of the run, and a wheel that obeys nothing looks exactly the
same. Turn the wheel by hand after each pair: the question is whether it
changes, not whether it is stiff.

The reason all four are worth trying: the wheel's own report descriptor
declares a 14-byte output report with id `0x0A`, but the Linux driver ignores
the HID layer entirely and writes 2 to 4 raw bytes on the interrupt OUT
endpoint with no report id. Only the hardware can say which framing the
firmware actually honours.

If none of them work, the useful follow-up is the rotation range instead of
the spring, since a firmware that silently drops one opcode may accept
another:

```sh
./build/bin/probe_setreport -r 270
./build/bin/probe_setreport -r 1080
```

### Also try it in boot mode

Worth five minutes, because a wheel that honours settings before the switch
would make the whole endpoint 0 problem in question 2 go away. Point
`probe_setreport` at the boot id, since it defaults to the firmware one:

```sh
./build/bin/probe_setreport -p 0xb65d
```

Be careful reading a null result here. This has already been run once, on a
wheel that was locked rigid at the time, and every write returned success
while nothing happened. That is not evidence the firmware ignored the bytes:
there was no way to see whether it had.

### Also try it with CrossOver running

Everything above is worth repeating once with a game running in a bottle.
macOS 26 fails `setReport` from every client the moment any process seizes
the device, and the design depends on CrossOver not doing that. A run that
succeeds on an idle desktop and fails with a game running has found something
important.

## Question 5: does the force feedback protocol work too?

**Answered: it works, and `42 04` was the missing packet.** The same upload
moves the wheel with the open ahead of it and does nothing without, on either
pipe, replicated across two sessions and four runs. RESEARCH.md A28 and A29.

What follows is the procedure that established it. It is worth rerunning
after any change to `src/lib/encode.c`, because it is the only thing that
checks those bytes against hardware rather than against golden vectors.

**The open is not optional and it does not persist.** Nothing on macOS opens
the wheel's input, so `42 04` has to lead every sequence here, and
`probe_intr` needs `-H` beside it because the open lasts only as long as the
tool holds the device. `probe_intr -O` sends the open on its own.

An effect uploads as three packets that correlate through slot keys, then a
fourth starts it. `-x` is repeatable and every packet goes out on one open
handle.

**On the HID path**, with the autocenter cleared first this time. Slot 0, a
constant force at half level, endless:

```sh
./build/bin/probe_setreport \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

**On the interrupt OUT pipe, holding the wheel and hands off.** `-H` keeps the
session open for fifteen seconds and reads the IN pipe while it waits, so the
effect has time to do something and its reports are visible. `-N 32` pads
every packet to the endpoint size, which is what the T300RS driver does.
**Take your hands off the wheel while it runs**: an idle T150 sends about four
reports a second and never changes them, so anything that moves is the wheel
moving itself:

```sh
sudo ./build/bin/probe_intr -N 32 -H 15 \
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
duration limit does not stop on its own. The `-H` form releases the wheel by
itself; stop the HID one with:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

**Then with the wheel's input actually open**, which is the one thing never
tried and now the leading suspect. The firmware tracks whether an application
has the input open, which is why the autocenter is "always active while no
input are open", and nothing on macOS opens it. `42 04` does:

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

**Two corrections are folded into both blocks above.** `42 04` opens the
wheel's input, which no run before ever sent, and `ff_first` is nine bytes
rather than eleven: Thrustmaster's own driver ends it at `fade_level`, and
only a condition carries the two extra `46 54` bytes. RESEARCH.md A26 and A27.

The `43 60` is the gain. Without it the effect may render at whatever gain
the wheel powers up with, and nothing has established what that is, so a
silent wheel would be ambiguous.

The level byte is `0x20` because a constant appears to top out at `0x40`, not
at `0x7f`: the driver divides a full scale value by `0x1ff`, which lands on
64. A periodic magnitude reaches `0x7f`. If `0x40` and `0x7f` feel the same,
the ceiling is real and `T150_FF_LEVEL_MAX` is right; if `0x7f` is stronger,
raise it.

**Hold the wheel or keep a hand on the plug.** A constant force with no
duration limit does not stop on its own. The `-H` form releases the wheel by
itself; stop the HID one with:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

**Then with the wheel's input actually open**, which is the one thing never
tried and now the leading suspect. The firmware tracks whether an application
has the input open, which is why the autocenter is "always active while no
input are open", and nothing on macOS opens it. `42 04` does:

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

`-H` is not optional here: the open only lasts as long as the session does.
RESEARCH.md A26.

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

Only worth doing once something oscillates at all. The protocol has type
codes for sine, sawtooth up and sawtooth down but none for square or
triangle, and the codes are contiguous. So `0x4020`, `0x4021` and `0x4025`
may be waveforms the Linux driver never implemented. Upload a periodic and
vary only the commit type code:

```sh
./build/bin/probe_setreport \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" \
    -x "04 0e 00 40 00 00 e8 03" \
    -x "01 00 20 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

That is a one second period at half magnitude with type `0x4020`. Repeat with
`21 40` and `25 40` in the commit packet, the one starting `01 00`. Anything
that oscillates is a waveform we can stop downgrading. Stop it the same way
as above.

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

## Question 7: does the daemon reach the wheel?

The probes proved the packets. This asks whether `t150d` puts the same ones
on the wire through its own backend, which is the piece everything above it
has been waiting for.

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
only when a client says hello, so with no game connected it sends nothing at
all. Question 8 is what connects one.

## Question 8: does a game reach the wheel?

The end to end path, and the one nobody has run. It needs the proxy installed
in a bottle, which is the procedure in the README under testing it today.

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
| `-w` works without `sudo` | The finished tool never needs a password. | **measured** |
| It moves on an idle desktop but not with a game running | Something in the bottle is seizing the device. Find out what before writing anything. | still untested |
| Settings work but the force feedback packets do nothing | The transport is fine and the packet layout is wrong. Recoverable, and much the better failure. | **measured, on both pipes** |
| A contiguous type code plays a waveform | Square or triangle stops being a downgrade. Record which code. | nothing played at all yet |
| Buttons change on the IN pipe but CrossOver sees none | An input path problem above USB, in macOS HID, SDL or winebus. Not a force feedback problem. | **measured** |
| The daemon opens the wheel and CrossOver keeps it | The non-seizing open holds, which is the assumption the whole design rests on. | untested |
| A game's effects reach the wheel and it moves | The project does what it was built for. | untested, and the only thing left |
| the switch needs root | One `sudo` per plug-in, and sleep, wake or a replug drops the wheel back to boot mode, so it is a mid-race failure too. | not needed |
| the switch needs `-s` | Reconsider the design before writing more code. | not needed |
