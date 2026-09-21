# PLAN — user-space macOS driver for the Logitech Driving Force GT (046d:c29a)

**Status: shipped and verified on this machine** (macOS 26.5.2, arm64, wheel attached).

| | | |
|---|---|---|
| M0 | recon tool | ✅ `dfgt probe` |
| M1 | read path + decoder | ✅ every control and the steering direction identified from capture |
| M2 | FFB primitives | ✅ `dfgt ffb / range` |
| M3 | daemon + IPC | ✅ `dfgt daemon` / `dfgt status` |
| M4 | richer effect emulation | ⏸ deferred (L1 doesn't need it) |
| M5 | packaging | ✅ `make install` / `make uninstall` |

L1 ("feels alive") is delivered: native mode, 40–900° range, self-centring spring, no kext/root.
L2 (socket API) is delivered. L3 (appear native to unmodified games) is **blocked**, see gate G1.

---

## 1. Goal / definition of done

| Layer | Deliverable | Value | Status |
|---|---|---|---|
| **L1** | Daemon sets native mode, range, autocenter spring. | Any game reading the joystick gets a wheel that resists and self-centres. | ✅ |
| **L2** | Line-based socket: decoded state out, FFB commands in. | Telemetry plugins/mods/emulators can drive real FFB. | ✅ |
| **L3** | Virtual HID wheel so unmodified games see a FFB device. | Needs an Apple-granted entitlement — see G1. | ❌ |

---

## 2. Non-goals (decided, don't relitigate)

- **No kext** (Apple Silicon can't load unsigned ones; `logiwheel2000` is a kext and unusable).
- **No DriverKit dext** (Apple-granted entitlement + notarisation + user approval; not worth it).
- **No hidapi** (its macOS backend opens devices *exclusively*, fighting the system HID driver).
- **Never seize the device** — the wheel keeps working as a plain joystick without our daemon.
- **No force watchdog.** The wheel holds an effect across arbitrary packet silence (measured,
  §3). A client that dies leaves its last force applied until `dfgt ffb off` or a replug. That is
  the device's own semantics; adding a timer would break by-hand use of `dfgt ffb`.

---

## 3. Verified facts — measured here, do not re-derive

### Device
- `046d:c29a`, `bcdDevice=0x1327`, one HID interface, 2 endpoints, one configuration. **All FFB goes
  through interface 0's output report** — there is no second interface.
- macOS binds `AppleUserUSBHostHIDDevice` → `AppleUserHIDEventDriver` with primary usage
  **Generic Desktop / Joystick (0x01/0x04)**, and publishes `GameControllerType`/`GameControllerPointer`.
- **Already in native mode** (14-bit steering, separate pedals). Compat mode would show a 200° axis
  and combined pedals; `dfgt native` detects and fixes that, and is a no-op today.

### Report descriptor (115 bytes, no report IDs)
| Report | Layout |
|---|---|
| Input 8B | `b0[3:0]` hat (0–7, **null = 8**), `b0[7:4]`+`b1`+`b2[0]` = buttons 1–21 (`bit n = 3+n`), `b3[7:1]` = 7 vendor state bits (**bit 4 = range flag**, fact 8), `b4 \| ((b5&0x3f)<<8)` = steering (0..16383), `b5[7:6]` = 2 vendor bits (**horn duplicate**), `b6` = throttle, `b7` = brake (255 = released) |
| Output 7B | `0xFF00 / 0x02` → the FFB channel |
| Feature 131B | `0xFF00 / 0x03` → unknown, untouched |

### Measured behaviour (these decided the design)
1. **`IOHIDDeviceSetReport(out, id 0, buf, 7)` → `kIOReturnSuccess`** while Apple's HID drivers are
   bound, no root, no entitlement, no kext.
2. **Effects persist across silence.** A constant force applied once kept driving the wheel for
   >10 s with zero further packets (wheel travelled 7972 → 6891 and stayed off-centre). There is no
   firmware watchdog → `kill -9` leaves force applied.
3. **Input reports are change-driven, not a heartbeat.** 3 s idle = 0 reports. Never use "no reports"
   as a liveness signal; state must be latched, and `dfgt status` may answer `no-input-yet` until
   something moves.
4. **A second process that opens the device steals report delivery from the first.** Verified: a
   reader got ~1 s of reports, then went silent the moment a CLI process opened the wheel. Therefore
   **the daemon must be the only reader**, and control commands must route through its socket — which
   is exactly what `dfgt ffb …` does when the socket answers.
5. **A client disconnecting mid-broadcast kills the daemon via SIGPIPE** (observed live) unless
   ignored. `SIGPIPE` is now ignored process-wide and dead connections are reaped.
6. Physical loop test: `fe 0d 02 02 30` + `14 00…` stiffened and self-centred the wheel;
   `11 08 90 80…` drove it; `13 00…`/`f5 00…` released it.
7. `dfgt watch` on a still wheel prints nothing — by design (see 3).
8. **The vendor bits are device state, not controls.** Across the 421-report control capture they
   never moved while any control was pressed. `vendor7` bit 4 tracks the configured range — clear at
   40°/200°, set at 540°/900° (threshold untested). Bits 1 and 2 each flickered for exactly one
   sample during the second capture (once during a quiet moment at centre, once on the first report
   after re-enumeration): transient firmware status, meaning unconfirmed. `vendor2` goes 0 → 1 → 3
   only while the horn is held (the horn is also button 20). The wheel emits a status report whenever
   the range changes, which is how this was observed.
9. **The range setting does not survive a replug.** After re-enumeration `vendor7` read 0x2f (range
   flag clear) instead of 0x3f, and the same hand sweep crossed the axis ~4.5× faster — the wheel had
   reverted to ~200° (900/200 = 4.5). **This is why the daemon re-asserts range and autocenter on every
   device match**: without it, every replug silently drops the wheel to 200°.

---

## 4. Architecture (as built)

```
games ──(HID / GameController, untouched)──┐
                                           │  single shared opener
                          ┌────────────────▼─────────────────┐
                          │  dfgt daemon                     │
                          │  latch state → decode → publish  │
                          └───────┬──────────────────┬───────┘
            7B output report (FFB)│                  │ unix socket, line-based
                                  │                  │
                              wheel              dfgt status / plugins
```

Files:
```
src/dfgt.c                        one binary, all subcommands (IOKit.hid + CoreFoundation only)
Makefile                          all / check / accept / install / uninstall / clean
launchd/local.dfgt.daemon.plist   login agent -> /usr/local/bin/dfgt daemon
tests/capture-wheel-motion.hex    real captured reports (wheel turning under FFB)
tests/capture-controls.hex        every control pressed once; the control-map evidence
tests/capture-turn-replug.hex      L1/R1, full left→right sweep, then unplug/replug
PLAN.md / README.md
```

- **Protocol:** newline-terminated lines, same grammar as the CLI —
  `state`, `ping`, `ffb constant <n>`, `ffb off`, `ffb autocenter <0..65535>`,
  `ffb autocenter-off`, `range <40..900>`. Replies are `ok …` / `err …`; state is pushed on
  every report as `steer=… steer_deg=… throttle=… brake=… hat=… hat_dir=… buttons=0x… pressed=… vendor7=… vendor2=… t=…`
  (`pressed=` carries the verified names, e.g. `shift-up,horn`).
- **Fail-safe:** the daemon neutralises force and autocenter on startup *and* on SIGINT/SIGTERM/exit.
  SIGKILL cannot be caught → documented: run `dfgt ffb off` (works with no daemon) or replug.

---

## 5. Milestones

- **M0 ✅** `dfgt probe`: element dump, native-mode detection, output-report self-test, `--cmd HEX`.
- **M1 ✅** `dfgt watch [N] [--hex]`: decodes and reports which fields changed. The full control map
  was established by capturing every control pressed one at a time
  (`tests/capture-controls.hex`, 421 reports, 163 s) and XOR-ing consecutive button states.
  HID button number = table entry:

  | # | control | # | control | # | control |
  |---|---|---|---|---|---|
  | 1 | X (cross) | 8 | L2 | 15 | dial push |
  | 2 | square | 9 | select | 16 | rocker + |
  | 3 | circle | 10 | start | 17 | dial CW (pulse per detent) |
  | 4 | triangle | 11 | R3 | 18 | dial CCW (pulse per detent) |
  | 5 | R1 | 12 | L3 | 19 | rocker − |
  | 6 | L1 | 13 | shift up (+) | 20 | horn |
  | 7 | R2 | 14 | shift down (−) | 21 | PS / GT |

  Axes: `b6` = accelerator, `b7` = brake (255 = released, 0 = fully pressed); hat 0/2/4/6 =
  up/right/down/left, 8 = centred. Dial turns report as ~0 ms button pulses, one bit per direction.
  Each shoulder pair is numbered right-first: 5 = R1, 6 = L1, 7 = R2, 8 = L2.
  **Steering direction verified** (second capture, `tests/capture-turn-replug.hex`): turning fully
  left (counter-clockwise) drives `steer` to **0**, fully right (clockwise) to **16383** — rising
  reads clockwise/right, and the mechanical stops use the full 0..16383 span at range 900.
- **M2 ✅** `dfgt ffb constant|off|autocenter|autocenter-off`, `dfgt range`, `dfgt native`,
  `dfgt modes` / `dfgt mode <persona>` (firmware multimode switching, `--vid/--pid` overrides).
  Byte builders are unit-tested against the Linux reference values in `dfgt selftest`.
- **M3 ✅** `dfgt daemon` (single reader, settings owner, socket server, reconnect on replug,
  clean shutdown), `dfgt status`.
- **M4 ⏸** Emulated spring/damper/friction/periodic by mixing torque at ~100 Hz (as `new-lg4ff`
  does). Only worth doing against a concrete consumer.
- **M5 ✅** `make install` (binary + login agent), `make uninstall` (unloads, neutralises wheel,
  removes files).

---

## 6. Acceptance tests

Automated (`make check`, no wheel needed): 20 decoder cases (real captures + synthetic edge cases)
and 20 command-builder assertions; fails on any drift.

Physical:
| | test | result |
|---|---|---|
| A1 | `dfgt ffb autocenter 0xaaaa` → wheel resists and self-centres | ✅ spring centred the wheel (steer 9424 → 8191) |
| A2 | `dfgt range 200` vs `900` → lock-to-lock changes, vendor7 bit 4 flips | ✅ verified via status reports; steering sign confirmed (0 = full left, 16383 = full right, full span at the stops) |
| A3 | `dfgt ffb constant 48` → wheel pulled off centre; `dfgt ffb off` → released | ✅ moved to −18° and held; off released it |
| A3b | `kill -9` the daemon with a force active | ⚠️ **wheel keeps the force** (no firmware watchdog). Clear with `dfgt ffb off`, or replug. |
| A4 | every control shows up correctly | ✅ 19 of 21 controls verified from the capture; buttons 5/6 (L1/R1) unverified |
| A5 | unplug/replug mid-session → reader resumes, daemon re-arms | ✅ `device removed` → `device ready` observed and reports resumed (read path verified across a real replug); the daemon's re-assert is verified against the reverted post-replug device state at startup. Both halves exercised, but not yet in one continuous run — one replug with the daemon live would close that |
| A6 | login → wheel is stiff/centred with no terminal open | ⏳ after `make install`, log out/in |

---

## 7. Open questions / gates

- **G1 ❌ DECIDED (spike run: negative). L3 is not reachable from user space without an Apple-granted
  entitlement.** `IOHIDUserDeviceCreate` is exported by IOKit.framework (but has no public header), so
  it can be linked; creating a device is what fails:
  - unsigned, unprivileged → returns NULL; kernel logs
    `(IOHIDFamily) IOHIDResourceDeviceUserClient:0x0 vhid is not entitled`
  - ad-hoc signed *with* `com.apple.hid.manager.user-access-device` → process **SIGKILLed**, and
    `AMFI: code signature validation failed` in the log: restricted entitlements cannot be self-signed
  - **root does not help**: `IOHIDResourceDeviceUserClient::initWithTask` (IOHIDFamily @02b1f53c)
    checks only `com.apple.hid.manager.user-access-device` then
    `com.apple.developer.hid.virtual.device` via `copyClientEntitlement`, with no uid fallback

  Re-verify: build a 30-line program that calls `IOHIDUserDeviceCreate` with a
  `kIOHIDReportDescriptorKey` + a vendor-page descriptor, then check `hidutil list` for it.
  Remaining route = paid Developer Program + request the restricted entitlement + Developer ID +
  notarisation (same user-client path, no dext required). Out of scope here.
  **Consequence: unmodified games will never see this wheel, so L2 + a game-side telemetry plugin is
  the only path to real force feedback in games.** Do not substitute keyboard/mouse emulation.
- **G2 ✅ resolved:** reports are change-driven; effects persist (facts 2 and 3 above).
- **G3 ✅** `vendor7`/`vendor2` explained (fact 8). No centre calibration needed: the wheel's rest
  position measured 8191–8210 across sessions, within ±0.2% of full scale of the 8192 nominal
  centre. **Remaining:** press L1/R1 to fix buttons 5/6, and turn the wheel fully left/right to fix
  the steering sign. The axis is unitless, so degrees are `(X − centre)/16383 × range`.
- **G4:** the 131-byte feature report is unexamined. Probe read-only if a real need appears.

---

## 8. Risks / limitations

- **Most Mac games don't look for wheels.** No driver fixes a game that reads only gamepads. L1 still
  helps SDL2/3 and HID-aware titles; L2 helps games with plugin/mod/telemetry hooks; L3 is the only
  general fix and is unproven here.
- **`kill -9` on the daemon leaves force applied** (hardware semantics). Documented, clearable.
- **Apple HID internals are undocumented API.** If a macOS update changes report routing, the daemon
  may break — but the wheel still works as a plain joystick without it.
- **Prior art is GPL** (`logiwheel2000`, Linux `hid-lg4ff`). Protocol facts were reimplemented from
  the reference; no code was copied.
- **Streaming (GeForce NOW) is a separate constraint.** GFN's force-feedback wheel support is a
  whitelist (G29/G920/G923/PRO) and the DFGT is not on it; the client carries no extendable wheel
  table (3960-entry controller DB, zero wheels). See `docs/geforce-now.md`.

---

## Appendix A — FFB command table (7-byte output report, report ID 0)

| Function | Bytes | Notes |
|---|---|---|
| Native mode (only if compat detected) | `f8 0a 00 00 00 00 00` then `f8 09 03 01 00 00 00` | 2nd makes the device detach and re-enumerate |
| Set range | `f8 81 <deg&0xff> <deg>>8 00 00 00` | ✅ degrees, 40–900; 900 = `f8 81 84 03 …` |
| Constant force | `11 08 <0x80+level> 80 00 00 00` | level −128..127, `0x80` = no force, 0 → `13 00…` |
| Force off | `13 00 00 00 00 00 00` | slot 1 |
| Autocenter profile | `fe 0d a a b 00 00` then `14 00 00 00 00 00 00` | `14 00…` is required to activate |
| Autocenter off | `f5 00 00 00 00 00 00` | |
| LEDs | `f8 12 <mask> 00 00 00 00` | probably a no-op on this wheel |

Autocenter maths (`m` = 0..0xffff; non-MOMO): `m ≤ 0xaaaa → ea = 0x0c·m, eb = 0x80·m`, else the
linear extension; then `ea >>= 1`, `cmd = fe 0d (ea/0xaaaa) (ea/0xaaaa) (eb/0xaaaa) 00 00`.
`m = 0xaaaa → fe 0d 06 06 80`, `m = 0x4000 → fe 0d 02 02 30` (gentle).

## Appendix B — evidence commands
```
hidutil list | grep -i 0x46d                 # 046d:c29a usage 1/4, transport USB
./build/dfgt probe                           # 32 elements + output-report self-test
./build/dfgt watch                           # decoded input, prints only on change
./build/dfgt watch --hex > tests/capture.hex # fixture-grade capture
make check                                   # decoder + command builders + fixture replay
```
Raw fixture line (wheel turning, pedals released, hat centred):
`08 00 00 5e 32 20 ff ff` → hat=8, buttons=0, vendor7=0x2f, steer=8242, throttle=brake=255.

## Appendix C — prior art
`gitlab.com/ColdPie/logiwheel2000` (kext port of Linux `lg4ff`, GPL, protocol cross-check) ·
`github.com/berarma/new-lg4ff` (effect emulation maths) · `github.com/CesarOvilla/g923-mac-ffb`
(userspace FFB daemon on Apple Silicon) · `github.com/eddieavd/fffb` (classic Logitech FFB from a
macOS game plugin) · `github.com/qryptixai/MotorAxN` (GameController input + HID FFB) ·
`codeberg.org/subaksu/FreeTheWheel` (minimal native-mode/900° enabler).

## Deliberate simplifications
- One binary, one run loop, flags only, no config file, no plugin system.
- No virtual device (G1), no effect emulation (M4), no watchdog (fact 2) until something needs them.
- `dfgt ffb …` falls back to talking to the wheel directly when no daemon runs — convenient for
  acceptance tests, and harmless because a daemon would otherwise be the only reader.
