# dfgt-ble-host — Stage 1

One ESP32-S3 **hosts** the Logitech Driving Force GT over USB, decodes it, and shows
macOS a **BLE HID gamepad**. Input only: no force feedback, no wheel/G29 identity, no
extra hardware. Stage 2 replaces the gamepad output with wheel emulation — that is why
the code is split the way it is (`module map` below).

## Why BLE and not USB

The S3 has a single USB peripheral (GPIO19/20 are shared between the USB-OTG controller
and the USB-Serial-JTAG bridge), so a board that *hosts* the wheel cannot also be a USB
device to the Mac. macOS offers no user-space virtual HID either (`PLAN.md` §7, gate G1,
closes that door), so the Mac-facing side has to be a real radio device. BLE HID is the
one that needs no second board.

## Status

- **Builds clean**: `pio run`, image 0.81 MB, fits `partitions.csv` (4 MB factory slot).
- **Verified without hardware**: report-map/bit-count assertions, decode rules carried from
  the macOS driver, the IDF 4.4 API facts listed under *Findings*.
- **Not yet verified** (needs the board + wheel): enumeration, the report stream, BLE
  pairing and client acceptance. The validation protocol below is the list of open questions.
- **Deliberately out of scope**: force feedback, G29/wheel emulation, DriverKit/CoreHID,
  browser input, a second radio board.

## Hardware

| Port on the board | Role |
|---|---|
| **Native USB** (the "USB" port, GPIO19/20) | **host** for the wheel — must be fed 5 V VBUS |
| **CH343 / UART port** | console (115200) + flashing + power for the board |

1. Feed **5 V to the wheel's VBUS**. On a DevKitC-1 both USB-C sockets are diode-isolated
   *into* the 5 V rail, so the board cannot push 5 V back out of its own connector: wire the
   board's `5V` pin to the host socket's VBUS (pin 1 of a USB-A breakout, or the VBUS pad of
   a USB-C→A adapter). Measure the DFGT's draw while steering — it is logic-only (the motors
   use the 24 V brick), so a couple of hundred milliamps, but the Mac's 500 mA budget has to
   cover the board *and* the wheel.
2. Wheel into the board's native port, board into the Mac via the CH343 port.
3. **Never connect two 5 V sources at once** (Mac on the UART port *and* an external 5 V feed).
4. The 24 V brick is only needed for force feedback; input works without it.

## Build, flash, monitor

```sh
cd firmware/dfgt-ble-host
pio run                       # build (this is the gate: clangd cannot model Xtensa, see .clangd)
pio run -t upload             # flash over the CH343 port, 115200
pio device monitor            # console, 115200  (or: screen /dev/cu.usbmodem* 115200)
```

For editor diagnostics, refresh the index after a build change:

```sh
pio run -t compiledb && python3 tools/strip_xtensa_flags.py
```

The tool exists because PlatformIO writes the real ESP-IDF flags into
`compile_commands.json`, and clangd rejects the Xtensa-only ones (`-mlongcalls`, …) at
driver level — before any per-file suppression can apply — so every firmware file reports
bogus errors that hide real ones. It edits only the index editors read; the firmware build
keeps its flags.

If `pio run -t upload` fights the auto-reset circuit, flash the three images explicitly
(same command the Arduino firmware uses, minus `boot_app0.bin` — this partition table has
no OTA slot):

```sh
PY=/opt/homebrew/Cellar/platformio/6.1.19/libexec/bin/python
ES=~/.platformio/packages/tool-esptoolpy/esptool.py
$PY $ES --chip esp32s3 --port /dev/cu.usbmodem59090597741 --baud 115200 write_flash -z \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0000  .pio/build/esp32-s3/bootloader.bin \
    0x8000  .pio/build/esp32-s3/partitions.bin \
    0x10000 .pio/build/esp32-s3/firmware.bin
```

Flashing **replaces** the Arduino gadget firmware in `../g29-gadget` (that one is what
GeForce NOW accepted over USB). Restore it with `pio run -d firmware/g29-gadget -t upload`
when you need the wired G29 presentation back.

## Console

UART0, 115200. Defaults: raw + decoded printing on, so the first plug-in produces evidence.

| key | action |
|---|---|
| `h` | help |
| `i` | status: USB state, ids, endpoints, counters, BLE state |
| `d` | dump the captured HID report descriptor again |
| `x` | toggle raw 8-byte report printing |
| `w` | toggle decoded state printing |
| `range <40..900>` | send the wheel's range command (configuration, not force feedback) |

## Validation protocol — in this order

1. **BLE alone (wheel unplugged).** Power the board; the log must show
   `advertising as "DFGT Pad" (303a:4001)`. Pair it in macOS *System Settings → Bluetooth*,
   expect `connected to a host`, and check the Mac sees a gamepad:
   `hidutil list | grep -i dfgt`
   This is the first open question: does a *generic* BLE gamepad show up as a controller?
2. **Wheel attach.** Expect the descriptor dump (ids, speed, strings), the interface/endpoint
   list, `HID report descriptor (… bytes)`, `range 900 deg applied`, then raw + decoded lines
   as you move things.
3. **Re-confirm the layout** (it is a hypothesis carried from the macOS driver until here):
   turn full left → `steer=0`, full right → `steer=16383`; press every control and compare the
   `pressed=` names with the control map in `PLAN.md` §5; pedals should read 255 released.
4. **GeForce NOW.** Start a stream and watch the log for the client picking up the pad, and
   `host wrote output report` lines if it ever writes to us. Acceptance by the native client
   is the acceptance test for Stage 1 — record whatever it says, including "it ignored it".

## Findings

Discovered while building this (toolchain and API, not hardware):

- **Arduino-ESP32 2.x has no USB host stack**, so this project is ESP-IDF; the two firmware
  projects coexist because they use the same single USB port in opposite roles.
- **IDF 4.4 ships no HID class driver** for the host (`components/usb` has only the library),
  so the HID-specific work here is: find the HID interface by class, claim it,
  `GET_DESCRIPTOR(Report)` over a control transfer, then self-resubmitting interrupt-IN.
- **`esp_hid`'s BLE device path needs Bluedroid in 4.4** (NimBLE is not wired up), and the S3
  has no classic BT, so `CONFIG_BT_CLASSIC_ENABLED` is impossible there — the BLE-only build
  compiles cleanly.
- **The report characteristic carries the report with no report-ID byte** in this esp_hid
  version (`value_len = bits/8`), so the report map here deliberately declares **no report ID**;
  adding one would shift every field by a byte.
- **PlatformIO's ESP-IDF builder reads `src_dir` from `[platformio]`, not `[env]`**, and the
  USB-Serial-JTAG console must stay disabled (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n`) because it
  shares the PHY with the host port.
- **A blocking bug found and fixed before flashing**: `attach()` runs in the USB *client* task,
  which is also the only task that dispatches transfer callbacks, so waiting on a transfer
  completion there waits for itself (500 ms timeout, then a bogus failure). The report-descriptor
  fetch is now fire-and-forget and the range write is applied by `app_main` once the interface
  reports `claimed`.
- **Two clangd artifacts, both provably benign.** The Xtensa-only flags (fixed for editors by
  `tools/strip_xtensa_flags.py`) and one include clangd cannot resolve: IDF's own
  `components/xtensa/esp32s3/include/xtensa/config/core.h` includes `../hal.h`, which GCC finds via
  its include paths and clangd does not. Neither says anything about this firmware — `pio run` is
  green, which is why the directory's gate is the build (`.clangd`).

## Blockers and open risks

1. **VBUS wiring** — nothing enumerates until the host port is fed 5 V (see Hardware).
2. **Client acceptance of a generic BLE gamepad** — unverified, and the most likely thing to
   fail. If the native client ignores it, the fix is to claim a known gamepad's ids in
   `ble_gamepad.h` (one line) and re-flash; the descriptor and mapping stay as they are.
3. **Rate**: the link is bounded by the BLE connection interval (7.5–15 ms ⇒ ~66–133 Hz) versus
   ~500 Hz on the wired gadget. Airtime is not the limit; the interval is.
4. **Input-only decode is a hypothesis** until step 3 confirms it on this hardware.
5. The wheel **reverts to ~200° on every replug** (`PLAN.md` §3 fact 9) — hence the range
   command at attach. Applied by `app_main`, not by the USB module: transport and policy stay
   separate.

## Module map

| file | owns |
|---|---|
| `main/dfgt_usb.c` | USB host transport: enumeration, descriptors, claim, interrupt-IN polling, one vendor-OUT write |
| `main/dfgt_decode.c` | the 8-byte report layout and the 7-byte range command (hypothesis + evidence log) |
| `main/pad_map.c` | what the host sees: wheel → gamepad mapping (the Stage 2 seam) |
| `main/ble_gamepad.c` | how it is encoded and announced: report map, appearance, identity, BLE HID glue |
| `main/app_main.c` | wiring, console, discovery logging, rate limiting, range policy |
