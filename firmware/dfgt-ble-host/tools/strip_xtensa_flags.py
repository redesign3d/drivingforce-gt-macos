#!/usr/bin/env python3
"""Strip GCC/Xtensa-only flags from compile_commands.json.

PlatformIO writes the real ESP-IDF build flags into compile_commands.json. clangd
cannot emulate the Xtensa target, so it rejects -mlongcalls and friends at driver
level — before any per-file diagnostic suppression can help — and every firmware
file then reports three bogus errors that hide real ones.

This edits only the index editors read; the firmware build is untouched (it keeps
using the flags from sdkconfig/CMake). Run it after generating the index:

    pio run -t compiledb && python3 tools/strip_xtensa_flags.py

The gate for this directory is still the real build: `pio run`.
"""
import json
import pathlib
import shlex

XTENSA_ONLY = {
    "-mlongcalls",
    "-fno-shrink-wrap",
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-fno-jump-tables",
}


def main() -> int:
    db = pathlib.Path(__file__).resolve().parents[1] / "compile_commands.json"
    if not db.is_file():
        print(f"{db} not found — run: pio run -t compiledb")
        return 1

    entries = None
    try:
        entries = json.loads(db.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        # A half-written index (interrupted build) is not a reason to traceback.
        print(f"{db.name}: cannot read the compilation database ({exc}) — rebuild it with:\n"
              f"  pio run -t compiledb")
        return 1
    changed = 0

    for entry in entries:
        if "command" in entry:
            parts = shlex.split(entry["command"])
            kept = [p for p in parts if p not in XTENSA_ONLY]
            if kept != parts:
                entry["command"] = " ".join(shlex.quote(p) for p in kept)
                changed += 1
        elif "arguments" in entry:
            kept = [a for a in entry["arguments"] if a not in XTENSA_ONLY]
            if kept != entry["arguments"]:
                entry["arguments"] = kept
                changed += 1

    db.write_text(json.dumps(entries, indent=1) + "\n")
    print(f"{db.name}: stripped Xtensa-only flags from {changed} of {len(entries)} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
