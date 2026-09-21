# dfgt — Logitech Driving Force GT on macOS

User-space driver for the Logitech Driving Force GT (USB `046d:c29a`). No kext, no DriverKit, no
root, no entitlements: the wheel is opened through IOKit HID and driven with Logitech's own 7-byte
force-feedback protocol, reimplemented from the Linux `hid-lg4ff` driver.

- Puts the wheel in native mode, sets the 40–900° range, and applies a self-centring spring so it
  feels like a wheel instead of a limp joystick.
- Exposes constant force / autocenter / range from the CLI and from a unix socket, for plugins,
  mods and emulators.

It cannot make a game read a wheel it does not support — macOS already exposes the wheel as a plain
joystick (usage 1/4), which SDL2/3 and HID-aware games pick up on their own.

## Install

```
make              # build
make check        # decoder + command-builder tests (no wheel needed)
make install      # ~/.local/bin/dfgt + login agent, no sudo
make uninstall    # unload, neutralise the wheel, remove files
```

`make install PREFIX=/usr/local` works too, if you would rather install system-wide.

## Use

```
dfgt status                    # current wheel state
dfgt watch                     # live decoded input; the tool for identifying controls
dfgt ffb autocenter 0xaaaa     # self-centring spring (what the daemon applies by default)
dfgt ffb autocenter-off
dfgt ffb constant 48           # gentle constant force; 0 or 'ffb off' clears it
dfgt range 200                 # lock-to-lock in degrees (40..900)
dfgt native                    # fix a wheel stuck in 200°/compat mode (no-op today)
dfgt probe                     # element dump + output-report self-test
dfgt watch --hex > tests/capture-controls.hex    # record reports as a regression fixture
dfgt selftest --fixture tests/capture-controls.hex
```

Anything that can open a unix socket can drive the wheel (`$DFGT_SOCKET`, default
`/tmp/dfgt-daemon.sock`):

```
printf 'ffb constant 40\n' | nc -U /tmp/dfgt-daemon.sock     # -> ok constant 40
printf 'state\n'           | nc -U /tmp/dfgt-daemon.sock     # -> ok steer=… throttle=…
```

## Three things worth knowing

1. **The wheel only sends reports when something changes.** `dfgt status` may answer "no input yet"
   until you turn the wheel or press a pedal once. That is the device, not a bug.
2. **The wheel holds a force until told otherwise** — there is no firmware timeout, so `kill -9` on
   the daemon leaves its last force applied. Clear it with `dfgt ffb off` (works with no daemon
   running) or unplug the wheel. The daemon neutralises the wheel on startup and on clean exit.
3. **Only one process should read the wheel.** Opening it from a second process silently stops
   report delivery to the first. Let the daemon own the device and send commands through the socket —
   `dfgt ffb …` does that automatically whenever a daemon is running.

## Troubleshooting

| | |
|---|---|
| daemon log | `/tmp/dfgt-daemon.log` |
| is the agent loaded? | `launchctl list \| grep dfgt` |
| wheel not found | `dfgt probe` (expects `046d:c29a`); `hidutil list \| grep -i 046d` |
| too stiff / too soft | `dfgt ffb autocenter 0x4000` … `0xffff`, or `autocenter-off` |
| no FFB in a game | most Mac games ignore wheels entirely; check whether the game sees the joystick at all |

Design notes, verified device facts, the FFB byte table and the acceptance tests live in
[PLAN.md](PLAN.md).
