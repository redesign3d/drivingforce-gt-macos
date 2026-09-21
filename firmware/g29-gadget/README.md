# g29-gadget — ESP32-S3 posing as a Logitech G29

Milestone 1 of the GeForce NOW route ([docs/geforce-now.md](../../docs/geforce-now.md)): make the
ESP32-S3 present itself to macOS as a **Logitech G29** (`046d:c24f`, "G29 Driving Force Racing
Wheel"), so GFN's whitelist is satisfied by identity instead of by the client's table.

Only the USB identity is a G29's. The report descriptor is the **Driving Force GT's own**, captured
from the real wheel — an 8-byte input report plus the 7-byte vendor `0xFF00/0x02` OUTPUT report that
carries Logitech's force-feedback command set.

## Why this shape

The S3 has a **single USB PHY** (USB-OTG and USB-Serial/JTAG share D+/D−), so it cannot be host for
the wheel *and* device for the Mac at the same time. The wheel therefore stays on the Mac, with the
`dfgt` daemon relaying state and force feedback over the board's UART bridge (milestone 2):

```text
[DFGT]--USB--[Mac]                   dfgt daemon owns the real wheel
                |  native USB  ->    this firmware: HID 046d:c24f
                +  UART bridge ->   state out / FFB in (milestone 2)
```

## What it answers

The decisive question is whether a GFN session **starts sending wheel force feedback** to a device
that merely claims to be a G29. Every incoming report is dumped to UART0 as

```text
[ffb] SET_REPORT id=0 len=7: f8 81 84 03 00 00 00
      -> lg4ff set range 900 deg
```

Any `[ffb]` line during a streaming session means the whitelist is identity-only and the whole
approach is viable. No output at all means GFN's wheel path needs something else (a real G-Series
descriptor, or Logitech's own driver present).

## Build, flash, watch

```sh
pio run                                     # build
pio run -t upload --upload-port /dev/cu.usbmodemXXXX   # flash via the CH343 bridge port
pio device monitor -p /dev/cu.usbmodemXXXX  # or: python3 -c 'import serial; ...'
```

The **native** USB port must be plugged into the Mac for HID to appear; the **bridge** port is for
flashing and for the log. After flashing, macOS should show `046d:c24f` with the product name
"G29 Driving Force Racing Wheel" (`hidutil list`, or `./build/dfgt --pid 0xc24f probe`).

## Bring-up notes (Waveshare ESP32-S3-WROOM-1 N16R8, 2026-09)

Flashing from the Mac over the board's CH343 bridge port (`/dev/cu.usbmodem…`) currently **fails on
writes**, in a very specific way:

| operation | result |
|---|---|
| `flash_id` (read) | ✅ 16MB detected, quad, 3.3V per eFuse |
| `read_flash` 256 B | ✅ correct bytes |
| `erase_flash` | ✅ reports success |
| `write_flash` | ❌ `Requested resource not found` (01050000) after 0% — at 115200, 57600 and 460800, with and without the stub, one file or all four |
| stub upload (RAM write) | ❌ `Operation timed out` (01070000) |
| `--before no_reset` | ❌ cannot connect at all, so the reset lines do work |

Reads and erases working while *every* write fails points at the flash write path rather than the
serial link: power sag during writes, signal integrity on the quad data lines, or a flash chip the
ROM cannot write. Next attempts, in order:

1. flash over the **native USB port** (USB-Serial-JTAG) — also lets the RAM stub load, and adds a
   second power path; that port has to be connected for the HID test anyway;
2. a different cable and a direct Mac port instead of a hub;
3. if writes still fail everywhere, treat the module as faulty rather than debug further.

## Regenerating the descriptor

`src/wheel_descriptor.h` is generated, never hand-edited:

```sh
./tools/make_descriptor.py < tools/dfgt-descriptor.hex
```

It re-derives the report layout from the bytes and refuses to write a header unless the descriptor
yields exactly an 8-byte input, 7-byte output and 131-byte feature report. `tools/dfgt-descriptor.hex`
is the capture from the real wheel; a fresh dump from the device can be diffed against it.
