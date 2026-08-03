# Phase 0: the measurements

Nothing else in this repository is worth writing until these questions are
answered on real hardware. Half an hour with the wheel plugged into the Mac.

Most of them now have answers, and the one that decides the project does not.
[What is already settled](#what-is-already-settled) says which is which, so
nothing here has to be run twice.

**Do them in this order.** The first wheel these were run against sat locked
rigid in boot mode, which made question 4 unanswerable: a wheel that cannot
turn cannot show an autocenter spring. The mode switch is not an optional
follow-up to the measurement, it is a precondition of it.

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

**A wheel that sweeps and is then rigid is a different matter, and is what
has been measured here.** It means the motor, the sensor and the firmware all
work. See RESEARCH.md A9 before spending time on it: the wheel appears to
hold itself by default in every mode, including one where nothing has been
sent to it, so being unable to turn it is not by itself evidence about
anything this project sends.

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
[RESEARCH.md](RESEARCH.md) A4 to A11.

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
| It rests rigid in every mode | including the PS4 position, where nothing from this project reaches it |
| `IOHIDDeviceSetReport` has never changed anything | every framing, both product ids, autocenter and range, all accepted, no reaction |

**The leading explanation is now external.** A shipping macOS driver for the
sibling T300RS states that Thrustmaster firmware acknowledges the control
SET_REPORT pipe and ignores it, and writes on the interrupt OUT pipe instead.
That predicts every result above. See RESEARCH.md C7, and run question 3
before drawing any conclusion from question 4.

## The order to run these in

1. **Question 1**, what macOS publishes. Always.
2. **Question 2**, the mode switch, if question 1 found `B65D`. It is a
   precondition, not a follow-up.
3. **Question 3**, the interrupt OUT write. This is now the most
   informative single command in the set, and on the evidence above it is
   more likely to move the wheel than question 4 is.
4. **Question 4**, the HID path, which is what the project actually needs to
   work and so far never has.
5. **Question 5**, the force feedback packets, once anything has moved.

Questions 3 and 4 send the same bytes down different pipes. That is the
whole comparison: if 3 frees the wheel and 4 does not, the transport is the
problem and the protocol is fine.

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

**Run this before concluding anything from question 4.** RESEARCH.md C7 says
Thrustmaster firmware acknowledges the control SET_REPORT pipe and ignores
it, which is exactly what question 4 keeps measuring, and that the pipe it
listens to is interrupt OUT. `probe_intr` writes there.

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

Only once something has moved the wheel. The settings opcodes prove the
transport; this proves the part the daemon will actually spend its time on,
and it costs one more run.

**On `probe_intr`, not `probe_setreport`.** The HID path is the one already
known to accept everything and act on nothing, so an effect sent that way
tests nothing that has not already been answered. Every packet below goes on
the interrupt OUT pipe, which is the pipe the wheel was shown to obey.

An effect uploads as three packets that correlate through slot keys, then a
fourth starts it. `-x` is repeatable and every packet goes out on one
capture, which is required here rather than merely convenient: handing the
wheel back re-enumerates it, and an uploaded effect will not survive that.

**Clear the autocenter first, in the same run.** At full force it fights
anything the effect does, and it is where the previous command left it. Slot
0, a constant force at half level, endless:

```sh
sudo ./build/bin/probe_intr \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00 46 54" \
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
duration limit does not stop on its own. Stop it with:

```sh
sudo ./build/bin/probe_intr -x "41 00 00 01"
```

If the settings packets moved the wheel and this does nothing, say so
plainly: it means the transport is fine and the force feedback layout is
wrong, which is a much better place to be than the alternative. The next
step from there is a `usbmon` capture on the Linux machine while
`scarburato/t150_driver` drives the wheel, which shows the real bytes rather
than anyone's reading of the source.

### While you are here: the two missing waveforms

The protocol has type codes for sine, sawtooth up and sawtooth down but none
for square or triangle, and the codes are contiguous. So `0x4020`, `0x4021`
and `0x4025` may be waveforms the Linux driver never implemented. Upload a
periodic and vary only the commit type code:

```sh
sudo ./build/bin/probe_intr \
    -x "40 03 00 00" \
    -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00 46 54" \
    -x "04 0e 00 40 00 00 e8 03" \
    -x "01 00 20 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

That is a one second period at half magnitude with type `0x4020`. Repeat with
`21 40` and `25 40` in the third packet. Anything that oscillates is a
waveform we can stop downgrading. Stop it the same way as above.

---

## What the answers decide

| Outcome | Consequence |
| --- | --- |
| Wheel already at `B677` | No control transfer, no root, anywhere. Best case. |
| `-A` frees a rigid wheel | E1 is answered yes. The firmware has been obeying all along and the architecture works. Build it. |
| `probe_intr -a 0` frees it but `probe_setreport -a 0` does not | C7 confirmed on this wheel. Settings are reachable and `t150ctl` can be built; driving effects during a game is not, because it needs the pipe held. |
| Neither frees it | The transport is not the problem and the packet layout is. Better than the alternative, and where PROTOCOL.md gets rechecked. |
| `-w` works without `sudo` | The finished tool never needs a password. Record it. |
| Wheel locked rigid and will not turn by hand | Do question 2, then question 3. Question 3 is the one most likely to free it. |
| Still locked after it reports `B677` | Report it. Nothing in this project holds a wheel rigid, so something else does, and that has to be understood before the measurement means anything. |
| Wheel at `B65D` but honours settings there | Almost as good: the mode switch stops being load bearing. |
| SetReport moves the wheel unprivileged | The architecture works. Build it. |
| It moves on an idle desktop but not with a game running | Something in the bottle is seizing the device. Find out what before writing anything. |
| Settings work but the force feedback packets do nothing | The transport is fine and the packet layout is wrong. Recoverable, and much the better failure. |
| A contiguous type code plays a waveform | Square or triangle stops being a downgrade. Record which code. |
| SetReport succeeds but nothing moves | Try every framing above before giving up; if none work the HID path is closed and only raw interrupt OUT is left, which needs device capture and root. |
| the switch works unprivileged | One clean tool, no password. |
| the switch needs root | One `sudo` per plug-in. Note that sleep, wake or a replug drops the wheel back to boot mode, so this is a mid-race failure, not just an install-time annoyance. |
| the switch needs `-s` | Reconsider the design before writing more code. |
