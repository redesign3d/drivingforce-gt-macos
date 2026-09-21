# GeForce NOW + a wheel on macOS (streaming notes)

**Goal:** Forza Horizon 6 through GeForce NOW, with the wheel working in full.

**Bottom line:** GFN's wheel support *with force feedback* is real — but it is a **whitelist**. The
Driving Force GT is not on it, and there is nothing client-side to extend, so with a DFGT the ceiling
through GFN is gamepad-emulated input with **no force feedback**. "The wheel in full" through GFN
requires one of the whitelisted wheels.

## What GFN supports (NVIDIA KB, spot-checked 2026-09)

Force-feedback wheels, Windows and macOS desktop apps only:

- Logitech PRO Racing Wheel & Pedals *(TrueForce not supported)*
- Logitech G923 Racing Wheel & Pedals *(TrueForce not supported; the Xbox variant can mis-enumerate
  as an Xbox device instead of a PC device on macOS)*
- Logitech G920 Driving Force Racing Wheel & Pedals
- Logitech G29 Driving Force Racing Wheel & Pedals
- Logitech G Driving Force Shifter

Community reports add two practical notes: Logitech **G HUB must be installed and running**, and
G29/G920 owners report FH6 tuning pain over GFN ("car spinning like crazy") because the in-game
rotation setting (e.g. 900°) reportedly does not take effect through the stream.

## Evidence from the installed client (GeForceNOW 2.0.88.129, macOS)

Read out of the app bundle, because "just patch the client" is the obvious next question:

- GFN carries its own controller database in `Contents/Frameworks/libGeronimo.dylib`
  (**3960 entries**; the bundled SDL2 framework holds another 382). **Neither contains a single wheel
  PID** — no G29 (`c24f`), no G920 (`c262`), no G923 (`c26e`/`c266`), no G27 (`c29b`), no DFGT
  (`c29a`). Its 32 Logitech entries are all gamepads: WingMan, Dual Action, F310/F510/F710,
  RumblePad, ChillStream, Cordless Precision.
- No `G29` / `G920` / `G923` / `Driving Force` / `racing wheel` strings, and no wheel protocol code,
  in `libGeronimo.dylib`. Its rumble path is NVIDIA's `Forge` controller abstraction over
  **CoreHaptics** (`ForgeGCController`, `CHHapticEngine`, `Haptics left/right server reset`) — that is
  haptics for supported *controllers*.
- Therefore wheel support is a dedicated, whitelist-driven path, not a mapping table we can add an
  entry to. Adding a VID/PID wouldn't buy force feedback either: the FFB stream is encoded in the
  *supported wheel's* protocol, and we cannot intercept the client's writes to the device (no HID
  filtering without a kext/dext — see PLAN gate G1).

## Can the DFGT present itself as a G29 in hardware? (tested — no)

Logitech's multimode wheels can be told to re-enumerate as another model: `f8 0a …`
("revert mode upon USB reset", so a replug is always an escape) followed by `f8 09 <idx> 01 …`, after
which the wheel comes back under the other model's USB PID — Linux identifies the current mode from
the PID alone. This is now exposed as `dfgt modes` / `dfgt mode <name> [--force]`, with global
`--vid/--pid` overrides so a switched wheel stays reachable.

What this unit's firmware actually did:

| persona | idx | expected pid | result |
|---|---|---|---|
| `dfex` | 0x00 | c294 | not exercised (a switch needs a replug to undo: from the DF-EX/DFP persona the wheel cannot be told to return to DFGT — only G27/G29 firmware can target DFGT) |
| `dfp` | 0x01 | c298 | not exercised (same reason) |
| `g25` | 0x02 | c299 | **ignored** — stayed at `c29a` |
| `dfgt` | 0x03 | c29a | current persona (native) |
| `g27` | 0x04 | c29b | **ignored** — stayed at `c29a` |
| `g29` | 0x05 | c24f | **ignored** — stayed at `c29a` |

So the firmware carries no G25/G27/G29 persona — exactly what Linux's policy table implies ("DFGT can
only be switched to DF-EX, DFP or its native mode") and what a 2007 firmware versus a 2015 wheel
suggests. **The hardware route to a whitelisted identity is closed.**

Honest caveat: DF-EX/DFP were not exercised, so this shows the newer indices are ignored, not that
the mode mechanism works on this unit. Either way, neither of those personas is on GFN's whitelist.

## What is left for "the wheel in full"

1. **A whitelisted wheel** (G29/G920/G923/PRO) — official path, force feedback, works today.
2. **Patching the GFN client** so it reports a whitelisted identity for our wheel. There is community
   precedent for modifying and locally re-signing the macOS client
   (`mikeqwe/gfn-steam-controller-fix` routes Steam Input's virtual Xbox controller through GFN's HID
   backend). Two things make it less hopeless than it sounds:
   - the force-feedback command family is **shared**: the G29 and the DFGT both use the `lg4ff` 7-byte
     vendor commands (`f8 81`, `fe 0d`, `11 08`, `14`/`f5`), so FFB may need little or no translation;
   - the whitelist looks **server-side** (the client carries no wheel table), so the deciding input is
     the identity the client reports;
   one thing makes it hard: the input report layouts differ (different axis/button packing), so input
   would need translation — and it is a reverse-engineering effort against a closed-source app that
   updates often.

## Route matrix

| wheel | input in streamed games | force feedback | notes |
|---|---|---|---|
| G29 / G920 / G923 / PRO | full wheel, officially supported | **yes** | needs G HUB installed + running; reported FH6 rotation tuning pain |
| Driving Force GT | none by default — GFN has no mapping for it, and no firmware persona gives it a whitelisted identity (tested above). Known workaround: masquerade as a gamepad via **Steam Input** (add GFN as a non-Steam game) | **no**, and no software route exists | community-confirmed for FH5 through GFN, explicitly without FFB/rumble |

## What this driver contributes in that setup

- Outside GFN: everything it already does — native mode, 900° range, self-centring spring, constant
  force, full control map, socket API. That is the DFGT's ceiling on macOS; no Mac game drives its
  FFB on its own.
- For the Steam-Input-as-gamepad route the wheel's physical swing matters: `dfgt range 270` (or 180)
  makes a stick's full deflection reachable quickly, and `dfgt ffb autocenter-off` removes the spring
  if it fights the mapping. `dfgt ffb autocenter <n>` puts some feel back.

## Two-minute tests, in order of value

1. **Is the DFGT visible to GFN at all?** Plug it in, open GFN, watch the in-app controller indicator
   or the session's input settings. (The DB audit predicts: no.)
2. **Does Steam Input recognise it?** With the wheel connected: Steam → Settings → Controller. Can it
   be mapped to an Xbox layout? Buildable into a full gamepad path if yes.
3. **Is a whitelisted wheel available to test the official path?** (G29/G920/G923/PRO.)

## Open questions

- Whether Steam Input maps this specific wheel — its local config dir is empty here and its database
  is server-driven, so this cannot be settled from files; test 2 above decides it.
- Whether G HUB is needed merely to put supported wheels into the right mode (plausible, unverified).
- If a G29/G920 is used: is the FH6 rotation problem fixable by setting the wheel's range *locally*?
  Our command set (`f8 81 <deg>`, `fe 0d`, `11 08`, `14`/`f5`) is the same family Linux's `lg4ff`
  uses for the G25/G27/G29/G920, so a local range override is a small extension — worth building only
  if G HUB's own range setting does not already fix it, and only with the hardware present to test.

## Recommendation

For "the wheel, in full, with FH6 through GFN": use a whitelisted wheel. The streaming path exists
and is supported; the DFGT is simply not part of it, and no client-side work changes that. Keep the
DFGT with this driver for local macOS use, where it is the only thing that makes the wheel work at all.
