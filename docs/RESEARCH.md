# What is known, and how

This is the evidence base. Every claim below was checked against source or
official documentation rather than recalled, and carries its citation. A new
session should treat section D as closed: those routes were investigated and
are dead, and re-exploring them wastes days.

Where a claim could not be verified it appears in section E, not in A to C.

---

## A. What already works on a stock Mac

**A1. macOS enumerates the wheel as an ordinary joystick with nothing
installed.** Its descriptor contains a Generic Desktop / Joystick application
collection with a 16-bit X axis, 8-bit Y, Rz and slider, thirteen buttons and
a hat, so the steering and pedals already reach games.

**The buttons were lost for a long time and A21 showed the wheel was not
to blame.** They are declared, they change on the interrupt IN pipe when
pressed, and for many sessions CrossOver registered none of them. Solved:
on the hidraw route with the B10 knob, all thirteen arrive in the bottle
with their identities intact (A37). The remaining pedal mislabel is the
descriptor's own and the proxy corrects it.

> Decoded from `t150_driver/traffic/old_caps/hid_report_fw35`. Corroborated
> for Logitech by CrossWheel's troubleshooting page: "macOS recognizes the
> G29 natively as a game controller without any Logitech software".

**A2. An unprivileged process may open a HID device and call setReport.**
`IOHIDLibUserClient::open()` requires only `kIOClientPrivilegeLocalUser` for
unprivileged clients. Admin rights or an entitlement are required only to
seize a keyboard, or for devices carrying `kIOHIDProtectedAccessKey`.

> apple-oss-distributions IOHIDFamily, `IOHIDFamily/IOHIDLibUserClient.cpp`,
> `IOHIDLibUserClient::open()`.

**A3. Force feedback for a Logitech wheel has been done exactly this way on
Apple Silicon, in the open.** `fffb` opens the wheel with `IOHIDDeviceOpen`
and writes Logitech's classic 7-byte force feedback commands with
`IOHIDDeviceSetReport(kIOHIDReportTypeOutput, ...)`. No kext, no root.

> eddieavd/fffb, `include/fffb/hid/report.hxx` and `include/fffb/hid/device.hxx`.
> Note it opens with `kIOHIDOptionsTypeSeizeDevice`, which this project must
> not do. See B6.

**A4. The wheel's physical selector decides which device macOS sees, and only
one of the two is this project's.** Measured on a T150 on macOS, the first
hardware this project has been run against:

| Switch | Enumerates as | Node | Max output |
| --- | --- | --- | --- |
| PS3 | `044f:b65d` "Thrustmaster FFB Wheel" | one, page `0x01` usage `0x05` | 8 |
| PS4 | `044f:b66d` "Thrustmaster Racing Wheel FFB" | page `0x01` usage `0x05` and page `0xfff0` usage `0x40` | 32 |

`b65d` is the boot identity every T-series wheel shares, so the wheel still
needs the C6 mode switch to reach `b677`. `b66d` is a PlayStation 4
personality: a DualShock 4 shaped descriptor with an output report id 5 of 31
bytes and the `0xF0` to `0xF3` authentication feature reports. Both its
product string and `b65d`'s are generic rather than naming a model, so
neither identifies the wheel; only the C6 model query does.

**Everything in this project assumes the PS3 position.** In the PS4 position
there is no `b677`, none of the protocol in PROTOCOL.md applies, and
`probe_setreport` matches nothing.

> Measured, plus scarburato/t150_driver's README: "For T150, always put the
> switch of your wheel to the `PS3` position before plug it into your
> machine!"

**A5. Two open questions answered, and one still open.** From the same run,
in boot mode: no node carries `ProtectedAccess`, so A2's restricted-device
case does not apply here. Unprivileged `IOHIDDeviceSetReport` of a four byte
unnumbered payload returned `kIOReturnSuccess`, both on an idle desktop and
with Assetto Corsa running in a bottle, so nothing CrossOver does provoked
B6's `kIOReturnExclusiveAccess`.

The wheel did not react: it stayed locked rigid throughout, before, during
and after every write. That is a negative result for those runs and **not**
an answer to E1, for two reasons. They were firmware mode opcodes sent to a
wheel still in boot mode, where nothing obliges the firmware to honour them.
And a wheel that is already held rigid has no room to demonstrate an
autocenter spring or a shorter lock to lock, so the observable the probe
relies on was not visible even in principle.

**So E1 cannot be answered until the wheel is out of boot mode and turning
freely.** Whatever holds it rigid is doing so before any of this project's
code reaches it, so a free wheel is a precondition of the measurement, not
one of its possible outcomes. That makes C6's mode switch, and therefore
`probe_ep0`, the next thing that has to work. It does: see A6.

**A6. The mode switch works, and the model query needs no privilege.**
Measured on the same wheel, in the PS3 position:

- `probe_ep0` as an ordinary user (uid 501), device unopened, returned
  `kIOReturnSuccess` with `49 00 21 00 00 00 06 03 ...`: attachment `0x06`,
  model `0x03`. That is hid-thrustmaster's T150 row, so **the wheel
  identifies itself as a T150** and **E5 is answered: vendor control
  transfers on endpoint 0 are available to an unprivileged process on macOS
  26**, with the device unopened, at step 1 of three.
- `sudo probe_ep0 -w` reported `kIOReturnNotResponding`. That is the
  expected answer rather than a failure: the wheel detaches the instant it
  accepts the switch, so it is gone before the completion can be delivered.
  It re-ran its power-on sequence and came back.
- Afterwards `probe_setreport`, which defaults to `044f:b677`, matched a
  node. **The wheel reaches firmware mode.**

**The switch write also needs no privilege.** A later run performed
`probe_ep0 -w` as uid 501 and the wheel switched. So the whole endpoint 0
path, query and switch, is open to an ordinary user on macOS 26, and the
finished tool never needs a password. E5 is fully answered.

**A7. Firmware mode is not the descriptor PROTOCOL.md describes.** The
`b677` node reports usage page `0x01` **usage `0x04`**, a joystick, where
boot mode reported usage `0x05`, a gamepad. Its `MaxOutputReportSize` is
**15**, not the 14 that C5 decodes from the firmware 3.5 capture.

Fifteen is the length of `ff_commit`, which PROTOCOL.md flags as the one
packet that does not fit a 14-byte report. So either the output report is 14
bytes plus a report id, or it is 15 bytes of payload, and the two readings
imply different framing. **The firmware mode descriptor has not been
dumped**, and doing so is now the cheapest useful measurement left:
`probe_hid -o .` while the wheel is at `b677`.

**A8. In firmware mode every write still returns success and the wheel is
still locked.** Superseded: A15 and A19 explain it. Every run described here
released the autocenter with `-A`, which does nothing, so the wheel was still
holding a full spring throughout and no framing could have shown a difference.
Kept because the framing matrix itself was sound and need not be repeated.

Autocenter on and off, rotation range at both extremes, with
and without report id `0x0A`, padded and unpadded: all `kIOReturnSuccess`,
and the wheel stayed rigid at centre throughout, unable to be turned by hand.

That reading was confounded the first time: every framing variant is the
*autocenter* action, which sets the spring to maximum and enables it, and
nothing disabled it in between, so a wheel obeying perfectly and a wheel
ignoring everything both ended up immovable.

**A third run removed the confound and the answer did not change.** It sent
`-A` first, on a wheel that was already rigid, then worked the whole matrix
with the spring released between each variant, then both rotation range
extremes. Every write returned `kIOReturnSuccess`. The wheel never moved and
never became turnable.

**The confound was not removed, because `-A` does not release the spring.**
That is A15. The matrix ran with the autocenter at maximum from the first
variant onwards, exactly as in the run before it. Substituting `-a 0` is what
made the same tool, on the same wheel, produce A19.

**A9. The wheel is healthy, and rigid is its resting state.** An earlier
reading of this blamed power, on the strength of Thrustmaster documenting a
wheel that fails to calibrate. That is not what is happening: **the wheel
does perform its startup calibration** whenever it has mains power, sweeping
and centring as documented. So the motor drives, the position sensor reads
and the firmware runs. Nothing is broken.

What it does *after* calibrating is hold itself rigid, and it does that in
every mode.

**It is rigid in the PS4 position too**, where the wheel takes no mode
switch, no driver and nothing at all from this project. So the rigidity is
not a response to anything sent: it is what this wheel does on its own once
it has calibrated, in every mode, with no host software involved.

That is worth knowing because it is consistent with the wheel's own default.
`t150_setup_task` in the Linux driver explicitly *disables* the autocenter as
one of the first things it does after probing, which only makes sense if the
wheel powers up with it on. A T150 holding full autocenter is genuinely hard
to move.

**C7 supplied a third and it was wrong.** It held that macOS's
`IOHIDDeviceSetReport` reaches the wheel over the control pipe and that
Thrustmaster firmware acknowledges that pipe and ignores it, on the authority
of an independent macOS driver for the sibling T300RS. A19 measured the
opposite on a T150: an unprivileged `IOHIDDeviceSetReport` changes the
autocenter and the wheel obeys.

**The real answer to this section is A15.** Every attempt that concluded the
wheel ignored us sent `0x04`, the autocenter enable flag, which does nothing
on macOS because the effect is active whenever no application has the input
open. Nothing was ever wrong with either pipe.

The two earlier hypotheses, both also retired by A15 and A19:

1. **HID output never reaches the firmware.** The writes are accepted by
   macOS and discarded, so nothing can change the wheel's resting state. This
   is C3's risk arriving exactly as feared, and it would mean E1 is answered
   no.
2. **The wheel wants something first.** The Linux driver sends a packet on
   the interrupt OUT pipe when the input device is opened, before any setting
   is honoured. This entry said its bytes were not recoverable, because the
   pointer is null at its declaration; A26 found where it is filled in, and
   the packet is `42 04`. Settings turn out not to be gated behind it, since
   they work without. Whether effects are is A26's question and is open.

Both predicted precisely what was measured, which is why neither could be
concluded at the time. Both are now settled and both were wrong: the wheel
frees under `40 03 00 00` on either pipe, so HID output does reach the
firmware and nothing has to be sent first.

The hypothesis that survives from this section is a narrower version of the
second one, and it applies to force feedback rather than to settings. The
autocenter is active "while no input are open", so the firmware does know
whether an input is open, and A20 records what that might mean for effects.

**A10. Two facts bound how rigid the wheel can actually be.**

The wheel calibrates, which means it drives itself through its whole range of
travel moments before it is found immovable. **A mechanical jam in the belt
or gear train is therefore ruled out**: a seized train cannot sweep.

And the T150's motor produces roughly 2 Nm. On a rim of that size it is on
the order of 1.4 kgf at the rim, which two hands beat comfortably. **No
firmware state, however wedged, can make this wheel genuinely immovable.**
Nor can a failed motor bridge: dynamic braking torque is proportional to
speed, so at a standstill it is zero, and a shorted bridge feels like treacle
rather than a wall.

So "cannot be turned" is very likely "very strongly held", which is what a
wheel sitting at maximum autocenter feels like to a light grip. **That is
exactly what it was.** `40 03 00 00` frees the wheel completely, on either
pipe, and the reason nothing this project sent ever persuaded it to stop is
that everything sent was `-A`. A15 and A19.

The electrical against mechanical question this section proposed to settle by
pulling the mains lead is therefore closed, and the answer is neither: the
hold was a firmware setting doing precisely what it was asked to.

> T150 motor output from published reviews; dynamic braking behaviour from
> standard motor control references. The mains-out test is reasoning from the
> T150's two supply rails rather than a documented procedure.

**A11. The wheel behaves identically on Linux, which is the expected result
and not a new fault.** Measured: a stock Linux machine holds the wheel just
as rigidly as macOS does.

That is what stock Linux should do. The kernel ships `hid-thrustmaster`,
which performs the mode switch and nothing else, and `hid-tmff`, which is an
older generic driver that does not speak this protocol. **There is no
in-tree T150 force feedback driver at all**; `scarburato/t150_driver` is out
of tree and has to be installed deliberately. So a stock Linux box reaches
exactly the state this project reaches on macOS: wheel switched to `b677`,
nothing driving it, wheel holding itself.

Two OSes agreeing says the behaviour is not macOS-specific, and it is not a
fault either: the wheel holds a full autocenter until something tells it not
to, and neither OS does on its own. A19.

**This makes the decisive experiment cheap, and it needs no Mac.** Install
`scarburato/t150_driver` on that same Linux machine, which is a DKMS module
with an `install.sh`, and watch the wheel.

- **It releases** — the wheel and the protocol are both fine. This is what
  happened, and it did not need Linux: `40 03 00 00` from macOS does it.
- **It stays rigid** — would have meant the wheel or its firmware revision.
  Did not happen.

And whichever way it goes, the same setup yields something this project has
wanted from the beginning. `usbmon` on that machine captures the exact bytes
a working driver puts on the wire: whether transfers go to the interrupt OUT
pipe or endpoint 0, the real framing behind the 14 against 15 byte question,
and enough traffic to check every encoder in `src/lib/encode.c` against
ground truth rather than against a reading of someone's source.

> `drivers/hid/Makefile` builds `hid-tmff.o` and `hid-thrustmaster.o` and no
> T150 force feedback driver; `scarburato/t150_driver` ships `install.sh`
> and DKMS.

**A12. The interrupt OUT route works mechanically, first time.** `probe_intr`
was run on the wheel and every step of it succeeded: capturing the device
from the HID driver, opening interface 0, finding the pipes, writing, and
handing the wheel back. Autocenter on and off, both rotation extremes and
gain all went out on the pipe and were accepted.

That answers the mechanical question and settles two protocol values that
were guesses:

| | Recorded before | Measured |
| --- | --- | --- |
| Interrupt OUT | `0x02` or `0x01`, unresolved | **`0x01`**, 32-byte packets |
| Interrupt IN | `0x81` | **`0x82`**, 16-byte packets |

The wheel has exactly two pipes on interface 0. The 32-byte maximum means
every packet in PROTOCOL.md fits one transfer, `ff_commit` included at 15.

What this run did **not** record is whether the wheel physically reacted,
which was the only thing E1 needed. Capture, write and release all returning
success says the bytes reached the firmware's doorstep; it does not say the
firmware acted on them. A15 later recorded the reaction, and E1 is answered.

**A13. The wheel is fine, and the initialisation was incomplete.** On a Linux
machine the same wheel switches to `b677`, reports itself as a
`Thrustmaster T150RS`, binds to **`hid-generic`** with no force feedback
driver whatsoever, and **turns freely**.

That is decisive twice over. The wheel is healthy, so nothing mechanical or
electrical is at fault. And nothing on that machine sent it a single setting,
because no T150 driver was loaded, so being free is not something settings
produce: it is what a properly initialised wheel simply is.

The difference is in the initialisation, and it is now identified.
`hid-thrustmaster` calls `thrustmaster_interrupts()` before the model query,
sending five packets on the interrupt OUT endpoint while the wheel is still
at the boot id. This project has only ever sent the two control transfers.
The five packets are recorded in PROTOCOL.md and `probe_intr -I` now sends
them, followed by the switch, without releasing the capture in between so
that nothing re-enumerates between the two.

> `drivers/hid/hid-thrustmaster.c`, `setup_0` to `setup_4` and
> `thrustmaster_interrupts()`, called from `thrustmaster_probe()` before
> `usb_fill_control_urb()`. The same five packets appear in Akellacom's macOS
> T300RS driver, described as mandatory before the mode switch.

**A14. The initialisation changed the wheel's behaviour.** `probe_intr -I`
was run: all five packets went out on the interrupt OUT pipe at the boot id,
the model query answered `0x06`/`0x03`, the switch went out, and the wheel
re-enumerated as `044f:b677 Thrustmaster T150RS`.

**The wheel then turned.** Against force, springing back to centre, where
before the same wheel from the same starting state was immovable. That is the
first physical change this project has ever produced, and it followed the one
thing that had been missing.

The same run finally captured the firmware mode descriptor. Its only output
report is id `0x0A` with 14 bytes of payload, exactly as the firmware 3.5
capture said, and `MaxOutputReportSize` 15 is those 14 plus the report id.
**So `ff_commit`, at 15 bytes of payload, cannot fit the HID output report at
all**, while it fits the interrupt OUT pipe's 32-byte packets easily. That
retires the contradiction PROTOCOL.md carried from the start, in favour of
the interrupt OUT route.

**A15. The wheel obeys settings sent on the interrupt OUT pipe. E1 is
answered yes for that route.** Measured: `probe_intr -a 0`, which sends
`40 03 00 00` followed by `40 04 01 00`, left the wheel **free to turn with
no resistance at all**, from a state where it had been gripped. That is an
unambiguous physical reaction to bytes this project chose and sent.

It also explains every earlier failure to free it, and the explanation was in
the driver source the whole time. `t150_set_enable_autocenter`'s comment:
"true if the autocenter effect is to be kept enabled when the input is
opened. **The autocentering effect is always active while no input are
open**".

So `0x04` is not an on/off switch. It decides whether the autocenter survives
an application opening the input, and nothing on macOS opens the input the
way a Linux application does, so the autocenter is permanently active and
`0x04` changes nothing observable. Every `-A` sent in every session was a
no-op by design. **Only the force, `0x03`, releases the wheel.**

That retires the whole "the wheel is locked" line of investigation. The wheel
was never locked, never faulty, and never ignoring us once the initialisation
was right: it was holding a maximum autocenter that the command we kept
sending was never going to lift.

**A16. Force feedback has not been demonstrated, and has not yet been tested
properly.** Superseded by A20, which tested it properly and got the same
answer. Kept for the reasoning, one part of which did not survive: the HID
path was thought at the time never to have delivered a setting, and A19 later
showed it delivers them fine. The `ff_commit` length objection stands as a
question, since it is 15 bytes against a declared 14-byte report.

The effect upload sequences were sent through `probe_setreport`, the HID
path, and the wheel did not move. That was not evidence about the protocol at
the time, because the pipe then believed to be the only working one was not
used.

The test that would mean something is the whole sequence in **one**
`probe_intr` capture, because the release re-enumerates the device and an
uploaded effect is unlikely to survive it.

A second session ran the effect sequences again and again saw no movement,
and again sent every packet through `probe_setreport`. That was this
project's fault, not the tester's: PROBES.md and the README both still
prescribed the HID path for force feedback long after the settings
instructions had been corrected. Both now prescribe `probe_intr`.

That run also left the autocenter at maximum force on the interrupt OUT pipe
and never cleared it, so even had the effect packets arrived they would have
been working against a full-strength spring. The corrected sequence clears it
in the same capture.

**A17. Autocenter force has a physical effect, and it is not the effect its
name implies.** Measured: with the force set through the whole range the
wheel's resistance changes, and the change is felt as a stiffening around a
point rather than as a pull back to centre. Releasing the wheel does not
return it to zero.

This is the first time a parameter *value* this project sent has changed how
the wheel feels, as distinct from A15's on-or-off result, and it confirms the
settings transport a second way.

**A later run resolved what shape it is, and it is a spring.** At full force
the wheel is "hard to turn from position 0", which is a spring anchored at
centre resisting movement away from it. At force 10 of 100, `40 03 0a 00`,
there is no perceptible centring but the wheel is stiff for roughly the first
25 degrees. That is the same spring, too weak to overcome the belt and gear
train's own friction, so it resists leaving centre and cannot drag the wheel
back. The earlier "does not restore" reading was the friction, not the
opcode.

A fourth run characterised it further at force 10: past roughly 25 degrees the
wheel gets harder to turn, and on release it returns only part way, to a
smaller angle rather than to centre. From about 120 degrees it returns further.
That is a spring whose restoring torque grows with displacement, working
against a fixed friction it can only overcome once it is wound up far enough.

So `0x03` is a spring force in hardware percent, exactly as PROTOCOL.md
records it, and `t150ctl` can present it as one. A user interface should not
promise that a low setting recentres the wheel, because below roughly a
quarter turn it will not.

**A18. The wheel's buttons do not reach Wine in firmware mode. Cause not yet
established.** Superseded by A21, which established it: the wheel puts them
on the wire, so the loss is above the USB layer. Kept for the descriptor
decode and for what `-R` was built to answer. Observed: with the wheel at `0xb677`, CrossOver's game
controller panel lists the wheel, but no button on it registers.

The wheel's own descriptor does declare them. Report `0x07` carries four
16-bit axes, then 13 buttons, then a hat, and its total matches the reported
`MaxInputReportSize` of 15 exactly. So either the wheel is not putting the
bits on the wire or something above the USB layer is dropping them, and
nothing that reads only HID node properties can tell those apart.

`probe_intr -R` was added for this. It reads the interrupt IN pipe with the
device captured, so what it prints is what the wheel sends with nothing in
between. A button that produces no line there never left the wheel; one that
does means the loss is in macOS, SDL or winebus, and B8 is where to look
next.

Note this is a firmware mode observation. Boot mode declares a completely
different report, buttons first and no report id, and that is the layout
every earlier session saw.

**A19. `IOHIDDeviceSetReport` moves the wheel. C7 is false for the T150, and
the architecture the project was designed around is the right one.**

Measured, unprivileged, unnumbered report id, 4-byte payload, no capture:

| Command | Payload | Observed |
| --- | --- | --- |
| `probe_setreport -a 0` | `40 03 00 00` | free to turn |
| `probe_setreport` | `40 03 64 00` | **hard to turn** |
| `probe_setreport -a 0` | `40 03 00 00` | **free to turn** |

Free, then held, then free, with all three transitions driven through the HID
layer and no `probe_intr` between them. The wheel had been left free by the
preceding interrupt OUT run, so the stiffening is attributable to the HID
write and to nothing else.

**A second setting confirms it independently.** `probe_setreport -r 270` then
`-r 1080`, again unprivileged, and the observation was that "the command for
blocking the wheel at an angle is good and run perfectly": the wheel's end
stops move. So the HID path carries opcode `0x11` as well as `0x03`, and this
is not a quirk of one opcode.

This is the single most valuable result the project has. It means the daemon
can drive settings with a non-seizing `IOHIDDeviceSetReport` while CrossOver
keeps the wheel, so there is no ownership conflict for settings, `t150ctl`
needs no root, and the interrupt OUT route is a fallback rather than the
plan. The design problem recorded here and in the README is withdrawn.

It also says the six sessions of "the firmware ignores the HID layer" were
an artefact of `-A`. Both pipes work. Neither was ever the problem.

**A20. Force feedback has moved nothing yet, and neither run that tried was
capable of showing it.** An earlier draft of this entry concluded that the
effect layout must therefore be wrong. That conclusion is withdrawn: it does
not follow, because both attempts were confounded, each in a different way.

**The interrupt OUT attempt.** All six packets in one capture, autocenter
cleared, gain set, for a constant force and again for a periodic with type
`0x4020`:

```
40 03 00 00 / 43 60 / 02 1c 00 .. 46 54 / 03 0e 00 20 /
01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00 / 41 00 41 01
```

Every write accepted, no movement. But `probe_intr` hands the wheel back the
moment the last packet goes out, and the release is a
`USBDeviceReEnumerate`. **An uploaded effect cannot survive that**, so a
correct upload and a wrong one produce the same silence. `-H` was added to
hold the device open instead, and no run has yet used it.

**The HID attempt.** The same four effect packets through `probe_setreport`,
which A19 later showed reaches the firmware. Also silent, and also
confounded: the last autocenter force sent before it was `40 03 64 00`, and
everything after that was `-A`, which clears nothing. **The effect was
working against a full-strength autocenter throughout.**

So the honest position is that force feedback is untested, not that it fails.
The three things to change, all from comparing against Akellacom's T300RS
driver, which does deliver force feedback on macOS:

- **Hold the session open.** Its `thrustmaster_usb_start()` captures the
  wheel, keeps the interface open, and pumps a `ReadPipeAsync` on the IN pipe
  for the life of the session. `probe_intr -H` now does the same.
- **Open force feedback first.** Before range, before gain, before any
  effect, it sends `60 01 05`: report id `0x60`, command `0x01`
  "openClose", argument `0x05`. That is the T300RS analogue of the T150
  driver's input-open callback, which this project had never sent. A26 has
  the T150's own bytes, `42 04`, from the same driver.
- **Pad and pace.** Every packet it sends is zero-padded to 64 bytes with
  `memset` then `WritePipe(..., 64)`, and it sleeps 50 ms between setup
  packets. This project sends bare 2 to 15-byte packets back to back. The
  T150's OUT endpoint maxes at 32, so 32 is the analogue, and `probe_intr -N`
  has always been able to do it.

**None of the T300RS bytes transfer.** Its command set is different
throughout: everything carries a `0x60` report id prefix and the commands are
`0x01`, `0x02`, `0x08`, `0x6A`, `0x89`, where the T150 uses `0x40`, `0x43`,
`0x02`, `0x03`, `0x41`. What transfers is the shape: open first, pad, pace,
hold.

> Akellacom/thrustmaster_t300rs_gt_macos_driver,
> `Sources/CUSBModeSwitch/CUSBModeSwitch.c` `thrustmaster_usb_start()` and
> `ff_constant_inner()`, and `Sources/ThrustmasterWheel/T300RSProtocol.swift`
> `SetupCommand.openClose` and `openCommand`.

Only after all three, and only if it is still silent, is the layout the
suspect. The answer then is a `usbmon` capture on the Linux machine with
`scarburato/t150_driver` driving the wheel, which shows the real bytes rather
than anyone's reading of the source. A26 has since recovered the open
packet from the source itself, so the capture is no longer needed for that.

**A23. An idle T150 is completely static on the wire, which is the control an
effect test needs.** Measured: `probe_intr -R 15` with nothing touched
returned **61 reports, 1 of them different from the one before**. So the wheel
emits roughly four reports a second at rest and its report never changes.

That makes `-H` decisive when it is run properly. If an effect renders, the
steering bytes move with nobody touching the wheel, against a baseline that is
otherwise flat. Nothing else this project can measure separates "the effect
did nothing" from "the effect was never running".

**A24. Force feedback through the HID path, properly run, moved nothing.**
The first genuinely clean effect test. `probe_setreport` with all six packets
on one open handle, the autocenter cleared first and the gain set:

```
40 03 00 00 / 43 60 / 02 1c 00 .. 46 54 / 03 0e 00 20 /
01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00 / 41 00 41 01
```

Every write accepted, the wheel did not move. None of A20's confounds apply
to this one: the autocenter was at zero, the gain was set, all six packets
went out, and the HID path does not re-enumerate the wheel. The one thing it
does do is close the handle immediately afterwards, and whether an effect
survives an `IOHIDDeviceClose` is unknown.

**The interrupt OUT run alongside it is still not readable**, for a reason
that is this project's fault again. `-H` reused the reader's own instructions,
which tell the tester to work every button and pedal, so the wheel was being
handled throughout its fifteen seconds and its five thousand changing reports
say nothing about the effect. Fixed, and the retest is one command.

So the position is: **one clean negative on the HID path, none yet on the
interrupt OUT pipe.** What has still never been tried is the open command.
The T300RS driver sends `60 01 05` before any effect (A20) and the T150 driver
has an equivalent, `42 04`, which A26 recovered from the same source. No run
here has ever sent it. That is now the leading explanation, ahead of the
layout.

**A25. Enabling winebus's hidraw backend makes the wheel vanish from
CrossOver entirely.** Measured: with hidraw and SDL both enabled in
CrossOver's controller settings the wheel is not listed at all; with either
off it is listed. In every combination CrossOver registers no button and no
axis movement.

That is a real datum for B8 rather than a workaround: the hidraw path is the
one that would carry the wheel's own report descriptor into the bottle, and it
drops the device instead of describing it. Whatever is losing the buttons is
in that neighbourhood.

**A26. The packet that opens the wheel's input is recoverable after all, and
this project has never sent it.** A20 and earlier entries said its bytes were
left null in the published source. That was a misreading: the pointer is null
at its declaration and `t150_init()` allocates and fills it.

```
42 04   open the input
42 05   sent twice, immediately before the close
42 00   close the input
```

Two bytes each, built as little-endian `uint16` `0x0442`, `0x0542`, `0x0042`,
so the opcode leads on the wire. Sent with `usb_interrupt_msg()` on `pipe_out`
with length 2, the open before `hid_hw_open()` and the other two after
`hid_hw_close()`.

**Why this is the leading explanation for A24.** The firmware knows whether an
application has the input open; that is established, not inferred, because
`t150_set_enable_autocenter`'s comment says the autocenter "is always active
while no input are open" and this project measured exactly that behaviour for
six sessions. Nothing on macOS opens the input, and no run here has ever sent
`42 04`, so on every effect test so far the wheel had no application attached
as far as it was concerned.

Akellacom's T300RS driver sends its own open, `60 01 05`, before range, gain
or any effect (A20). Same shape, one wheel family over, and it delivers force
feedback.

The test is one command, and it needs `-H` beside it because nothing holds the
input open once the tool exits:

```sh
sudo probe_intr -N 32 -H 15 -x "42 04" -x "40 03 00 00" -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00 46 54" -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" -x "41 00 41 01"
```

> `hid-t150/hid-t150.c` `t150_init()`, and `hid-t150/input.c`
> `t150_input_open()` and `t150_input_close()`.

**A27. The driver repository ships packet captures, including from
Thrustmaster's own Windows driver, and they settle five things.** Readable
with `tshark -r f -Y usb.capdata -T fields -e usb.capdata`. The vendor ones
are `traffic/ffb/windows/*.pcapng`, which are **not in the working tree**: they
were deleted in commit `7c1f80e` and have to be recovered from git history.

**`42 04` is on the wire**, which confirms A26 independently of the source.
Thrustmaster's own constant-force capture opens like this:

```
42 04                                  <- open the input
40 04 00 00                            <- autocenter enable = 0
40 03 0d 00                            <- autocenter force 13
43 80                                  <- gain, full scale
```

Note it clears the enable flag and sets a low force, where this project sends
force then enable. And `43 80` is full gain, which is A28's evidence too.

**The complete vendor upload**, and it is the sequence this project sends
except for one packet:

```
42 04
02 1c 00 e8 03 02 e8 03 01                     <- ff_first, NINE bytes
03 0e 00 3e                                    <- ff_update constant, 4 bytes
01 00 00 40 c4 09 00 00 00 0e 00 1c 00 00 00   <- ff_commit, 15 bytes
   ... slots 1 to 5 the same ...
41 00 41 01                                    <- play slot 0
41 01 41 01
```

So `ff_update` at 4 bytes for a constant, `ff_commit` at 15 and the control
packet `[0x41, slot, 0x41, 0x01]` are all confirmed exactly as documented.

**`ff_first` is nine bytes for a constant or a periodic, and this project has
been sending eleven.** The vendor ends it at `fade_level`. The two extra bytes
this project appends, `46 54`, sit at the head of every effect upload it has
ever tried. Together with the missing `42 04` that is two concrete reasons
force feedback has never worked.

**The trailer belongs to conditions only, and is not one constant pair.**
Spring uploads end `46 54`, damper uploads end `64 64`:

```
05 1c 00 00 00 00 00 00 00 46 54     spring ff_first
05 1c 00 00 00 00 00 00 00 64 64     damper ff_first
```

`0x54` and `0x64` are exactly the spring and damper saturation maxima this
project already records, so the trailer reads as a saturation hint keyed to
the effect type rather than the magic numbers the Linux driver hardcodes. That
driver sends `46 54` for both, which is why PROTOCOL.md called them
"unexplained".

**One earlier reading of this was wrong and is withdrawn.** An earlier version
of this entry cited `traffic/sine0_linux.json` as evidence that no capture
contains `46 54`. That capture is nine bytes because it predates the fields:
it was added on a side branch forked the day before `f2`/`f3` were introduced,
and `git show fa59bba:t150/forcefeedback.h` shows `struct ff_first` ending at
`fade_level`. It says nothing about the vendor. The vendor captures above do,
and they happen to agree for class `0x02`.

**A28. Force feedback works. The missing piece was `42 04`.** Measured on a
T150, and the project's first moving wheel.

Two runs, identical but for one packet:

```
sudo probe_intr -N 32 -H 15 \
    -x "40 03 00 00" -x "43 60" \
    -x "02 1c 00 00 00 00 00 00 00" -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" -x "41 00 41 01"
                                          -> nothing

sudo probe_intr -N 32 -H 15 \
    -x "42 04" \
    ... the same six packets ...
                                          -> "it turn the wheel to the max
                                             left and next it have the max
                                             force to prevent i turn it"
```

So the wheel does render effects, and it renders them only once something has
opened its input. That is A26's packet, recovered from the driver's own
source after four sessions of assuming its bytes were unrecoverable.

**The tool confirmed it independently.** `-H` reads the interrupt IN pipe
while it holds the wheel, and that run returned 829 reports, 784 of them
different, with this mask:

```
00 ff ff 00 00 00 00 00 00 00 00 00 00 00 00
```

Only bytes 1 and 2 moved, which is the steering axis. No button, no pedal, no
hat. The wheel was turning under its own power with nobody touching it, which
is exactly what `-H` was built to make visible and what an idle T150 never
does (A23: 61 reports, one of them different).

**Periodics work too, and on the HID path.** A later run through
`probe_setreport`, with no capture and no root, played a periodic and the
wheel "turn right left indefinitely". So both transports carry effects, and
the earlier `42 04` was still in force across a release and re-enumeration.
Whether the open survives a replug is untested.

**`0x4020` is a real waveform.** That periodic committed with type `0x4020`,
which PROTOCOL.md lists as a guess: the codes are contiguous around the known
periodics and `0x4020`, `0x4021` and `0x4025` "may be the missing waveforms".
One of them is. It oscillates. Which waveform it is, square or triangle,
needs a run that compares it against `0x4021` by feel, and until then the
downgrade table stays as it is.

**A first reading of this entry called the force wrong, and that was a
mistake.** It said a constant at level `0x20` driving the wheel to full left
lock and holding it there meant the direction or level scaling was broken. It
does not. A DirectInput constant force is a steady torque, and a free wheel
under a steady torque travels to its stop and stays pressed against it. That
is the effect behaving correctly.

Two further reasons the reading was unfounded. The packet was typed by hand
from the procedure, so it never went through `src/lib/encode.c` and said
nothing about this project's arithmetic either way. And a sweep of that
encoder afterwards produces exactly the sine projection and linear scaling it
should: `0`, `±45`, `±64` across the compass at full magnitude, and 64, 32,
16, 0 down the magnitude range, symmetric in sign. `tests/encode_check.c`
`test_constant_level()` now pins all of it.

**What is genuinely untested is everything except a constant and one
periodic.** Springs, dampers, envelopes, ramps and per-effect gain have never
reached hardware, and each is arithmetic derived from a driver rather than
measured. The vendor captures in A27 are the reference to check against, and
their constant sends level `0x3e` against this project's ceiling of `0x40`,
which is the one number that has independent support.

**A29. A28 replicated, two for two, and the force is a force rather than a
lock.** A single session ran the same upload four times, twice without the
open packet and twice with it:

| run | transport | `42 04` | observed |
| --- | --- | --- | --- |
| 1 | HID | no | nothing |
| 2 | interrupt OUT, held | no | nothing; "i've only turn the wheel" |
| 3 | interrupt OUT, held | **yes** | "turn to max left automatically and make force to stay in this position" |
| 4 | interrupt OUT, held | **yes** | "it turn with force to max left" |

That is the cleanest evidence the project has. The `-H` reads back it up: all
three held runs changed only the steering bytes, `00 ff ff 00 ...`, but in run
2 the person was moving the wheel and in runs 3 and 4 it moved itself.

**The force is moderate and a person overpowers it.** "I've turn it to max
right with hands", and on the next run three full turns of it. So level
`0x20`, half the documented ceiling, is a real half-scale torque. This is what
settles the claim withdrawn above: a constant force reaching the stop is the
effect working, and the wheel is not being commanded to a position.

**The stop packet releases it.** `41 00 00 01` gave "unlock the force that
force to stay max anti-clockwise".

**A periodic oscillates and drifts.** `04 0e 00 40 00 00 e8 03` committed as
type `0x4020`, so magnitude 64 of a possible 127, no offset, no phase, a one
second period. The wheel swung through roughly 90 degrees and kept going
indefinitely, but travelled further anticlockwise than clockwise each cycle.

**Do not read that drift as a protocol fault yet, because the wheel's centre
has moved.** The same session reports the end stops at about 170 degrees left
and 190 degrees right from one position, which puts the sensor's idea of
centre roughly ten degrees off. A symmetric torque about a centre that is
itself displaced looks exactly like a drifting one. Re-seat the wheel by
unplugging it from mains and USB, let it calibrate, and repeat before
suspecting the waveform.

**Autocenter returns the wheel in steps.** "Perfect center, but the steering
wheel returns in several stages, gradually, with more stages depending on the
angle." That fits A17's progressive spring: the restoring torque grows with
displacement, so from far out it moves, stalls against friction, and moves
again, more times the further it started.

**CrossOver still sees no button and no axis**, in either its DirectInput or
its XInput view, on a wheel that is at that moment being driven by this
project. A21 and A25 stand.

**A30. The input open outlives everything except the close.** Test 13
settled how long `42 04` lasts: until something sends `42 00`, and nothing
else ends it. Not the exit of the process that sent it, not the device being
captured and released, not another process opening and closing the wheel's
HID node.

The evidence is a chain with no other explanation:

- The question 5 HID sequence, sent before any open had ever been sent, did
  nothing: "i've see no change". A28, as expected.
- Two interrupt sessions then sent `42 04`, held the wheel, released it and
  exited.
- The same upload as a periodic, through `probe_setreport`, from a fresh
  process, with no `42 04` of its own, swung the wheel left and right
  indefinitely. The only open it could have been riding was one a dead
  process had sent, across the other pipe.
- The oscillation survived that tool's own close of the HID node, two further
  capture and release cycles, and the rest of the session, until `t150d` was
  interrupted. Its shutdown sends `42 00`; the rendering stopped there.

Three consequences. Every sequence that opens the input must close it, or it
leaves a wheel rendering whatever was last started, indefinitely, with
nothing visibly attached to it; PROBES.md question 5 now ends with a cleanup
block. The daemon cannot assume a fresh wheel, since it may inherit one whose
input a dead process left open with an effect still playing, so it stops
every slot and closes the input when it takes the wheel. And the earlier
claim that the open "lasts only as long as the session" is withdrawn: it was
an inference, never a measurement, and the measurement says otherwise.

**A31. `t150boot`, `t150ctl` and `t150d` all touched hardware for the first
time, and all worked.** Test 13 again.

- `t150boot` switched the wheel out of boot mode and confirmed it back at
  `0xb677`. Question 2 is now closed by the tool a user would actually run,
  not just by the probe.
- `t150ctl status` identified the wheel correctly, through a non-seizing open
  taken while the wheel was mid-runaway. `range 270`, `range 1080` and
  `autocenter 0` all ran without error, but nobody recorded whether lock to
  lock actually shortened, so question 6b's decisive outcome is still
  unobserved.
- `t150d -v` found the wheel, opened it unprivileged and printed its three
  startup lines. On interrupt, its safe state did something better than any
  staged test: it ended a runaway effect it had never been told about. The
  panic only stops slots in the daemon's own table and this sine was in none
  of them, so what stopped the rendering was the `42 00` its session end
  sends. A rescue by close, on hardware, of exactly the failure A30
  describes.

The periodic also rendered through the HID pipe, the same `0x4020` upload as
A29's. `0x4021` and `0x4025` remain untried: the session ran the `20 40`
commit twice rather than editing it, so square and triangle are still open.

**A32. Driving the wheel into its stops moves its idea of centre.** After
the runaway sine had worked against the left stop for a long stretch, the
wheel came to rest "not at position 0... only the more nearby from the max
left": it straightened itself to a centre that was visibly wrong. The T150
has no absolute reference, it calibrates by sweeping its end stops at
power-on, so lost steps accumulate into a shifted zero and every angle after
that is read through the shift. Unplug the wheel from USB, plug it back and
let it sweep before trusting any measurement that involves position. A29's
drift note already suspected this; test 13 watched it happen.

One loose end from the same moment, recorded rather than concluded: the
daemon's panic zeroes the autocenter force before its close, yet the wheel
straightened itself after the close, which reads as the firmware restoring
its own autocenter once the input shuts. Whether `42 00` resets the
commanded force is unmeasured, and nothing yet depends on it.

**A33. The proxy loads in a real bottle and chain-loads CrossOver's
builtin.** Test 14, on CrossOver 26.3.0, in a Steam bottle, with
`regsvr32.exe dinput8.dll` and `+loaddll` tracing. The passing run shows
`dinput8.dll` as `native`, which is the proxy, then the builtin's own
imports loading, `hid.dll`, `setupapi.dll`, `comctl32.dll`, `oleaut32.dll`,
then `dinput8_orig.dll` as `builtin`, then a clean unload and no failure
from regsvr32. The proxy executed, resolved the file beside it, and
forwarded a call into CrossOver's own implementation. M4's chain-load
question is answered; what has still never run is the force feedback path,
because the game launch that followed lost `$CX_ROOT` and never started.

The failure the day before is instructive and is why the loaddll tag
matters more than the file listing: both `dinput8.dll` and
`dinput8_orig.dll` were the proxy, byte for byte, so the chain-load found a
real PE, loaded it as `native`, and `DllRegisterServer` failed downstream.
The tag comes from the file's own bytes, `Wine builtin DLL` at offset 64,
which also means a `head -c 64` can never show it: the check in the README
stopped one byte short of the signature and printed nothing on every file.

Two more firsts from the same session's warm-up. The question 5 HID block
with its own `42 04` drove the constant to the stop through nothing but
`probe_setreport`, so both effect classes now render on both pipes with the
open sent on the same pipe. And the stop plus `42 05 42 05 42 00` cleanup
line freed the wheel as designed. One observation recorded rather than
concluded: after the stop, with the input open, the last 45 degrees or so
before each end stop turned hard, which reads like a firmware soft stop.
Still untried after three sessions: `0x4021` and `0x4025`, because the
waveform run used `20 40` again, and an isolated, felt `range 270`.

**A34. `0x4021` is a waveform and `0x4025` is not, and the game cannot see
the wheel at all.** Test 15.

- **`0x4021` renders and oscillates.** "Wheel turn smooth": no hard flips,
  so by feel it is not a square. Whether it is a triangle or another
  sine-alike needs a back-to-back comparison against `0x4020`, so the
  square and triangle downgrades stay until then.
- **`0x4025` renders nothing, and the evidence is unusually clean.** The
  stop between blocks was skipped, so the `0x4025` upload went into slot 0
  over the still-playing `0x4021` and the motion ceased: a type that
  replaces a running effect and produces no force is a type the firmware
  does not render.
- **The shipped range change is felt.** `range 270` then `range 1080`,
  "both runs perfectly", which closes question 6b's last outcome.
- The regsvr32 chain-load check passed again after re-copying the builtin.

**The blocker moved.** With the daemon on `-n` and on its real backend,
"assetto corsa didn't see the wheel on both": the wheel does not appear
inside the bottle at all, so no DirectInput device exists for the proxy to
wrap and question 8 cannot start. The input-path problem, called separate
and non-blocking in every document since A21, now gates the project's goal.
Note the regression shape: A1-era sessions had steering and pedals working
in games, so the wheel did reach the bottle once on this machine and does
not now. B8 and B9 were researched assuming the wheel arrives without
force feedback; the measured reality is that nothing arrives. Resolved in
test 16: A35.

**A35. The wheel is back in the bottle, and the proxy has run inside a
real game.** Test 16.

- **`SDL_JOYSTICK_HIDAPI=0` restored the wheel.** The tester reports every
  earlier step succeeding, and Assetto Corsa listed the wheel with live
  axes. B11's mechanism, inferred from source, is confirmed by experiment,
  and the variable belongs in the bottle's `cxbottle.conf` permanently.
- **The proxy loaded inside `acs.exe`, on both of the game's launches**,
  `DINPUT8.dll` as native then `dinput8_orig.dll` as builtin, the whole
  chain-load inside the real game this time, with live dinput traffic
  after it. A 32-bit Steam helper got the plain builtin fallback, which is
  the 64-bit-only design behaving as documented. One lesson for log
  readers: the game asks for `DINPUT8.dll` in upper case, so a
  case-sensitive search misses the most important line in the file.
- **No force feedback was possible, and the reason is ordering, not a
  defect.** The daemon had been stopped before the game started, in both
  passes; the one `t150d -v` run began after the game was already up. The
  proxy looks for the endpoint once, when the game creates its DirectInput
  device, and a daemon that appears later is invisible to a game already
  running. The daemon must be running before the game starts, and stay
  running.
- **The proxy was silent throughout, which is its own finding.** A
  Steam-relaunched game's stderr goes nowhere, so every `T150_DEBUG` line
  was lost. `T150_LOG` now appends the same lines to a file from every
  process, and `--debugmsg +debugstr` surfaces the `OutputDebugString`
  copies in a terminal run.
- **Input arrives but rough**: pedals inverted, which is pedals resting at
  max, phantom throttle and clutch, the in-game wheel drifting from the
  real one, steering sometimes ignored. Three suspects, none yet tested:
  Assetto Corsa has never been through its per-axis setup wizard for this
  device; Steam Input may be re-exporting the wheel as its own virtual pad
  on top of it, since Steam runs inside the bottle; and the SDL-synthesised
  descriptor is the low-fidelity route, with the B10 `Hidraw` knob as the
  A/B against the wheel's own descriptor.

**A36. The proxy missed the daemon by one path, and its new log caught the
bug on its first run.** Test 17's `proxy.log`, three times over:

```
t150-dinput8: no endpoint at Z:\Users\crossover\Library\Application
Support\t150ffb\endpoint, staying out of the way
```

The proxy built its default endpoint path from the bottle's `USERNAME`,
and a CrossOver bottle names its Windows user `crossover` whoever owns the
Mac, so every bottle's proxy looked in a home that does not exist while
the daemon published under the real one. The proxy behaved exactly as
designed around a wrong assumption of its own: nothing was wrapped, the
game was left alone, and without `T150_LOG` this would have read as yet
another silent failure. Fixed by checking the guess against the
filesystem and then trying every home under `Z:\Users`; with a 0.1.3 or
older proxy, `T150_ENDPOINT` set explicitly does the same.

**The `Hidraw` A/B came back in favour of the wheel's own descriptor.**
With the B10 knob on, "minus problems as before": the phantom
acceleration is gone except in neutral, and steering stays consistent.
Pedals remain inverted, which is the game's own axis setup, still never
run. One observation recorded with a question mark rather than a
conclusion: under the hidraw route the tester reports "no ffb no
autocenter", a limp wheel, and nothing known sends the input-open on that
path; it may be a leftover open from the session's earlier tools, and the
next session with the daemon connected makes it moot, since the daemon
owns the input state.

The end to end join is now blocked by nothing measured: the wheel is in
the bottle, the proxy runs in the game and logs what it does, the daemon
drives the wheel, and the one wrong path between them is fixed.

**A37. The pedals are labelled backwards in the wheel's own descriptor,
and the hidraw route delivers every button correctly.** Test 19, one
pedal at a time under `probe_intr -R`, hands off the rim:

- accelerator: report bytes 5 and 6, the second pedal field, usage
  **Rz**, DirectInput axis 2
- brake: report bytes 3 and 4, the first pedal field, usage **Y**,
  DirectInput axis 1
- hands off: "nothing ever changed"

`joy.cpl` inside the bottle shows the same, axis 1 brake and axis 2
accelerator, so the mapping survives to DirectInput unchanged. The common
convention games and presets are built for, the G29 family and the
corrected T300RS layout hid-tmff2 ships, is the opposite: **Y gas, Rz
brake**. So a stock game brakes on the accelerator and reads the brake,
resting at 1023, as a throttle held open, which is tests 16 and 17's
"accelerates alone" and "accelerator is brake" in one mislabel. On
Windows the vendor driver relabels the fields; in the bottle the proxy
now does, swapping the Y and Rz values in the stock joystick data formats
and relabelling buffered events to match. `T150_PEDALS=raw` turns it off,
and a custom data format is left untouched with a log line saying so.

**The buttons are whole.** On the hidraw route the tester named every
one: both paddles, the PS-layout face buttons, Share and Options, R2, L2,
L3, R3, PS, and the hat "good". A21's loss, open since the first session,
was an SDL-path casualty; the wheel's own descriptor carries all thirteen
into the bottle correctly.

**A38. The join carried live force feedback, and the pedals need the
direction fix too.** Test 20, with the 0.1.5 proxy confirmed in the
bottle.

- **The proxy connected to the daemon and wrapped the wheel in Assetto
  Corsa**, three times across two daemon instances, the last on the live
  daemon's port, and the tester's screen recording shows the game's own
  force feedback meter alive and swinging between roughly 5 and 65
  percent while driving. The game generated effects into a wrapped,
  daemon-connected wheel. Whether the wheel pushed back in the tester's
  hands went unrecorded, which is the one question left.
- **The pedal symptoms decode as the swap working against a stale game
  config.** Assetto Corsa's `controls.ini` still carried the pre-swap
  assignment, throttle on the Rz object and brake on the Y object, so the
  swap re-crossed them: the game's throttle read the physical brake,
  resting at max, "release and it accelerates", and the physical
  accelerator fed the game's brake, "press it and the car stops". The
  recording's pedal bars agree, the brake bar never lighting and the
  throttle bar full at rest.
- **Rest-at-max is a fault of its own.** With bindings refreshed the car
  still drives itself, so the game does not rest-calibrate, and the
  earlier read that direction was not the problem is withdrawn: the
  perception changed with the config underneath it. The proxy now mirrors
  the pedal values inside the game's own axis range, tracked from
  `DIPROP_RANGE`, so a released pedal reads zero. `T150_PEDALS` accepts
  `raw`, `swap` or `invert` for less than both corrections.
- **Two proxy builds were indistinguishable in the log**, which cost this
  test its interpretation until the tester confirmed the version by hand.
  The proxy now logs `git describe` and its pedal settings at wrap time,
  and CI fetches tags so release builds stamp a real version.

After the DLL update, the game's pedals must be reassigned once, since
every stored binding predates the corrected layout.

**A39. The 0.1.6 inversion default was wrong twice over, and is
withdrawn.** Within an hour of release a tester had both pedals showing
pressed at rest, the brake dead and the throttle backwards, a clean
regression from 0.1.5.

- **The mirror used the wrong range.** It mirrored inside ranges shadowed
  from the game's `SetProperty(DIPROP_RANGE)` calls, tracked for
  device-wide and by-offset addressing but not by object id, which is how
  this game addresses its axes. The mirror then reflected real values
  inside a stale default range and produced values outside the axis
  entirely, clamped at the pressed end: both pedals full at rest, exactly
  as reported. Fixed by asking DirectInput for the axis's effective range
  instead of shadowing, `GetProperty(DIPROP_RANGE)` by offset, cached and
  invalidated on any range change, which no addressing mode can bypass.
- **The default was built on a misread.** A38 took "it accelerates alone"
  as evidence that games do not handle the rest position; the regression
  shows this game corrects pedal direction itself when the pedals are
  bound fresh, so inverting under it re-breaks what the game fixed. The
  earlier report is better explained by stale bindings than by a missing
  mirror.

So the default is the swap alone, the one correction measured beyond
doubt, and the mirror is opt-in: `T150_PEDALS=full` for a game that does
not handle direction itself, `raw` and `invert` for diagnosis. The A/B
that should have preceded the 0.1.6 default is now the procedure: bind
fresh on the swap default, and only if the car still drives itself with
feet off, switch to `full` and bind fresh again.
is above the USB layer.** A18 is answered, and against the wheel.

`probe_intr -R` read the interrupt IN pipe with the device captured. The mask
of bits that ever moved lines up with the report descriptor field for field,
which is also the strongest evidence available that the tool is right:

| Bytes | Field | Mask | |
| --- | --- | --- | --- |
| 0 | report id `0x07` | `00` | constant, as declared |
| 1-2 | X, 16-bit | `ff ff` | steering |
| 3-4 | Y, 10-bit | `ff 03` | pedal, to its logical maximum |
| 5-6 | Rz, 10-bit | `ff 03` | pedal |
| 7-8 | slider, 10-bit | `00 00` | no third pedal on a stock T150 |
| 9-10 | padding | `00 00` | constant, as declared |
| 11 | buttons 1-8 | `ff` | all eight |
| 12 | buttons 9-13 | `1c` | plus its three padding bits at zero |
| 13 | padding | `00` | constant, as declared |
| 14 | hat | `0f` | four bits |

Three runs cross-validate it. The first worked every button except the
paddles and the PS button and reported buttons 3 to 12. The second added
exactly those controls and reported buttons 1, 2 and 13 on top, so the paddles
are buttons 1 and 2 and PS is button 13. A third worked everything at once and
returned byte 11 `ff`, byte 12 `1f`: **all thirteen buttons in a single run**,
with byte 12's three padding bits still at zero.

Meanwhile CrossOver's controller panel registers no button at all, in either
its DirectInput or its XInput view. The wheel is therefore fine and this is
an input path problem in macOS HID, SDL or winebus. B8 is where to look, and
it is independent of force feedback.

**And it may be worse than buttons.** The most recent report is "no button or
axis functional in crossover game controllers menu with DInput and XInput",
where earlier ones said the wheel was listed and its axes worked. Nothing
here explains the difference and the wheel had been through several captures
and a mode switch by then, so it needs a clean check before it is treated as
a fact: replug the wheel, switch it with `probe_intr -I`, touch nothing else,
and open the panel.

**A22. Gain has no perceptible effect on its own, which is expected.**
Measured: `43 80` and `43 00` on the interrupt OUT pipe felt the same. Gain
scales force feedback effects, and no effect has ever rendered, so there is
nothing for it to scale. This is not evidence that the opcode is wrong, and
it will only become a real test once something moves the wheel.

Everything below was written when the wheel was believed to be at fault, and
is kept because the reasoning still holds if the wheel turns out not to obey
settings.

**The wheel stays blocked, as of before that initialisation existed.**

Put the whole record together and it does not describe a protocol problem:

- It calibrates, sweeping under its own power, so the belt and gear train
  are not jammed and the motor drives.
- It is blocked in boot mode, in firmware mode, and in the PS4 position where
  nothing from this project reaches it.
- It is blocked on a stock Linux machine as well as on macOS.
- Writes through the HID layer and writes on the interrupt OUT pipe, the one
  the Linux driver uses, both leave it blocked.

A wheel that drives itself through a full sweep and then cannot be turned by
hand is not being held by anything a host has said to it, because in the PS4
position no host has said anything at all. **Nothing further can be learned
about the protocol from this wheel in this state**, and no result obtained
from it should be treated as evidence about E1 in either direction.

**The one test never yet run separates the two remaining explanations.**
Pull the mains lead and leave USB connected: the motor rail comes from mains
and the logic rail from USB, so with mains out the motor cannot hold
anything. If the wheel becomes turnable, the hold is being produced
electrically and the hardware is sound. If it stays immovable with no motor
power, the cause is mechanical and nothing in software will ever address it.

After that, the reference test is the wheel on a Windows machine with
Thrustmaster's own driver, or on Linux with `scarburato/t150_driver`
installed. Those are the only setups known to drive this model, and if the
wheel stays blocked under one of them the answer is the wheel.

Nothing in the project should be changed until that is known. The transport
facts stand on their own either way, because none of them depend on the
motor: the descriptors read, the endpoint 0 path works unprivileged,
`setReport` is accepted, no node is protected and CrossOver does not seize.

> Thrustmaster T150 help centre, sections 1.1, 2.1 and 2.2, plus the PS4
> position observation.

> Measured. Boot mode publishes an unnumbered 8-byte vendor output report
> (usage `0x2621`) and a matching feature report, not the id `0x0A` 14-byte
> report C5 decodes from a firmware 3.5 capture. That descriptor describes
> firmware mode, which this wheel has not yet been switched into.

**A41. Correcting the pedals by default was wrong, and the tester's own
history says so.** Reported after 0.1.7: the car still accelerates by
itself and the brake does nothing, and "it was working better yesterday
morning."

Yesterday morning is test 17, and what it ran was the hidraw route with a
proxy from before the swap existed. Its verdict then was the best this
project has had: "minus problems as before... no acceleration without i
press buttons except in N position." Test 20's capture of Assetto Corsa's
`controls.ini` explains why. It reads `THROTTLE AXLE=2` and `BRAKES
AXLE=1`, which against the raw wheel is Rz for the accelerator and Y for
the brake, exactly what A37 measured. **The game was already correct**,
because it binds pedals by asking the player to press them, and a
detection binding resolves the labels and the direction together without
knowing or caring what the descriptor claims.

So each correction this project added underneath re-crossed a correct
binding. 0.1.5's swap moved the brake onto the axis the game reads as
throttle, and a released brake rests at maximum, which is the car
accelerating by itself. 0.1.6's mirror compounded it. 0.1.7 removed the
mirror and kept the swap, which is why the symptom survived.

The lesson is the same one A39 recorded, one layer up, and it is now taken
properly: **the proxy forwards input untouched by default.** It is a force
feedback proxy, its own file header says everything else is forwarded
straight through, and twice it has broken a working setup by not meaning
it. `T150_PEDALS` keeps swap, invert and full for a game that assumes the
common convention and offers no way to rebind, which is a real case and a
different one.

A37 stands as a measurement: the descriptor really does label the pedals
against the convention. What was wrong was concluding that this project
should be the one to fix it, unasked, for every game.

**Unrelated, and worth recording so it is not chased twice.** The same
report mentions a warning about 100 percent CPU. That is Assetto Corsa's
own "CPU OCCUPANCY > 99%" banner and it is visible in every frame of the
video recorded during test 20, before any of these changes, so it is the
game under CrossOver on Rosetta rather than anything this project does.
The proxy's per-poll work is a two-value swap, and its log file is written
at connect and wrap only, six lines in a whole session.

**A40. Thrustmaster's own Windows driver, decompiled, confirms the encoders
and corrects four things.** The T150's Windows package, `2026_TTRS_1.exe`,
is an InstallShield v31 installer whose payload splits into a header
cabinet and two data volumes; `unshield` extracts 252 files from it. The
force feedback stack for `USB\VID_044f&PID_b677` is `tmPID64.DLL`, a COM
in-proc server implementing `IDirectInputEffectDriver`, over `tmHidUsb.sys`.

**The find that made the rest possible is a report descriptor.**
`tmHidUsb.sys` carries an 891-byte HID **Physical Interface Device**
descriptor at file offset `0x81670`, opening `05 0f 09 21 a1 02 85 0b`.
The wheel exposes no PID collection itself, which is this project's founding
measurement; Thrustmaster keeps one driver-side and it names every field
this project reverse engineered. Its report lengths are 15, 9, 4, 8, 11 and
4, exactly our `T150_FF_COMMIT_LEN`, `FIRST_LEN`, `UPDATE_LEN_CONSTANT`,
`_PERIODIC`, `_CONDITION` and `CONTROL_LEN`, and its report ids 0x0b to 0x0f
map to our wire classes 0x01 to 0x05 by the driver's own `sub dl,0xa`.

**Confirmed, first-hand rather than transcribed.** The four divisors this
header cites from the Linux driver appear as literal constants in the
vendor's DLL, `mov edx,0x147`, `mov edx,0x30c`, `mov edx,0x28f` and
`0x1ff`. The descriptor independently declares the coefficient as -100 to
100, the centre as -500 to 500, the deadband as 0 to 1000, the phase as 0
to 255, the damper saturation as 0 to 100 and the gain as 0 to 0x80 against
10000. Every packet length and class byte matches. So do the condition
bytes: `05 1c 00 00 00 00 00 00 00 46 54` for a spring and `64 64` for a
damper come out of our encoder byte for byte, and `tests/encode_check.c`
now pins them to the vendor's captures.

**Corrected, four of them.**

- **Start delay is a whole 16-bit millisecond field**, not the high byte of
  one. This project read the Linux driver as making the unit 256 ms, so
  every delay under 25.6 seconds was sent as zero. The descriptor declares
  16 bits and `multiple_saw_down.pcapng` carries a 100 ms delay as
  `01 05 24 40 88 13 00 00 00 9a 00 a8 00 64 00`.
- **Envelope levels run to 127**, not 255. `09 5b 25 7f 75 08` for attack
  level and `09 5d 25 7f 75 08` for fade level, logical maximum 127 against
  a physical maximum of 10000. Our 0xff was a guess and this header said so.
- **Square is `0x4020` and triangle is `0x4021`, both native.** The
  descriptor's effect type array is constant, square, triangle, sine,
  sawtooth up, sawtooth down, spring, damper, and the driver indexes a
  nine-byte table with that position for the low byte: `00 00 20 21 22 23 24
  40 41`. Six of the eight were already ours, under an ordering that is not
  numeric and could not have been guessed. Both downgrades are removed. This
  also settles A34's puzzle: `0x4025` renders nothing because the table has
  no entry for it.
- **The slot keys are sixteen bits.** They are parameter block offsets, 28
  bytes per slot, declared report size 16 and stored by the driver as words.
  Ours returned `uint8_t`, which is invisible through slot 8, the highest
  any capture reaches, and wraps from slot 9: 0x11a became 0x1a, a slot 0
  key, so two live effects would have addressed the same block.

**Explained rather than changed.** The constant level stopping at 64 while a
periodic reaches 127 is not a field limit: both are one signed byte. The
vendor halves constant force on this model before it reaches the wire, and
`constant0.pcapng` steps 0x40, 0x20, 0x10 for 100, 50 and 25 percent. Our
64 reproduces the vendor's bytes and stays.

**Also corrected in the model, not the bytes.** The 11-byte condition
packet's last two bytes are not a trailer keyed to the effect type: they are
the positive and negative saturation of the parameter block the packet
addresses, and a condition is two such packets rather than an envelope plus
a trailer. `damper0.pcapng` shows the pair varying across slots, which no
fixed trailer could do. The emitted values are unchanged.

**Left alone deliberately.** The vendor converts a DirectInput ramp into a
sawtooth periodic and folds per-effect gain into magnitudes; this project
slices a ramp into constants and keeps gain separate because the wheel has
one device gain. Both are defensible and neither is a defect. Spring
saturation is the one genuine unknown left: the descriptor declares 0 to
100 for both conditions, while the vendor's spring captures never exceed
0x54 and its damper captures reach 0x64, so our asymmetric maxima
reproduce observed traffic and may still be a driver-side ratio rather than
a firmware limit.

> `2026_TTRS_1.exe`, InstallShield v31, extracted with `unshield` built from
> source. `FFB_Drivers_Win10/amd64/tmPID64.DLL` and `tmHidUsb.sys`, the
> latter's descriptor at file `0x81670` and effect type table at `0x81548`.
> Captures as in A40's vectors. The exercise ran 93 agents and refuted 52 of
> 86 candidate findings; what survived was re-verified by hand before any
> code changed.

---

## B. How CrossOver handles HID and force feedback

Verified against CrossOver 26.3.0's own published sources, not only upstream
Wine.

**B1. The macOS HID backend is a raw pass-through.** `bus_iohid.c` implements
`raw_device_vtbl`, not `hid_device_vtbl`. It copies the device's real report
descriptor out of `kIOHIDReportDescriptorKey` and hands it to the bottle
unchanged. But see B8: for this wheel that backend's device is normally
discarded before the bottle ever sees it.

> `dlls/winebus.sys/bus_iohid.c`, `iohid_device_vtbl` and
> `iohid_device_get_report_descriptor()`.

**B2. It does support output reports, straight to IOKit.**
`iohid_device_set_output_report()` is a direct call to
`IOHIDDeviceSetReport(impl->device, kIOHIDReportTypeOutput,
packet->reportId, packet->reportBuffer, packet->reportBufferLen)`.

> Same file. Feature reports both ways are implemented the same way.

**B3. It implements no force feedback of its own.** None of
`haptics_start`, `haptics_stop`, `physical_device_control`,
`physical_device_set_gain`, `physical_effect_control` or
`physical_effect_update` appear, and it never calls
`hid_device_add_physical()`. The Linux evdev backend does exactly the
opposite: `lnxev_device_vtbl` is a `hid_device_vtbl` and synthesises a PID
descriptor from evdev capabilities. This is true of the file and irrelevant
to the outcome, because of B8 and B9.

> `bus_iohid.c` versus `bus_udev.c:1096` and `bus_udev.c:626`.

**B4. DirectInput derives force feedback capability purely from a PID
descriptor.** `guidFFDriver` is set only when
`HidP_GetSpecificButtonCaps(HidP_Output, HID_USAGE_PAGE_PID, 0,
PID_USAGE_DC_DEVICE_RESET, ...)` returns a hit, and `DIDC_FORCEFEEDBACK` only
when a PID Device Control Report collection was found. A wheel whose
descriptor has no PID collection gets no force feedback, and there is no
other path.

> `dlls/dinput/joystick_hid.c`, `hid_joystick_device_try_open()` and
> `hid_joystick_create()`.

**B5. `dinput8.dll` is a self-contained PE with no unix library.** It is a
full build of the `dinput` sources (`PARENTSRC = ../dinput`) importing only
PE modules. This is what makes the proxy approach mechanically possible: a
renamed copy can be loaded as an ordinary PE.

> `dlls/dinput8/Makefile.in`.

**B6. Seizing breaks everything else.** macOS 26 added an `fClientSeized`
check to `setReport`: the moment any other process seizes the device, every
`setReport` from a non-seizing client returns `kIOReturnExclusiveAccess`. Not
seizing yourself is not sufficient protection, and seizing yourself would
take input away from CrossOver.

> IOHIDFamily rel/IOHIDFamily-2238,
> `IOHIDFamily/IOHIDLibUserClient.cpp:1994-1996`; `open()` sets
> `fClientSeized = ret == kIOReturnExclusiveAccess`. This check is absent
> from older IOHIDFamily releases, so older sources are misleading here.

**B7. setReport requires the console user.** Its `fValid` gate is
`clientHasPrivilege(fClient, kIOClientPrivilegeConsoleUser)`. Fast user
switching, the login window, or an SSH-only session therefore turn every
write into `kIOReturnNotPermitted`.

> Same file, `resourceNotificationGated()`. Practical consequence: the write
> path cannot be tested from a headless or SSH session, and macOS CI cannot
> cover it.

**B8. On macOS the wheel does not reach the bottle through `bus_iohid.c`.**
winebus creates one device per backend and then arbitrates between them.
`bus_iohid.c` marks everything it creates `is_hidraw = TRUE`, and on
`BUS_EVENT_TYPE_DEVICE_CREATED` the main loop removes any device whose
`is_hidraw` flag disagrees with `is_hidraw_enabled()`. For a Generic Desktop
joystick or gamepad that function returns a `prefer_hidraw` default of FALSE
unless the VID:PID is on a hardcoded list or in the `EnableHidraw` registry
value. The T150's `044f:b677` is on neither: the Thrustmaster entries are the
T-Rudder `b679`, the TWCS Throttle `b687` and the T.16000M `b10a`. `Enable
SDL` defaults to 1. So the IOHID instance is discarded and the SDL one is
what the bottle sees.

> `dlls/winebus.sys/main.c`, `bus_options_init()`, `is_hidraw_enabled()` and
> the `BUS_EVENT_TYPE_DEVICE_CREATED` case; `bus_iohid.c`, the
> `.is_hidraw = TRUE` initialiser. Checked against Wine master.

**B9. So the absent PID collection is SDL's doing, not the descriptor's.**
`bus_sdl.c` is a `hid_device_vtbl` and does synthesise a PID collection, but
only inside `if (impl->effect_support & EFFECT_SUPPORT_PHYSICAL)`, and
`descriptor_add_haptic()` sets `effect_support` to 0 whenever
`SDL_JoystickIsHaptic()` is false or `SDL_HapticOpenFromJoystick()` fails.
SDL's macOS haptic backend is `ForceFeedback.framework`, which by D1 only
sees devices whose driver published an `IOCFPlugInTypes` plug-in, so a wheel
with no macOS driver is never haptic there.

Worth noticing what this means: winebus already passes `force = TRUE` for a
device whose usage is `HID_USAGE_SIMULATION_AUTOMOBILE_SIMULATION_DEVICE`,
which declares every `PID_USAGE_ET_*` effect type rather than only the ones
SDL reports. The descriptor machinery is present and willing. It is the
capability query underneath it that returns nothing.

> `dlls/winebus.sys/bus_sdl.c`, `descriptor_add_haptic()` and its wheel
> caller; SDL `src/haptic/darwin/SDL_syshaptic.c`, the `FFIsForceFeedback()`
> gate.

B4 still decides the outcome and the design is unaffected, because the proxy
sits above DirectInput and never reads a descriptor. One practical
consequence though: putting `044f:b677` in `EnableHidraw` routes the wheel
back through `bus_iohid.c`, and the bottle then sees the wheel's own
descriptor instead of SDL's synthesised one. That is an input fidelity knob,
not a force feedback fix. Since A34, that knob has become the leading
candidate fix for the whole input problem: see B10.

**B10. CrossOver 26.3.0's own winebus, read from CodeWeavers' published
source, and why a missing wheel means the SDL chain.** A34 measured the
wheel absent from the bottle entirely, which B8 and B9, researched against
upstream Wine, did not predict. CodeWeavers publishes CrossOver's modified
Wine tree, and 26.3.0's `dlls/winebus.sys/` says the following.

- **CrossOver runs four buses, not three.** SDL, udev, iohid, and a
  CodeWeavers-only `bus_xbox360.c` (2019, Aric Stewart), an IOKit USB
  backend for Xbox pads. Xbox pads therefore never depend on the SDL bus,
  and DualShock and DualSense ride the iohid bus through a hidraw
  allowlist. **A dead SDL chain blanks exactly one class of device: the
  generic HID joystick, which is what a T150 is.** Every controller a
  typical user owns keeps working, which is how a broken SDL bus stays
  unnoticed.
- **The arbitration matches upstream.** `is_hidraw_enabled()` has the same
  Thrustmaster allowlist as upstream, `b679`, `b687`, `b10a`, not `b677`,
  so the wheel's iohid copy is discarded and the SDL copy is the only one
  the bottle can receive. No CrossOver-specific filter drops it:
  `sdl_add_device()` treats wheels as plain joysticks, and deliberately so,
  with an explicit `joystick_type != SDL_JOYSTICK_TYPE_WHEEL` guard keeping
  wheels out of the game-controller mapping.
- **So the fault is in the SDL chain**: winebus's `dlopen` of CrossOver's
  own `lib64/libSDL2-2.0.0.dylib`, `SDL_Init`, or SDL's macOS HID
  enumeration inside the bottle's processes, which carry CrossOver.app's
  TCC identity rather than Terminal's, where every working tool in this
  project runs. B11 names the leading suspect inside that chain, read out
  of the exact SDL version CrossOver ships.

Two experiments decide it, both cheap:

1. **Make winebus talk.** Quit CrossOver fully, then start the bottle from
   a terminal with `--debugmsg +winebus` and the wheel plugged in, and run
   `control.exe joy.cpl` for DirectInput's view. The trace prints the SDL
   bus starting or failing to load libSDL2, and either
   `creating non-hidraw device 044f:b677` or nothing at all. One log names
   the broken link.
2. **Reroute around SDL.** `HKLM\System\CurrentControlSet\Services\WineBus\
   Devices\044f/b677` with DWORD `Hidraw` = 1, read at bottle boot, sends
   the wheel through `bus_iohid.c` instead, which passes the wheel's own
   descriptor into the bottle, buttons and all (B1, B8). If the wheel then
   appears, the input problem is solved better than SDL ever solved it,
   force feedback unaffected since the proxy sits above DirectInput. If it
   does not appear through IOHID either, the fault is process-level, TCC
   or the device, not SDL.

> `sources/wine/dlls/winebus.sys/` in
> `crossover-sources-26.3.0.tar.gz` from media.codeweavers.com:
> `main.c` `bus_options_init()`, `load_device_options()`,
> `is_hidraw_enabled()`, the `IRP_MN_START_DEVICE` case starting all four
> buses; `bus_sdl.c` `sdl_bus_init()`, `sdl_add_device()`;
> `bus_xbox360.c`. The shipped Mac package carries
> `lib64/libSDL2-2.0.0.dylib` and the winebus allowlists verbatim.

**B11. The suspect, verified in the shipped bits and then by experiment:
SDL 2.30.12's HIDAPI layer claims Thrustmaster devices and the IOKit
backend then skips them.** Test 16 confirmed it on hardware:
`SDL_JOYSTICK_HIDAPI=0` put the wheel back in the bottle (A35). CrossOver 26.3.0's `libSDL2-2.0.0.dylib` identifies itself as
`SDL-release-2.30.12`, extracted from CodeWeavers' own Mac package and read
directly. In that exact release, every link of the following chain is in
the source:

- `SDL_HIDAPI_DEFAULT` is `SDL_TRUE` on macOS, and winebus sets no hint
  against it, so SDL's HIDAPI joystick drivers are active inside the
  bottle.
- `HIDAPI_SupportsPlaystationDetection()` in 2.30.12 has
  `case USB_VENDOR_THRUSTMASTER: return SDL_TRUE;`. The exclusion for
  Thrustmaster, with its comment "Most of these are wheels", exists only in
  SDL releases after 2.30.12. CrossOver ships the last version that still
  probes them.
- The PS3 third-party HIDAPI driver therefore claims a T150 at enumeration,
  "might be supported by this driver, enumerate and find out", and probes
  it with a Sony third-party feature report query, against a wheel whose
  descriptor declares a one-byte feature report.
- Meanwhile the IOKit joystick backend's device-arrival callback finds
  `HIDAPI_IsDevicePresent()` true, comments "The HIDAPI driver is taking
  care of this device", and returns without adding it. When HIDAPI's probe
  then rejects the wheel, no new arrival event replays, so the wheel ends
  on no SDL path at all, which is what winebus sees and what A34 measured.

The static links are all read from source; the dynamic ordering, HIDAPI's
claim landing before IOKit's callback, is the one inferred step, and the
env-var test below settles it on hardware.

**The sibling wheel confirms it from the outside.** Akellacom's T300RS
driver, the same project C7 cites, instructs launching CrossOver with
`SDL_JOYSTICK_HIDAPI=0`, "forces SDL to use IOKit so the wheel appears in
DirectInput", and its troubleshooting entry for a missing or wrong wheel is
that variable being forgotten. An independent project hit this exact
mechanism on the T300RS and shipped the workaround.

Their CrossOver mode, read from `main.swift` rather than the README, sends
exactly three packets before idling: the T300RS force feedback open
`60 01 05`, the range and the gain, left on the wheel through the same
persistence A30 measured on the T150. The baseline spring and damper and
the telemetry-driven effects run only in their native capture mode, which
needs SIP and AMFI disabled for the virtual device, the path D-section
rejected. What force feedback their CrossOver users get from games is the
subject of B12, which corrects an overstatement this entry briefly made.

So the experiment order for the bottle, cheapest and most likely first:

1. `SDL_JOYSTICK_HIDAPI=0` in the bottle's environment. Proven on the
   sibling wheel. It costs HIDAPI-based Bluetooth pad rumble in that
   bottle, which is why winebus set the PS4 and PS5 rumble hints; the
   narrower `SDL_JOYSTICK_HIDAPI_PS3=0` would spare those and is the
   refinement to try once the broad form is proven.
2. The `Hidraw` knob from B10, which bypasses SDL entirely and carries the
   wheel's own descriptor, buttons included, so it may fix A21 too.
3. The `+winebus` trace, if neither works, to see what the bus actually
   said.

> The dylib: `lib64/libSDL2-2.0.0.dylib` in `crossover-26.3.0.zip`, string
> `SDL-release-2.30.12-0-g8236e01a9`. SDL 2.30.12:
> `src/joystick/hidapi/SDL_hidapijoystick.c`
> `HIDAPI_SupportsPlaystationDetection()`,
> `src/joystick/hidapi/SDL_hidapi_ps3.c`
> `HIDAPI_DriverPS3ThirdParty_IsSupportedDevice()`,
> `src/joystick/darwin/SDL_iokitjoystick.c` the `HIDAPI_IsDevicePresent`
> skip, `src/joystick/hidapi/SDL_hidapijoystick_c.h` `SDL_HIDAPI_DEFAULT`.
> Akellacom/thrustmaster_t300rs_gt_macos_driver README, the CrossOver
> section and its troubleshooting entry; its `Sources/ThrustmasterWheel/
> main.swift` for what the CrossOver mode actually sends.

**B12. CrossOver on macOS does deliver game force feedback, for hardware
that brings a PID descriptor, and the chain is already verified piecewise
in this file.** An earlier draft of B11 said Wine force feedback on macOS
"is none". That is wrong as a general statement, and the correction
matters because it explains CodeWeavers' hidraw allowlist and strengthens
the B10 knob.

The chain, every link measured or read from CrossOver 26.3.0's source:

- `bus_iohid.c` passes the device's own report descriptor into the bottle
  unchanged (B1).
- `bus_iohid.c` implements `set_output_report` as a straight
  `IOHIDDeviceSetReport(kIOHIDReportTypeOutput, ...)`, verified again in
  the 26.3.0 tree, so what a game writes reaches the hardware (B2).
- DirectInput derives force feedback capability purely from a PID
  descriptor and speaks PID output reports to it (B4).

So a wheelbase whose firmware carries a real PID collection, routed
through the hidraw bus, gets **native, untranslated DirectInput force
feedback** in CrossOver on macOS: dinput builds PID reports, hidclass
hands them to winebus, `bus_iohid` writes them with the same call this
project's daemon uses. That is why `is_hidraw_enabled()`'s allowlist reads
like a sim rig catalogue, four Simucube models, Fanatec ClubSport pedals,
VKB sticks: those are PID devices, and hidraw is CodeWeavers' supported
path for them.

Three consequences:

- **"Whatever FF Wine supports" on macOS means: full pass-through for PID
  hardware, nothing for descriptor-less wheels.** The T150 declares no PID
  collection, which is this project's founding measurement, so it can
  never ride that path, and neither can the T300RS. The proxy and daemon
  remain the only route for them, and the niche is real but narrower than
  "no force feedback in CrossOver on macOS": it is force feedback for
  wheels PID left behind.
- **The B10 `Hidraw` knob is not a hack but CodeWeavers' own wheel path**,
  missing only the allowlist entry for `044f:b677`. Which also suggests
  the durable fix to offer upstream: CodeWeavers adding the T150 to the
  same list its T-Rudder and T.16000M siblings are already on.
- **If Thrustmaster firmware ever exposed a PID mode, this project would
  be obsolete for it.** It does not; that is D6 and C5's territory and
  nothing new.

> `sources/wine/dlls/winebus.sys/bus_iohid.c`,
> `iohid_device_set_output_report()`; B1, B2, B4 for the measured links;
> `main.c` `is_hidraw_enabled()` for the allowlist.

---

## C. How these wheels are actually driven

**C1. Logitech force feedback is a plain HID output report.** `new-lg4ff`
fills a 7-byte report and calls `hid_hw_request(entry->hid, entry->report,
HID_REQ_SET_REPORT)`. It validates the device has a 7-byte output report
first.

> berarma/new-lg4ff `hid-lg4ff.c:493`, `:511`, `:2299`.

**C2. The T300 family also goes through the HID layer.** `hid-tmff2`'s
`t300rs_send_buf()` fills the report's field values and ends in
`hid_hw_request(t300rs->hdev, t300rs->report, HID_REQ_SET_REPORT)`. Vendor
control transfers are used only for mode switching, firmware version and
attachment detection.

> Kimplul/hid-tmff2 `src/tmt300rs/hid-tmt300rs.c:457-474` and
> `t300rs_switch_mode()`.

**C3. The T150 does not.** Its Linux driver bypasses the HID layer entirely
and writes raw on the interrupt OUT pipe. `grep` for `hid_hw_request` across
the whole driver returns nothing.

> scarburato/t150_driver `hid-t150/settings.c:18`, `:137`,
> `forcefeedback.c:32`, `input.c:27`; pipe built at `hid-t150.c:77` with
> `usb_sndintpipe()`. Its own `traffic/old_caps/t150_test.py` does the same
> from userspace with libusb, claiming interface 0 and writing to `0x01`.
>
> **This was the single biggest risk in the project, and A19 retired it.** C2
> proves Thrustmaster firmware can accept HID output reports, C3 meant nobody
> had demonstrated that the T150's does, and now someone has.

**C4. On Linux, a SET_REPORT on an output report goes out the interrupt OUT
endpoint** when the device has one, falling back to the control pipe only
when it does not. So C1 and C2 are interrupt OUT traffic, framed as HID.

> linux `drivers/hid/usbhid/hid-core.c`, `__usbhid_submit_report()`.

**C5. The T150 declares a usable output report.** Report id `0x0A`, vendor
usage page `0xFF00`, 14 bytes, `Output (Data, Var, Abs)`. So
`IOHIDDeviceSetReport` has something valid to address, even though C3's
driver never uses it.

> `t150_driver/traffic/old_caps/hid_report_fw35`, bytes
> `85 0A 06 00 FF 09 0A 75 08 95 0E 26 FF 00 46 FF 00 91 02`. Firmware 3.5,
> not current hardware.

**C6. The boot to firmware switch is two ep0 vendor control transfers.**
`bmRequestType 0xC1 / bRequest 73` to read model and attachment, then
`0x41 / 83` with a per-model switch value.

> linux `drivers/hid/hid-thrustmaster.c`. Both are recipient = interface with
> `wIndex` 0, that is, directed at the interface macOS's HID driver owns.

**C7. A macOS implementation reports that Thrustmaster firmware ACKs the
control SET_REPORT and ignores it. Measured false for the T150, see A19.**
It held the project's architecture hostage for six sessions and it does not
apply to this wheel: `IOHIDDeviceSetReport` was later shown to change the
T150's autocenter, from an ordinary unprivileged process, with CrossOver
free to keep reading the wheel.

The reason it looked true is recorded in A15: every HID attempt until then
sent `-A`, the autocenter enable flag, which is a no-op on macOS by design.
The transport was never the thing that was broken.

What follows is kept as written, because it is still an accurate account of
a T300RS implementation and of why the hypothesis was reasonable.

`Akellacom/thrustmaster_t300rs_gt_macos_driver` is a userspace macOS driver
for the T300RS on Apple Silicon. Its wheel configuration code carries this
comment, and its implementation follows it:

> Linux hid-tmff2 sends via interrupt OUT (usbhid_submit_report to urbout).
> **SET_REPORT control pipe is ACKed but IGNORED by T300RS firmware.**
> macOS IOHIDDeviceSetReport clips data to original descriptor (62 bytes),
> but firmware needs 63 bytes (per Linux fixed descriptor).
> Solution: USBInterfaceOpenSeize to take interface from HID driver,
> write 63 bytes via WritePipe to interrupt OUT, then release.

So an independent implementation, on the same operating system, concluded
that `IOHIDDeviceSetReport` does not reach a Thrustmaster wheel's firmware,
and took the device away from the HID driver to write on the interrupt OUT
pipe instead. It also confirms E3's 62-byte clip, which until now rested on a
single second-hand report.

That was precisely the shape of what had been measured here at the time:
every write accepted, nothing ever happening. It looked like C3 arriving as
feared, one layer further out than expected. It was not. Every one of those
runs tried to release the wheel with `-A`, which cannot, so an obedient
firmware and a deaf one produced identical results. A15 and A19.

**Two things stop this being conclusive for this project.** It is a T300RS
finding, not a T150 one, and the T150's protocol and firmware differ. And the
project has not itself demonstrated which pipe `IOHIDDeviceSetReport` uses on
macOS 26; the claim that it is the control pipe is Akellacom's, inferred from
behaviour rather than from Apple's sources, which are closed.

It was the best available explanation of A8 at the time, and it was wrong.
A19 measured `IOHIDDeviceSetReport` moving this wheel, and A8's real cause
was `-A`, which is A15.

> `Sources/CUSBModeSwitch/CUSBModeSwitch.c`, the comment above
> `thrustmaster_configure_wheel()`, and its use of `USBInterfaceOpenSeize`
> and `WritePipe` throughout.

**And it does deliver force feedback**, which is worth stating plainly
because it proves the motor is drivable from macOS userspace at all. It does
so by two paths, and the difference between them is the whole question for
this project:

- **`thrustmaster_configure_wheel()`**, the path its CrossOver mode uses.
  Captures the device with `USBDeviceOpenSeize` and
  `USBDeviceReEnumerate(kUSBReEnumerateCaptureDeviceMask)`, writes a handful
  of 64-byte packets to the interrupt OUT pipe, then releases and
  re-enumerates so the HID driver reclaims the wheel. Its README calls this
  "configures the wheel then leaves it for Wine". Needs root, needs no SIP or
  AMFI change, and coexists with CrossOver because it does not stay.
- **`thrustmaster_usb_start()` and the `thrustmaster_ff_*` family**, marked
  "Direct USB I/O (replaces HID driver entirely)". Captures the device and
  keeps it, reading input on interrupt IN and writing effects on interrupt
  OUT. This is where spring, damper, constant, sine, play and stop live. It
  is continuous, game or telemetry driven, and the wheel belongs to it alone
  for the duration.

So force feedback on macOS is demonstrated, and the price is stated: the
second path owns the device. Its native and ETS2 modes additionally need SIP
and AMFI disabled, but only because they publish a virtual HID device, which
this project does not do (D2).

**What that means here, with the parts that have since been measured:**

- **The settings half of this project is unblocked, and more cheaply than
  this suggested.** Rotation range, gain and autocenter are exactly what
  `thrustmaster_configure_wheel()` sets, by briefly capturing the wheel and
  letting it go, and `t150ctl` could be built that way. It does not have to
  be: A19 showed the same settings work through `IOHIDDeviceSetReport` with
  no capture and no root at all.
- **The prediction written here was wrong, and expensively so.** It said
  sending `40 04 00 00` down the interrupt OUT pipe should release the
  autocenter and free the wheel. `0x04` is the enable flag and frees nothing;
  `40 03 00 00` is what does. Following it cost several sessions. See A15.
- **The game-driven half is still open, but not for the reason given.** The
  claim was that continuous writes need the interrupt OUT pipe held, and that
  holding it takes the wheel from CrossOver. The first half no longer follows
  now that the HID path is known to carry settings; whether it carries
  effects is untested, because no effect has been made to render on any pipe.
  A20 is where that stands.

**C8. The same project shows what a T-series wheel wants before it will
behave.** `T300RSProtocol.swift` carries a set of pre-initialisation packets
"sent to endpoint 1" which "MUST be sent before the mode switch to prevent
crashes":

```
42 01 00 00 00 00 00 00 00
0a 04 90 03 00 00 00 00
0a 04 00 0c 00 00 00 00
0a 04 12 10 00 00 00 00
0a 04 00 06 00 00 00 00
```

They go to the interrupt OUT endpoint, not through the HID layer, which is
the same conclusion as C7 reached from the other direction. Their model table
also lists the T150 as `0x0603`, the same two bytes the probes read here in
the other order.

This was the initialisation this project had never sent and had no route to
send. `probe_intr -I` now sends exactly these five packets before the mode
switch, in one capture, which is A13. Whether the T150 needs an
equivalent is unknown; that it exists for a sibling wheel makes the question
worth settling rather than assuming.

> `Sources/ThrustmasterWheel/T300RSProtocol.swift`, `T300RSInit.setupPackets`.

**C9. Reading a Thrustmaster wheel from macOS userspace is uncontroversial.**
`lockieluke/Thrustmaster4Mac` opens a T300RS with `hidapi`, explicitly
non-exclusively (`set_open_exclusive(false)`), reads pedal and wheel state,
and forwards it to an Assetto Corsa Lua plugin over a websocket. No force
feedback, no privilege, no capture.

It is worth noting only for the contrast: input from these wheels is easy and
needs nothing special, which is consistent with A1 and with this project's
whole premise. It is output that is the problem.

> `src/main.rs`.

---

---

## D. Dead ends. Do not re-explore these

**D1. Presenting a force feedback device to macOS.**
`ForceFeedback.framework` is a CFPlugIn architecture: it can only reach a
device whose *driver* published an `IOCFPlugInTypes` property (type UUID
`F4545CE5-BF5B-11D6-A4BB-0003933E3E3E`). Userspace cannot inject that
property, because `IORegistryEntry::setProperties` returns
`kIOReturnUnsupported` by default and `IOHIDDevice` does not override it.
The framework is not deprecated; the blocker is the missing plug-in, and
supplying one requires the kext or dext this project exists to avoid.

> `ForceFeedback.framework/Headers/IOForceFeedbackLib.h`; IOKitUser
> `IOCFPlugIn.c`; xnu `iokit/Kernel/IORegistryEntry.cpp`.

**D2. Virtual HID devices.** `IOHIDUserDeviceCreate` requires
`com.apple.developer.hid.virtual.device`, a restricted entitlement. Same wall
as DriverKit.

> Akellacom's macOS T300RS driver ships exactly that entitlement and its
> README states it bypasses the requirement by disabling SIP and AMFI.

**D3. SDL virtual joysticks as an injection point.** Refuted twice over.
CrossOver's `bus_sdl.c` contains zero references to `AttachVirtual`, so
winebus would not enumerate a virtual joystick even if one existed in
process; and SDL's virtual joystick registry is a process-local static
(`static joystick_hwdata *g_VJoys`), so no external program can inject one
into `winedevice.exe`.

> CrossOver 26.3.0 `dlls/winebus.sys/bus_sdl.c`; SDL `src/joystick/virtual/`.

**D4. `com.apple.vm.device-access` as a way to avoid root for USB capture.**
Restricted entitlements must be authorised by a distribution provisioning
profile, and a plain command line tool cannot carry one. Apple has separately
told the libusb project that this is not the right entitlement anyway.

> libusb wiki FAQ, quoted by maintainer mcuee in libusb issue #1851
> (2026-06-10); Apple, "Creating distribution-signed code for the Mac".

**D5. An in-bottle WDM bus driver, as the starting point.** It is not
impossible, and it would cover more than DirectInput 8, but five seams were
confirmed against CrossOver's sources and all of them vanish under the proxy
design. Reconsider only if a target game turns out not to use DirectInput 8.

- `hidclass.sys` unconditionally marks the write IRP pending, and
  DirectInput's `WriteFile` waits `INFINITE` on winedevice's single request
  thread, so any blocking send freezes the game rather than degrading.
  > `dlls/hidclass.sys/device.c:517-520`; `dlls/kernelbase/file.c:4053-4059`.
- `hidclass.sys` silently drops input reports shorter than the declared
  `InputLength`, with only an ERR line to show for it.
  > `dlls/hidclass.sys/device.c:349-352`.
- `hid_device_thread` abandons a pending `IOCTL_HID_READ_REPORT` IRP without
  cancelling it, and the FDO buffer is then freed. That is a use after free.
  > `dlls/hidclass.sys/device.c:337-341`, `pnp.c:393-400`.
- Nothing tells a minidriver that a game exited: `hidclass` consumes
  `IRP_MJ_CLOSE` at the PDO and never forwards it.
  > `dlls/hidclass.sys/device.c:783-813`, `pdo_close()`.
- Hiding the native copy of the wheel needs three registry values as an
  atomic set. `Hidraw`=0 alone removes only the IOHID copy and guarantees an
  SDL-sourced duplicate survives, and `Enable SDL`=0 without `DisableInput`=1
  drops every Generic Desktop joystick from the bottle.
  > CrossOver 26.3.0 `dlls/winebus.sys/main.c:960`, `:537`, `:1305`.

**D6. Reusing the predecessor project's PID descriptor.** Not applicable to
the current design, but recorded so it is not repeated: `macoswheels`'
`Sources/DEXT/HIDDescriptor.cpp` declares PID Device Control (`0x96`) and
Effect Type (`0x25`) as 8-bit `Output(Data,Var,Abs)` scalars, where Wine's
generator emits inner Logical collections containing usage arrays, and it
never declares `PID_USAGE_DC_DEVICE_RESET` (`0x9A`) at all. Wine's hidparse
flags a cap as a button only when `bit_size == 1` or the item is an array, so
`guidFFDriver` would never be set while `DIDC_FORCEFEEDBACK` still would.
That failure presents as a force feedback tab that exists and does nothing.

> Verified directly in `Sources/DEXT/HIDDescriptor.cpp:148-156` and `:16`
> against `dlls/winebus.sys/hid.c`, `hid_device_add_physical()`.

---

**D7. Driving the wheel in its PS4 position instead.** Tempting, and wrong,
though not for the reason it looks.

The attraction is real. In the PS4 position the wheel is `044f:b66d` with no
mode switch needed, so the whole endpoint 0 question in C6 disappears, along
with any privilege it might have demanded. Its force feedback is also better
understood than the T150's: `hid-tmff2` binds `0xb66d` in the same case group
as the PS3 ids and drives it through `hid_hw_request(HID_REQ_SET_REPORT)`
with a flat 31-byte packet, the same command set as PS3 mode with only the
buffer length differing. Nothing in that driver ever touches the `0xF0` to
`0xF3` authentication reports, so the PlayStation handshake does not gate
force feedback.

What kills it is input, not output. In PS4 mode the wheel presents a
DualShock 4 shaped descriptor whose Generic Desktop axes are the pad's dead
sticks; the steering and pedals live inside a 54-byte vendor blob that
nothing interprets. `hid-tmff2` fixes this by **substituting a whole report
descriptor**, which a kernel driver may do and this project may not.

> Kimplul, hid-tmff2 issue #30: "the buttons from the wheel worked just fine
> but pedals and wheel rotation didn't. Seems that the wheel uses a 54 byte
> input section marked as `Vendor defined 1` usage page, which means that
> Linux doesn't know how to interpret the data it gets. I modified the
> `rdesc` to get input based on what I saw with my own wheel." The same
> 54-byte section is visible in the descriptor measured in A4:
> `06 00 ff 09 21 95 36 81 02`.

That breaks the premise this whole design rests on, that input already works
and only the force feedback channel is missing (see ARCHITECTURE.md). Wine
passes the descriptor into the bottle verbatim (B1), so a game would see
dead sticks and no steering. Recovering it means the daemon streaming input
reports and the proxy synthesising a DirectInput device, which is D5's
rejected bus driver arriving through a side door.

So: **worse target, kept as the fallback.** If C6's mode switch turns out to
be impossible on macOS, PS4 mode is where this goes next, and the cost is
an input path the architecture does not currently have. Note also that
`hid-tmff2` supports no T150 at all, so that its packet set works on this
wheel is likely rather than known.

One free diagnostic in the meantime: the PS4 position needs no
initialisation, so if the wheel turns freely there and is rigid in the PS3
position, the motor is healthy and the rigidity is the uninitialised boot
state rather than a fault.

---

## E. Open questions. Only the Mac can answer these

In rough order of how much they matter. The first five are answered and are
kept here with their answers, because each was once the thing the project
turned on.

1. **Answered yes.** Does an unprivileged, non-seizing `IOHIDDeviceSetReport`
   physically move the T150? It does: A19. C3 and C5 argued this was genuinely
   uncertain rather than merely unconfirmed, and they were right to; the
   answer came out the good way.
2. **Answered: unnumbered and unpadded works.** Which framing does the
   firmware honour? A19 used report id 0, payload `40 03 64 00`, no padding.
   Whether the others also work is untested and no longer interesting for
   settings. It may yet matter for `ff_commit`, which is 15 bytes against a
   declared 14-byte report.
3. Does macOS clip an output report to the descriptor-declared maximum? One
   first-hand report on a T300RS on Apple Silicon says it clips at 62 bytes.
   No open source shows a clip; the decision happens inside the closed
   `AppleUserUSBHostHIDDevice` dext. Still open, and still relevant only to
   `ff_commit`: a 15-byte payload was accepted without complaint, but nothing
   confirms all 15 bytes arrived.
4. **Answered: `B65D`.** The wheel comes up at the boot id in the PS3
   position and needs the mode switch.
5. **Answered yes, unprivileged.** The ep0 vendor requests work against the
   HID-claimed interface with no capture and no root: A6.
6. How many `IOHIDDevice` nodes does one wheel publish, and which one accepts
   the output report? CrossWheel's documentation says a working G29 shows two
   nodes, usage page 1 and usage page 65280, and that both are needed.
7. What PE architecture is the bottle, x86_64 or arm64ec? One command:
   `file "$BOTTLE/drive_c/windows/system32/winedevice.exe"`.
8. Where does CrossOver keep the real builtin `dinput8.dll`, and can a
   renamed copy be loaded? Wine creates fake DLL stubs in a prefix's
   `system32` while loading builtins from its own directory, so the proxy
   must copy the right file. Unverified.

Probes 1 to 6 are what `src/probe/` exists to answer. See
[PROBES.md](PROBES.md).

---

## F. Prior art

- **CrossWheel** (crosswheel.seastian.com), commercial, closed source. Ships
  this exact architecture: a proxy DLL in the CrossOver bottle forwarding
  DirectInput force feedback calls to a macOS app that writes HID output
  reports. Covers the full DirectInput effect set with a 500 Hz mixer.
  Logitech is stable; its Thrustmaster beta targets the T300RS protocol, not
  the T150's. Useful as proof the shape works, and as an oracle: if it drives
  a wheel on your Mac, the macOS half is possible there.
- **fffb** (eddieavd/fffb), open source, see A3.
- **Akellacom/thrustmaster_t300rs_gt_macos_driver**, userspace macOS T300RS
  driver. Its CrossOver mode needs neither SIP nor AMFI off, but it needs
  sudo, and it does not deliver game-driven force feedback into Wine at all.
- **Oversteer** (berarma), contributes nothing portable: it is a GTK front end
  over python-evdev and the sysfs attributes the Linux drivers export.
- **CodeWeavers** has no wheel force feedback solution. Staff answer on the
  "Logitech G923 Wheel Force Feedback not working through Crossover" thread,
  2021-12-23.
