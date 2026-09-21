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

## What the client actually does with this wheel (audited)

- GFN holds the wheel open at **both** levels: a USB device user client and a HID client
  (`IOHIDLibUserClient`, creator `pid …, GeForceNOW`). So its custom HID layer really does pick the
  wheel up — it is not simply invisible.
- Its input layer is a hand-rolled `IOHIDManager` stack (device matching, element parsing,
  `IOHIDDeviceSetReport` for output) alongside SDL2, plus a joystick element-map / remap layer
  (`JOYSTICK_parseElementMap`, `JOYSTICK_loadRemapInfo`, `JOYSTICK_dumpDeviceMapping`) and identity
  fields (`vendorId`, `productId`, `productName`, `deviceId`, `controllerCategory`, `devicesConnected`).
- The UI bundle carries a device-category enum containing **`WHEEL`** next to `X_INPUT_GAMEPAD`,
  `DIRECT_INPUT_GAMEPAD`, `JOYSTICK`, `FLIGHT_CONTROLS`; it reports
  `{inputDevice, manufacturer, versionNumber}`. So the protocol has a wheel concept — but nothing in
  the client maps our wheel to it: **no wheel PIDs and no wheel names are compiled in**. Which
  identity counts as a supported wheel is therefore decided outside the client (server-side list);
  the client's part is only to report the device it sees.
- The client *does* contain Logitech wheel command code — `f8 81 …` (range), `fe 0d …` (autocenter),
  `f8 09 …` (mode switch), i.e. the same `lg4ff` dialect this wheel speaks — but no per-frame
  constant-force shape, so torque most likely arrives as opaque bytes from the cloud.

## Software levers on the macOS side (tested — closed)

`hidutil property --set` accepts arbitrary properties for the wheel's HID *event service*
(registry `1001e132a`): setting `Product` to "G29 Driving Force Racing Wheel", then `ProductID` to
`0xc24f`, both reported success — and changed **nothing** that clients see. `IOHIDDeviceGetProperty`
(the API GFN uses) still returned `Product = Driving Force GT`, the device still matched `046d:c29a`
and did *not* answer as `c24f`. Identity comes from the USB descriptors, so no macOS-side property
override can satisfy the whitelist. (`dfgt probe` now prints the identity exactly as clients see it,
which is what made this test conclusive.)

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
   updates often. **Ruled out by the owner: no GFN client modifications.**
3. **A wheel-side USB proxy — the only route that spoofs identity without touching GFN.** A board that
   is both USB host (for the wheel) and USB device (for the Mac) can present a G29 to macOS:
   gadget HID with `idVendor=046d`, `idProduct=c24f`, product string "G29 Driving Force Racing Wheel"
   and a wheel-shaped report descriptor. GFN then sees a whitelisted wheel and enables wheel mode;
   the FFB it sends down is the shared `lg4ff` dialect this wheel already understands, so the proxy
   can largely forward it, while re-shaping the 8-byte input report into G29-shaped axes/buttons.
   **Hardware check (ESP32-S3-WROOM-1 N16R8, Waveshare).** The S3 has a *single* USB PHY: the USB-OTG
   peripheral and the USB-Serial/JTAG controller share the same D+/D- pins, so it cannot be host for
   the wheel and device for the Mac at the same time — no transparent two-port proxy. It is still
   right for a **relay** design, because these boards also carry a WCH UART bridge on its own USB port
   (seen as `1a86:55d3`, `/dev/cu.usbmodem…`), giving a second, independent USB connection to the Mac:

   ```text
   [DFGT]--USB--[Mac]                    dfgt daemon keeps owning the real wheel
                    |  native USB  ->   S3 presents HID 046d:c24f "G29 Driving Force Racing Wheel"
                    +  UART bridge ->  S3 <-> daemon: wheel state out, FFB commands in
   ```

   Milestones: (1) the S3 presents that identity plus a wheel-shaped report descriptor with neutral
   input, and we watch whether a GFN session starts sending wheel FFB to it — that is the decisive
   test of the identity-only assumption, and it is machine-observable as HID output/SET_REPORT
   traffic; (2) the relay (daemon streams wheel state to the S3, forwards received FFB to the wheel,
   nearly 1:1 since the dialect is shared).
   Unverified assumption: that the whitelist is identity-only. No G HUB code exists in the client,
   which is why identity-only is the better bet — but if GFN's wheel path implicitly also needs
   Logitech's driver present, a spoofed identity would not be enough.

## Result: the whitelist is identity-only — verified end to end (2026-09-21)

The ESP32-S3 posing as `046d:c24f` — with the **Driving Force GT's own** report descriptor — is accepted
by GeForce NOW as a Logitech G29, in the cloud as well as locally:

- **FH5 (streamed through GFN) detects the wheel** and offers its "LOGITECH G29" wheel layout, with a
  full preset bound to the device's actual elements: steering / clutch / accelerate / brake on axes
  1-4, the hat as `SWITCH 1` up/down/left/right, and buttons up to 19 (gears 1-6 on 13-18, horn on
  12, shift up on 5, reverse on 19).
- **Force feedback arrives.** The firmware counts every FFB report it receives and reports the count
  back in its input report's unused vendor bits: `0x04 -> 0x10` across the session — twelve wheel
  commands from the cloud to a device that merely *claims* to be a G29. Over 10 s idle the counter
  does not move, so the traffic is session-driven rather than client chatter.
- The host→device path was verified separately: sending `11 08 94 80 …` then `13 00 …` from the Mac
  moved the counter `0x01 -> 0x02 -> 0x03`.

Two things follow for the relay:

1. The FFB the cloud sends is the `lg4ff` dialect this wheel already speaks, so forwarding it to the
   real Driving Force GT should be close to a pass-through (the same `f8 81` range, `fe 0d`/`14`
   autocenter and `11 08`/`13` constant-force commands).
2. The game's G29 preset expects **four axes in the order steering, clutch, throttle, brake**, while
   the DFGT descriptor has three (X steering, Y throttle, Z brake). Because the relay synthesises the
   input report, it can present whichever layout the preset expects — steering, empty clutch,
   throttle, brake — and the default mapping then lines up with no remapping in-game.

## The wire format has to match a real G29, not just the identity (2026-09-21)

Matching the USB identity got the cloud to whitelist the device (FH5 applies its G29 preset and
force feedback flows), but **input did not reach the game**. GFN's client log shows why the plumbing
is fine and the *bytes* are not:

```
[GIOInterface] Enabling GSHID
[HIDDevicesController] GSHID: Supporting {046D:C24F}                 <- our identity is whitelisted here
[HIDDevicesController] Plugging 046D:C24F:0100                        <- plugged, id 25
[NVST:RiClientBackend] Sending HID Change event: control=1, ID=10, 046D:C24F:0100
[HIDDevice] start: Beginning HID reading loop for 046D:C24F:0100 25   <- raw reports are read and forwarded
```

So the client forwards our device's **raw reports**, and the cloud parses them with G29 semantics
(that is where the identity led it). Our fake device was sending the Driving Force GT's own layout,
which differs in ways that matter:

| field | a real G29 (lg4ff family, measured on hardware by LogiWheelHost) | the DFGT layout we were sending |
|---|---|---|
| buttons | bytes 0-3 | bytes 0-3 ✅ |
| steering | bytes 4-5, **16-bit, centre 32768** | bytes 4-5, **14-bit, centre 8192** ❌ (centre reads as ~12% = hard left) |
| throttle / brake | bytes 6 / 7, uint8, idle 255 | the same ✅ |
| clutch | **byte 8** | **absent** - the report ran one byte short ❌ |
| `bcdDevice` | 0x1350 (Linux's lg4ff ident mask) | whatever the stack defaults to ❌ |

Our fake device now presents a G29-shaped wheel: `tools/make_g29_descriptor.py` generates a
*9-byte* input report (hat + 25 buttons in bytes 0-3, steering 16-bit at 4-5, throttle 6, brake 7,
clutch 8) plus the unchanged 7-byte vendor FFB output report, and the relay assembles that layout
from the real wheel (its 14-bit axis scaled by four, so centre 8192 -> 32768). `bcdDevice` is 0x1350.

That change is deliberately **self-consistent under either interpretation**: the descriptor declares
16-bit steering and the payload sends 16-bit steering, so a reader that trusts our descriptor and a
reader that assumes G29 semantics agree on the same values. Verified locally: the fake device reads
back as `usage=0x30 value=28004`, exactly four times the real wheel's 7001, with the clutch axis
present and `bcdDevice = 0x1350`.

## Route matrix

| wheel | input in streamed games | force feedback | notes |
|---|---|---|---|
| G29 / G920 / G923 / PRO | full wheel, officially supported | **yes** | needs G HUB installed + running; reported FH6 rotation tuning pain |
| Driving Force GT | **mapped as a G29** by the ESP32-S3 identity bridge — FH5 in GFN detects it and its wheel layout binds to the device's elements | **yes** — the cloud sends wheel FFB to the bridge (12 commands in one session) | milestone 2 forwards that FFB to the real wheel; the older workaround (gamepad via Steam Input, no FFB) is no longer needed |

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
