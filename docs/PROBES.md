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

```sh
./build/bin/probe_setreport            # autocenter to full, then enable
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
./build/bin/probe_setreport -i 0x0a         # the declared output report id
./build/bin/probe_setreport -P              # zero-padded to the declared size
./build/bin/probe_setreport -i 0x0a -P
./build/bin/probe_setreport -n 1            # the other HID node, if there is one
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
./build/bin/probe_setreport -r 270          # very short lock to lock
./build/bin/probe_setreport -r 1080         # back to full
```

## Question 3: does endpoint 0 work, and as whom?

Only needed if question 1 found the wheel at `B65D`.

The mode switch is a pair of vendor control transfers directed at interface
0, which is the interface macOS's own HID driver owns, so it may well be
refused. `probe_ep0` tries three escalating approaches and prints the
`IOReturn` of every one. By default it performs only the read-only model
query and does not switch anything.

```sh
./build/bin/probe_ep0                  # as your user
sudo ./build/bin/probe_ep0             # then as root, compare
sudo ./build/bin/probe_ep0 -s          # last resort, seizes the device
```

Record which step first succeeded and under which user. That single fact
decides whether the finished tool needs a password once per plug-in or never.

Once a model query has succeeded, and only then, the switch itself:

```sh
sudo ./build/bin/probe_ep0 -w
./build/bin/probe_hid                  # the product id should now be B677
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
| SetReport moves the wheel unprivileged | The architecture works. Build it. |
| SetReport succeeds but nothing moves | Try every framing above before giving up; if none work the HID path is closed and only raw interrupt OUT is left, which needs device capture and root. |
| ep0 works unprivileged | One clean tool, no password. |
| ep0 needs root | One `sudo` per plug-in. Note that sleep, wake or a replug drops the wheel back to boot mode, so this is a mid-race failure, not just an install-time annoyance. |
| ep0 needs `-s` | Reconsider the design before writing more code. |
