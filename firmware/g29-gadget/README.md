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
pio run                                                    # build
pio run -t upload --upload-port /dev/cu.usbmodemXXXX       # flash over the NATIVE USB port
```

**Which port:** flash over the board's **native USB port** (`303a:1001`, USB-Serial-JTAG). The CH343
UART bridge port (`1a86:55d3`) enumerates and reads fine but *cannot write flash* on this board — see
the bring-up notes. The native port is also the one that presents the HID wheel, so it is the cable
that has to be attached anyway.

**The reflash trap:** the S3 has a single USB PHY, so while this firmware runs the HID device owns
the native port and the USB-Serial-JTAG port **does not exist** — esptool has nothing to talk to.
Two ways back into the ROM bootloader:

```sh
# 1. ask the firmware (it knows the REBOOT! magic output report):
./build/dfgt --pid 0xc24f probe --cmd 5245424f4f5421
# 2. or hold BOOT and tap RESET (replug while holding BOOT also works)
```

Once the ROM is waiting, `/dev/cu.usbmodem*` reappears and flashing works. Logs go to **UART0** (the
CH343 bridge port) through the ESP-IDF console, so that cable is needed only when you want the text.

### Getting back into download mode without buttons

If the BOOT/RESET sequence does not take (GPIO0 has to be low *at* the reset, which is easy to miss),
the CH343 bridge can do it deterministically: its DTR/RTS lines are wired to EN/GPIO0, which is why
esptool can reset the chip there. That bridge cannot *write* flash on this board, but it can *enter*
download mode - and in download mode the ROM brings the native USB-Serial-JTAG port back:

```sh
# 1. enter download mode over the UART bridge and leave the chip waiting (--after no_reset)
esptool.py --chip esp32s3 --port /dev/cu.usbmodem<CH343> --before default_reset --after no_reset flash_id
# 2. the native port re-enumerates; flash over it
cd firmware/g29-gadget && pio run -t upload --upload-port /dev/cu.usbmodem<native>
```

**Both cables are needed** for this (the bridge to reset, the native port to flash). Once the current
firmware is in, neither is needed again: the `REBOOT!` magic report above does step 1.

**Checking the decisive question with no serial cable at all:** the input report's unused vendor bits
carry the number of force-feedback reports received (0 = none yet), so FFB arriving during a GFN
session can be read straight off the device from the Mac:

```sh
./build/dfgt --pid 0xc24f watch           # vendor7 column: 0x00, then counts up when FFB arrives
./build/dfgt --pid 0xc24f probe --values  # same, read directly instead of waiting for a report
```

## Bring-up notes (Waveshare ESP32-S3-WROOM-1 N16R8, 2026-09)

**Flashing works over the native USB port** (`303a:1001`, USB-Serial-JTAG): 314352 bytes written,
hash verified. It is the CH343 UART bridge port (`1a86:55d3`) that fails, in a very specific way:

| operation over the CH343 bridge | result |
|---|---|
| `flash_id` (read) | ✅ 16MB detected, quad, 3.3V per eFuse |
| `read_flash` / `erase_flash` | ✅ correct bytes / reports success |
| `write_flash` | ❌ `Requested resource not found` (01050000) after 0% — at 115200, 57600 and 460800, with and without the stub, one file or all four |
| stub upload (RAM write) | ❌ `Operation timed out` (01070000) |
| `--before no_reset` | ❌ cannot connect at all, so the reset lines do work |

Reads and erases working while *every* write fails means the bridge's write path (or the USB port it
is plugged into) is the problem, not the module. Use the native port; if the CH343 is ever needed,
try another cable and a direct Mac port first.

Note also that `pio run -t upload` cannot be used while the HID firmware runs — see the reflash trap
above. Build with PlatformIO; that is the gate for this directory (the `ignore` entry in
`.pi-lens.json` exists because host-side linters cannot parse ESP32 Arduino code without the Xtensa
toolchain).

## Regenerating the descriptor

`src/wheel_descriptor.h` is generated, never hand-edited:

```sh
./tools/make_descriptor.py < tools/dfgt-descriptor.hex
```

It re-derives the report layout from the bytes and refuses to write a header unless the descriptor
yields exactly an 8-byte input, 7-byte output and 131-byte feature report. `tools/dfgt-descriptor.hex`
is the capture from the real wheel; a fresh dump from the device can be diffed against it.
