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
a hat. Axes, pedals, buttons and the hat therefore already reach games.

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
still locked.** Autocenter on and off, rotation range at both extremes, with
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

**C7 supplies a third, and it is now the leading one:** macOS's
`IOHIDDeviceSetReport` reaches the wheel over the control pipe, and
Thrustmaster firmware acknowledges that pipe and ignores it. An independent
macOS driver for the sibling T300RS says exactly this and seizes the USB
interface to write on the interrupt OUT pipe instead. Read C7 before
spending any more time on framings; it predicts every result seen here.

The two earlier hypotheses, which C7 largely subsumes:

1. **HID output never reaches the firmware.** The writes are accepted by
   macOS and discarded, so nothing can change the wheel's resting state. This
   is C3's risk arriving exactly as feared, and it would mean E1 is answered
   no.
2. **The wheel wants something first.** The Linux driver sends a packet on
   the interrupt OUT pipe when the input device is opened, before any setting
   is honoured. Its bytes are not recoverable from the published source,
   where the pointer is left null, so this project has never sent it. If the
   firmware gates settings behind that, every write so far would be accepted
   and ignored regardless of framing.

Both predict precisely what was measured, which is why neither can be
concluded. What separates them is a wheel that unlocks under a driver known
to work: on Windows with Thrustmaster's own driver, or on Linux with
`hid_thrustmaster` and `t150_driver`. If it frees up there, the wheel and the
protocol are both fine and the fault is in the macOS HID path. If it stays
rigid there too, the wheel's resting behaviour is simply this and the
autocenter theory is wrong.

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
wheel sitting at maximum autocenter feels like to a light grip. That is
consistent with everything else and it keeps hypothesis 1 alive: the wheel is
holding, and nothing this project sends persuades it to stop.

The cheap test that separates electrical from mechanical, if there is any
doubt left: **pull the mains lead and leave USB connected.** The motor rail
comes from mains, the logic rail from USB. If the wheel frees up, the hold is
being produced electrically and the hardware is fine. Do not escalate force
to find out: a stripped gear and a shredded belt are this wheel's signature
failures.

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

Two OSes agreeing therefore says the behaviour is not macOS-specific, and
says nothing yet about whether the wheel is faulty.

**This makes the decisive experiment cheap, and it needs no Mac.** Install
`scarburato/t150_driver` on that same Linux machine, which is a DKMS module
with an `install.sh`, and watch the wheel.

- **It releases** — the wheel and the protocol are both fine, the T150 simply
  requires an initialisation this project has never sent, and hypothesis 2
  above is the answer.
- **It stays rigid** — a driver written for this exact wheel cannot free it
  either, and the problem is the wheel or its firmware revision.

And whichever way it goes, the same setup yields something this project has
wanted from the beginning. `usbmon` on that machine captures the exact bytes
a working driver puts on the wire: the initialisation packet whose bytes are
left null in the published source, whether transfers go to the interrupt OUT
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

What this run does **not** record is whether the wheel physically reacted,
which is still the only thing E1 needs. Capture, write and release all
returning success says the bytes reached the firmware's doorstep; it does not
say the firmware acted on them.

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

Everything below was written when the wheel was believed to be at fault, and
is kept because the reasoning still holds if `-I` does not fix it.

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
not a force feedback fix.

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
> **This is the single biggest risk in the project.** C2 proves Thrustmaster
> firmware can accept HID output reports; C3 means nobody has demonstrated
> that the T150's does.

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
control SET_REPORT and ignores it.** This is the most consequential external
finding this project has, because it predicts exactly what A8 measured.

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

That is precisely the shape of what was measured here: every write accepted,
nothing ever happens. It is C3 arriving as feared, one layer further out than
expected. C3 said the T150's driver never uses the HID layer; C7 says that on
macOS the HID layer is the only thing an unprivileged process has, and the
firmware does not listen to it.

**Two things stop this being conclusive for this project.** It is a T300RS
finding, not a T150 one, and the T150's protocol and firmware differ. And the
project has not itself demonstrated which pipe `IOHIDDeviceSetReport` uses on
macOS 26; the claim that it is the control pipe is Akellacom's, inferred from
behaviour rather than from Apple's sources, which are closed.

It is nonetheless the best available explanation of A8, and it should be
treated as the leading hypothesis rather than as one of several.

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

**What that means here, split honestly in two:**

- **The settings half of this project is unblocked.** Rotation range, gain
  and autocenter are exactly what `thrustmaster_configure_wheel()` sets, by
  briefly capturing the wheel and letting it go. `t150ctl` can be built that
  way, needs root but no SIP or AMFI change, and leaves CrossOver's wheel
  alone afterwards because the settings persist in the wheel's own firmware.
  It also predicts something testable: sending `40 04 00 00` down that pipe
  should release the autocenter and free the rigid wheel, which would
  simultaneously prove the wheel healthy, the encoders correct and the pipe
  the culprit.
- **The game-driven half is not.** Continuous per-effect writes need the
  interrupt OUT pipe held open, and holding it means capturing the device,
  and capturing it takes the wheel away from CrossOver. That is the exact
  trade B6 and D5 were written to avoid, and no amount of care avoids it: the
  wheel cannot be owned by the daemon and read by the bottle at the same
  time.

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

This is the initialisation this project has never sent and could not send,
because it has no route to the interrupt OUT pipe. Whether the T150 needs an
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

In rough order of how much they matter.

1. Does an unprivileged, non-seizing `IOHIDDeviceSetReport` physically move
   the T150? Everything depends on this. See C3 and C5 for why it is genuinely
   uncertain rather than merely unconfirmed.
2. Which framing does the firmware honour: report id `0x0A` or unnumbered,
   padded to 14 bytes or the raw 2 to 4?
3. Does macOS clip an output report to the descriptor-declared maximum? One
   first-hand report on a T300RS on Apple Silicon says it clips at 62 bytes.
   No open source shows a clip; the decision happens inside the closed
   `AppleUserUSBHostHIDDevice` dext.
4. Does the T150 sit at `044F:B65D` on macOS with nothing installed, or does
   it already come up at `B677`? If the latter, question 5 is moot.
5. Can the ep0 vendor requests (recipient = interface, `wIndex` 0) be issued
   against a HID-claimed interface, unprivileged or as root, without device
   capture?
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
