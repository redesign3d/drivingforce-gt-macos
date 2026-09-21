#!/usr/bin/env python3
"""Generate src/g29_descriptor.h - the report descriptor our fake wheel presents.

Why not the Driving Force GT's own descriptor? Because the cloud matched the *identity*
(046d:c24f) as a G29 and parses the raw reports with G29 semantics, and the two differ:

    field      a real G29 (lg4ff family, measured by LogiWheelHost on hardware)
    buttons    bytes 0..3
    steering   bytes 4..5, 16-bit, centre ~32768
    throttle   byte 6, uint8, idle 255
    brake      byte 7, uint8, idle 255
    clutch     byte 8, uint8, idle 255   <- our DFGT-shaped report had no such byte

The DFGT presents 14-bit steering centred at 8192, so a G29 reader sees its centre as
~12% of full scale (hard left) - and the report ran one byte short. The FFB path is
unchanged: a 7-byte vendor 0xFF00/0x02 OUTPUT report, which is what GeForce NOW's client
already writes to us successfully.

Run:  ./tools/make_g29_descriptor.py
"""

import os
import sys

# --- declarative layout: (name, size_bits, count) ------------------------------
INPUT_FIELDS = [
    ("hat", 4, 1),          # Generic Desktop 0x39, logical 0..7, null when centred
    ("buttons", 1, 25),     # Button page, usage 1..25
    ("pad", 3, 1),          # byte-align: 4 + 25 + 3 = 32 bits = bytes 0..3
    ("steer", 16, 1),       # Generic Desktop X, logical 0..65535, centre 32768
    ("throttle", 8, 1),     # Y, 0..255, 255 = released
    ("brake", 8, 1),        # Z
    ("clutch", 8, 1),       # Rz
]
EXPECTED_INPUT_BITS = 72    # 9 bytes
EXPECTED_OUTPUT_BITS = 56   # 7 bytes, the FFB channel


def build() -> bytes:
    d = bytearray()
    d += bytes([0x05, 0x01, 0x09, 0x04, 0xA1, 0x01])            # Usage Page GD, Joystick, Collection
    # hat
    d += bytes([0x05, 0x01, 0x09, 0x39])                        #   Usage Page GD, Hat switch
    d += bytes([0x15, 0x00, 0x25, 0x07])                        #   Logical 0..7
    d += bytes([0x35, 0x00, 0x46, 0x3B, 0x01])                  #   Physical 0..315
    d += bytes([0x65, 0x14])                                    #   Unit: degrees
    d += bytes([0x75, 0x04, 0x95, 0x01, 0x81, 0x42])            #   Size 4, Count 1, Input (Null state)
    d += bytes([0x65, 0x00])                                    #   Unit: none
    # buttons 1..25
    d += bytes([0x05, 0x09, 0x19, 0x01, 0x29, 0x19])            #   Usage Page Button, 1..25
    d += bytes([0x15, 0x00, 0x25, 0x01])                        #   Logical 0..1
    d += bytes([0x75, 0x01, 0x95, 0x19, 0x81, 0x02])            #   Size 1, Count 25, Input
    # padding to the end of byte 3
    d += bytes([0x75, 0x03, 0x95, 0x01, 0x81, 0x01])            #   Size 3, Count 1, Input (Const)
    # steering, 16-bit
    d += bytes([0x05, 0x01, 0x09, 0x30])                        #   Usage X
    d += bytes([0x16, 0x00, 0x00, 0x26, 0xFF, 0xFF])            #   Logical 0..65535
    d += bytes([0x75, 0x10, 0x95, 0x01, 0x81, 0x02])            #   Size 16, Count 1, Input
    # pedals: throttle (Y), brake (Z), clutch (Rz), 8-bit each
    d += bytes([0x26, 0xFF, 0x00])                              #   Logical 0..255
    d += bytes([0x05, 0x01, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35])  #  Usage Y, Z, Rz
    d += bytes([0x75, 0x08, 0x95, 0x03, 0x81, 0x02])            #   Size 8, Count 3, Input
    # FFB output report, unchanged from the DFGT (what GFN already drives us with)
    d += bytes([0x06, 0x00, 0xFF, 0x09, 0x02])                  #   Usage Page vendor 0xFF00, Usage 2
    d += bytes([0x75, 0x08, 0x95, 0x07, 0x91, 0x02])            #   Size 8, Count 7, Output
    d += bytes([0xC0])                                          # End Collection
    return bytes(d)


def report_bits(descriptor: bytes) -> dict:
    """Walk the items and total up the bits per report kind, as a self-check."""
    i, size, count, totals = 0, 0, 0, {}
    kinds = {8: "input", 9: "output", 11: "feature"}
    while i < len(descriptor):
        prefix = descriptor[i]
        i += 1
        if prefix == 0xFE:                              # long item
            skip = descriptor[i]
            i += 2 + skip
            continue
        size_code = prefix & 0x03
        item_len = 4 if size_code == 3 else size_code
        item_type = (prefix >> 2) & 0x03
        tag = (prefix >> 4) & 0x0F
        value = int.from_bytes(descriptor[i:i + item_len], "little")
        i += item_len
        if item_type == 1 and tag == 7:
            size = value
        elif item_type == 1 and tag == 9:
            count = value
        elif item_type == 0 and tag in kinds:
            kind = kinds[tag]
            totals[kind] = totals.get(kind, 0) + size * count
    return totals


def main() -> int:
    descriptor = build()
    totals = report_bits(descriptor)
    if totals.get("input") != EXPECTED_INPUT_BITS or totals.get("output") != EXPECTED_OUTPUT_BITS:
        print(f"refusing to write: got {totals}, expected input={EXPECTED_INPUT_BITS} "
              f"output={EXPECTED_OUTPUT_BITS}", file=sys.stderr)
        return 1

    here = os.path.dirname(os.path.abspath(__file__))
    header = os.path.join(here, "..", "src", "g29_descriptor.h")
    body = "\n".join("    " + " ".join(f"0x{b:02x}," for b in descriptor[j:j + 10])
                      for j in range(0, len(descriptor), 10))
    try:
        with open(header, "w", encoding="utf-8") as handle:
            handle.write(f"/* Generated by tools/make_g29_descriptor.py - do not hand-edit.\n"
                         f" * A G29-shaped wheel: {totals['input']}-bit input report "
                         f"({totals['input'] // 8} bytes) and the 7-byte\n"
                         " * vendor FFB output report. Field layout:\n"
                         " *   buttons 0-3, steering 16-bit (centre 32768) 4-5,\n"
                         " *   throttle 6, brake 7, clutch 8. */\n")
            handle.write("static const uint8_t g29_report_descriptor[] = {\n")
            handle.write(body + "\n};\n")
    except OSError as err:
        print(f"cannot write {header}: {err}", file=sys.stderr)
        return 1

    print(f"wrote {header} ({len(descriptor)} bytes), layout {totals}, "
          f"input {totals['input'] // 8} bytes, output {totals['output'] // 8} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
