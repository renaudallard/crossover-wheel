# Phase 0: the three measurements

Nothing else in this repository is worth writing until these three questions
are answered on real hardware. They take about twenty minutes with the wheel
plugged into the Mac.

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
switch in question 3 takes it to `044f:b677` where everything in
[PROTOCOL.md](PROTOCOL.md) applies. In the PS4 position it is `044f:b66d`
instead, a DualShock 4 shaped device with a different descriptor and a
different protocol, and nothing here applies to it: `probe_setreport` will
match no node at all. See RESEARCH.md A4.

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

---

## Question 1: is the wheel already in firmware mode?

The T-series enumerates at the shared boot product id `044F:B65D` and only
reveals its real personality after a vendor control transfer flips it to
`044F:B677`. If your wheel is already at `B677` when you plug it in, the
control transfer is not needed at all and question 3 becomes irrelevant.

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

## Question 2: does an unprivileged SetReport move the wheel?

This is the one that decides the project. It only applies once the wheel is
in firmware mode.

**Check the wheel turns freely first.** One measured wheel sat locked rigid
in boot mode and stayed that way through every write. That is not an answer:
a wheel already held immovable cannot demonstrate an autocenter spring or a
shorter lock to lock, so the thing this question watches for is invisible
before question 3 has run. If the wheel will not turn by hand, go to question
3, come back, and check again. If it still will not turn once it reports
`B677`, say so, because that is a different and much more interesting
failure.

```sh
./build/bin/probe_setreport
```

The autocenter spring is used rather than the rotation range because its
effect is unmistakable: the wheel starts pulling itself back to centre
immediately, with no need to hunt for the end stops. Switch it back off with:

```sh
./build/bin/probe_setreport -A
```

**A success return is not the answer.** macOS can accept a report that the
firmware then ignores. What settles this is whether the wheel physically
reacted. If every call returned `kIOReturnSuccess` and the wheel did nothing,
work through the framings before concluding anything:

```sh
./build/bin/probe_setreport -i 0x0a
./build/bin/probe_setreport -P
./build/bin/probe_setreport -i 0x0a -P
./build/bin/probe_setreport -n 1
```

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

`probe_setreport` defaults to the firmware product id, so if question 1 found
the wheel at `B65D` the runs above matched nothing. Point it at the boot id
and repeat:

```sh
./build/bin/probe_setreport -p 0xb65d
```

This is worth five minutes even when the wheel is already at `B677`, because
a wheel that honours settings in boot mode makes the whole endpoint 0 problem
in question 3 go away.

### Also try it with CrossOver running

Everything above is worth repeating once with a game running in a bottle.
macOS 26 fails `setReport` from every client the moment any process seizes
the device, and the design depends on CrossOver not doing that. A run that
succeeds on an idle desktop and fails with a game running has found something
important.

## Question 2b: does the force feedback protocol work too?

Only once question 2 has moved the wheel. The settings opcodes prove the
transport; this proves the part the daemon will actually spend its time on,
and it costs one more run.

An effect uploads as three packets that correlate through slot keys, then a
fourth starts it. `-x` is repeatable so all four land on one open handle,
which matters because nobody knows whether the wheel keeps an uploaded effect
across a close. Slot 0, a constant force at half level, endless:

```sh
./build/bin/probe_setreport -g 0x60
./build/bin/probe_setreport \
    -x "02 1c 00 00 00 00 00 00 00 46 54" \
    -x "03 0e 00 20" \
    -x "01 00 00 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

The level byte is `0x20` because a constant appears to top out at `0x40`, not
at `0x7f`: the driver divides a full scale value by `0x1ff`, which lands on
64. A periodic magnitude reaches `0x7f`. If `0x40` and `0x7f` feel the same,
the ceiling is real and `T150_FF_LEVEL_MAX` is right; if `0x7f` is stronger,
raise it.

**Hold the wheel or keep a hand on the plug.** A constant force with no
duration limit does not stop on its own. Stop it with:

```sh
./build/bin/probe_setreport -x "41 00 00 01"
```

If the settings packets moved the wheel and this does nothing, retry it with
the same framing flags that worked for question 2, then say so plainly: it
means the transport is fine and the force feedback layout is wrong, which is
a much better place to be than the alternative.

### While you are here: the two missing waveforms

The protocol has type codes for sine, sawtooth up and sawtooth down but none
for square or triangle, and the codes are contiguous. So `0x4020`, `0x4021`
and `0x4025` may be waveforms the Linux driver never implemented. Upload a
periodic and vary only the commit type code:

```sh
./build/bin/probe_setreport \
    -x "02 1c 00 00 00 00 00 00 00 46 54" \
    -x "04 0e 00 40 00 00 e8 03" \
    -x "01 00 20 40 ff ff 00 00 00 0e 00 1c 00 00 00" \
    -x "41 00 41 01"
```

That is a one second period at half magnitude with type `0x4020`. Repeat with
`21 40` and `25 40` in the third packet. Anything that oscillates is a
waveform we can stop downgrading. Stop it the same way as above.

## Question 3: does endpoint 0 work, and as whom?

Only needed if question 1 found the wheel at `B65D`.

The mode switch is a pair of vendor control transfers directed at interface
0, which is the interface macOS's own HID driver owns, so it may well be
refused. `probe_ep0` tries three escalating approaches and prints the
`IOReturn` of every one. By default it performs only the read-only model
query and does not switch anything.

```sh
./build/bin/probe_ep0
sudo ./build/bin/probe_ep0
sudo ./build/bin/probe_ep0 -s
```

Record which step first succeeded and under which user. That single fact
decides whether the finished tool needs a password once per plug-in or never.

Once a model query has succeeded, and only then, the switch itself:

```sh
sudo ./build/bin/probe_ep0 -w
./build/bin/probe_hid
```

Be aware of what `-s` costs if it turns out to be the only thing that works.
Seizing takes the device away from every other client, so the wheel would
vanish from CrossOver for as long as the daemon holds it, which is not a
degraded mode but a different design.

---

## What the answers decide

| Outcome | Consequence |
| --- | --- |
| Wheel already at `B677` | No control transfer, no root, anywhere. Best case. |
| Wheel at `B65D` but honours settings there | Almost as good: the mode switch stops being load bearing. |
| SetReport moves the wheel unprivileged | The architecture works. Build it. |
| It moves on an idle desktop but not with a game running | Something in the bottle is seizing the device. Find out what before writing anything. |
| Settings work but the force feedback packets do nothing | The transport is fine and the packet layout is wrong. Recoverable, and much the better failure. |
| A contiguous type code plays a waveform | Square or triangle stops being a downgrade. Record which code. |
| SetReport succeeds but nothing moves | Try every framing above before giving up; if none work the HID path is closed and only raw interrupt OUT is left, which needs device capture and root. |
| ep0 works unprivileged | One clean tool, no password. |
| ep0 needs root | One `sudo` per plug-in. Note that sleep, wake or a replug drops the wheel back to boot mode, so this is a mid-race failure, not just an install-time annoyance. |
| ep0 needs `-s` | Reconsider the design before writing more code. |
