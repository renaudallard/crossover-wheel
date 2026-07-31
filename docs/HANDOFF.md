# Start here

Instructions for continuing this project in a fresh session with no prior
context. Read this file, then [RESEARCH.md](RESEARCH.md). Everything else is
detail.

---

## 1. The goal

Give a **Thrustmaster T150** working force feedback in games running under
**CrossOver** on macOS, with nothing installed system wide: no kext, no
DriverKit system extension, no SIP change, no AMFI change, no system
extension approval, and ideally no root.

Scope is the T150 and CrossOver. Native macOS games are out of scope. Other
wheels are out of scope. Both by choice.

## 2. Why the obvious approaches are not available

A DriverKit system extension is not a path: Apple signs DriverKit
entitlements only for paid teams granted DriverKit case by case. That was the
predecessor project (`macoswheels`, same author) and it only ever loaded with
SIP disabled and `amfi_get_out_of_my_way=0x1`.

Presenting a force feedback device to *macOS* by any other route is also
closed, for reasons unrelated to signing: see RESEARCH.md D1 and D2. So the
force feedback device is presented **inside the CrossOver bottle** instead.

## 3. The design

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

The wheel keeps reaching the game through the normal path for input, because
that already works (RESEARCH.md A1, B1). Only the force feedback output
channel is missing, because the wheel's descriptor has no PID collection and
Wine adds none of its own on macOS (RESEARCH.md B3, B4).

Consequences worth internalising, because they are what make this design
small: there is no synthetic HID device, no report descriptor to build or
splice, no PID report parsing anywhere, and no duplicate wheel to hide.

## 4. Decisions already made

Do not relitigate these without new evidence. Each has its reasoning in
RESEARCH.md.

| Decision | Where |
| --- | --- |
| Proxy DLL, not an in-bottle bus driver | D5 |
| Never seize the HID device | B6 |
| No PID descriptor anywhere in this project | B4, D6 |
| C throughout, no Swift | user's call |
| T150 only | user's call |
| Downgrade unsupported effects rather than refusing them | PROTOCOL.md |

## 5. Current state

Implemented and working:

- `src/probe/` : three macOS measurement tools, see section 6.
- `include/t150/` : wire constants, normalized effect model, wire protocol.
  Portable, compiled and self-tested on Linux.
- `Makefile`, CI, `README.md`, man pages, docs.

Not started: `t150d`, `t150-dinput8.dll`, `t150boot`, `t150ctl`.

The probe sources have **never been compiled on macOS**. They were
type-checked on Linux against hand-written stub CoreFoundation and IOKit
headers, which catches syntax, types and misuse of this project's own APIs
but cannot catch misuse of the real IOKit API. The `macos` CI job is the
first genuine compile.

## 6. The gate

**Do not write the daemon or the DLL until this is answered.** One
measurement decides whether the whole design is viable: does an unprivileged,
non-seizing `IOHIDDeviceSetReport` physically move the wheel?

It is genuinely uncertain, not merely unconfirmed. The T150's Linux driver
bypasses the HID layer entirely and writes raw on the interrupt OUT pipe,
while the newer T300 family does go through `hid_hw_request(SET_REPORT)`.
See RESEARCH.md C3 and C5.

The procedure, what to record and what each outcome means are in
[PROBES.md](PROBES.md). It needs the Mac, the wheel and about twenty minutes.

Two traps:

- A `kIOReturnSuccess` return is **not** the answer. macOS can accept a
  report the firmware then discards. What settles it is whether the wheel
  physically reacted.
- If the first attempt fails, work through every framing (`-i 0x0a`, `-P`,
  `-n 1`, and the range opcode as well as the spring) before concluding the
  HID path is closed. A wrong "no" here kills a viable project.

## 7. Build order after the gate

Each milestone ends in something checkable. Do not start the next until the
previous one is verified.

**M1. Encoder, on Linux.** Port the T150 settings and force feedback encoders
to C from the byte layouts in [PROTOCOL.md](PROTOCOL.md). Pure functions,
no I/O.
*Done when:* `make test` is green with golden vectors asserting the exact
byte sequences in PROTOCOL.md, including the `0x43` gain narrowing and the
`0x40 0x11` range scaling.

**M2. Protocol and daemon core, on Linux.** Implement
`t150_proto_pack_*`/`unpack_*` from `include/t150/proto.h`, the socket
server, the slot table, the effect downgrades, and a fake HID backend that
logs the bytes it would have written.
*Done when:* a test client drives the fake backend end to end and the logged
bytes match the M1 golden vectors. Still no Mac needed.

**M3. macOS HID backend.** Device matching, non-seizing open, output writes,
hot plug. Plus `t150ctl`.
*Done when:* on the Mac, `t150ctl range 270` visibly shortens lock to lock
and `t150ctl autocenter 0` releases the spring, with no password prompt.

**M4. Proxy DLL, under stock Wine on Debian.** Build with mingw-w64. The
**first** thing to settle is the forwarding mechanism: our `dinput8.dll` has
to reach the builtin implementation without the loader handing it back
itself. Copy CrossOver's real builtin, not the fake-DLL stub a prefix keeps
in `system32` (RESEARCH.md E8). Then wrap `GetCapabilities`, enumeration
under `DIEDFL_FORCEFEEDBACK`, `EnumEffects`, `CreateEffect`,
`IDirectInputEffect::SetParameters`/`Start`/`Stop`, `SetProperty` for
`DIPROP_FFGAIN` and `DIPROP_AUTOCENTER`, and `SendForceFeedbackCommand`.
*Done when:* a small DirectInput 8 test exe under Debian's stock Wine reports
`DIDC_FORCEFEEDBACK`, creates and starts a constant force and a spring, and
the fake daemon from M2 logs the right normalized effects. This whole
milestone is falsifiable without a Mac.

**M5. First force feedback in a real game.** Install both halves into a
bottle.
*Done when:* a real title's force feedback settings are live and the wheel
pushes back.

**M6. Robustness.** `t150boot`, the watchdog, reconnect on both ends, and
docs.
*Done when:* unplug and replug mid-game recovers; killing the daemon leaves
the wheel limp rather than latched.

The watchdog is not optional and is easy to forget. Nothing in the stack
learns that a game exited: Wine's `hidclass.sys` consumes `IRP_MJ_CLOSE` at
the PDO and DirectInput sends a reset only on a graceful unacquire. A crashed
or force-quit game would otherwise leave the last commanded force latched on
a wheel that pulls hard. Treat silence as a fault: see `T150_WATCHDOG_MS`.

## 8. If the gate fails

In order of preference.

1. **Wrong framing.** Exhaust section 6's list first. This is the most likely
   explanation for a failure.
2. **Wrong node.** If the wheel publishes a separate vendor-usage node, the
   output report may only be reachable there (RESEARCH.md E6). Note that
   winebus ignores vendor-usage nodes, so this affects only the daemon, not
   the bottle.
3. **Raw interrupt OUT.** Needs USB device capture, which needs root
   (RESEARCH.md D4), and capture terminates every other client of the device
   including CrossOver's view of it. This is a different architecture, not a
   degraded mode. Reassess before building it.
4. **Stop.** Say so plainly. A wheel whose firmware ignores HID output
   reports and whose interface cannot be captured without breaking CrossOver
   cannot be driven this way, and the honest outcome is to report that rather
   than build something that does not work.

## 9. Working rules

The author's global instructions apply. The project-specific ones:

- Development happens on Debian arm64. The Mac is for installing and testing
  only, and builds are CI-only. Do not assume a Mac is reachable.
- Keep as much as possible buildable and testable on Linux. `include/` and
  the encoders are portable by design; that is deliberate, not incidental.
- `make strict` is what CI runs. Keep it clean.
- The probe sources cannot be compiled on Linux. If you change them,
  type-check against stub headers before claiming they build, and say plainly
  that CI is the first real compile.
- Never claim the wheel works without a physical observation from the Mac. A
  successful return code is not an observation.
- Update `README.md` and the man pages in the same commit as the change they
  describe.
- One commit per change, human-readable message, no attribution lines.
- Never push unless asked.
- KISS and UNIX principles. OpenBSD-quality C: no buffer overflows, no use
  after free, no undefined behaviour, no leaks, no duplicated code.

## 10. Map

| File | What it is |
| --- | --- |
| `docs/HANDOFF.md` | this file |
| `docs/RESEARCH.md` | every verified fact with its citation, the dead ends, the open questions |
| `docs/PROBES.md` | how to run the gate and what each outcome means |
| `docs/PROTOCOL.md` | the T150 wire protocol, and what in it is still unconfirmed |
| `docs/ARCHITECTURE.md` | the design and the rejected alternative |
| `include/t150/t150.h` | wire constants, each traced to a cited source |
| `include/t150/effect.h` | the normalized effect model the DLL sends |
| `include/t150/proto.h` | the DLL to daemon wire protocol |
| `src/probe/` | the three gate measurement tools |
| `tests/header_check.c` | what CI can check without a Mac or a wheel |
