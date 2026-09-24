<!-- Improved compatibility of back to top link: See: https://github.com/othneildrew/Best-README-Template/pull/73 -->
<a id="readme-top"></a>

<!-- PROJECT SHIELDS -->
[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]
[![Platform][platform-shield]][platform-url]

<br />
<div align="center">
  <h3 align="center">drivingforce-gt-macos</h3>

  <p align="center">
    A Logitech Driving Force GT that macOS actually drives — and that GeForce NOW accepts.
    <br />
    <a href="PLAN.md"><strong>Explore the design notes »</strong></a>
    <br />
    <br />
    <a href="https://github.com/redesign3d/drivingforce-gt-macos/issues/new?labels=bug">Report Bug</a>
    &middot;
    <a href="https://github.com/redesign3d/drivingforce-gt-macos/issues/new?labels=enhancement">Request Feature</a>
  </p>
</div>

<!-- TABLE OF CONTENTS -->
<details>
  <summary>Table of Contents</summary>
  <ol>
    <li>
      <a href="#about-the-project">About The Project</a>
      <ul>
        <li><a href="#built-with">Built With</a></li>
      </ul>
    </li>
    <li>
      <a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#installation">Installation</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#roadmap">Roadmap</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>

<!-- ABOUT THE PROJECT -->
## About The Project

```text
                  +---------------- Mac ----------------+        +------- ESP32-S3 -------+
   [Driving       |  dfgt daemon / relay                |        |  g29-gadget firmware   |
    Force GT]--USB|   owns the wheel, decodes it,       |  UART  |  presents 046d:c24f    |--USB--> macOS
   046d:c29a      |   writes Logitech FFB commands      |<------>|  ("G29 Driving Force") |        (GFN app)
                  +-------------------------------------+        +------------------------+
```

Two pieces, one goal — using the wheel in full on macOS:

1. **`dfgt`** — a user-space driver for the Driving Force GT. No kext, no DriverKit, no root, no
   entitlements: the wheel is opened through IOKit HID and driven with Logitech's own 7-byte
   force-feedback protocol, reimplemented from the Linux `hid-lg4ff` driver. It puts the wheel in
   native mode, sets the 40–900° range and applies a self-centring spring, so the wheel resists and
   self-centres instead of hanging limp. Constant force, autocenter and range are available from the
   CLI and from a unix socket, for plugins, mods and emulators.
2. **`g29-gadget`** — an ESP32-S3 that claims to be a **Logitech G29** (`046d:c24f`) so that
   GeForce NOW's wheel whitelist (G29/G920/G923/PRO) is satisfied by identity. The DFGT stays on the
   Mac; the relay streams its state to the board and forwards force feedback back to the wheel.

**What works today:** local force feedback (verified on hardware — commanding a constant force
physically moves the steering axis), every control identified and decoded, and in GeForce NOW
**the wheel is detected and plays as a wheel** (the streamed Forza Horizon 5 offers its G29 layout).

**What does not:** force feedback inside GeForce NOW. The macOS client gates FFB on Apple's
ForceFeedback framework, and on this machine that framework cannot grant FFB to *any* device — 0 of
19 HID devices pass, including the real wheel, and no plug-in provider exists for it to load.
Reimplementing Logitech's protocol on the device side does not change that; the evidence, the decoded
client behaviour and the closed routes are written up in [docs/geforce-now.md](docs/geforce-now.md).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

### Built With

* [C][c-url] — the driver is one binary, CoreFoundation + IOKit HID only, no third-party libraries
* [IOKit HID][iokit-url] — device access without kext, DriverKit or entitlements
* [PlatformIO][platformio-url] + [Arduino-ESP32][arduino-url] — the ESP32-S3 firmware
* [TinyUSB][tinyusb-url] — custom HID report descriptor and the vendor FFB output report
* [launchd][launchd-url] — login agents for the daemon and the relay

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- GETTING STARTED -->
## Getting Started

### Prerequisites

* macOS on Apple Silicon (verified on macOS 26.5.2, arm64)
* A Driving Force GT (`046d:c29a`), and the 24 V brick for force feedback
* For the GeForce NOW part: a GeForce NOW account, an ESP32-S3 devkit, and
  [PlatformIO][platformio-url]

### Installation

```sh
make              # build the driver
make check        # decoder + command-builder tests (no wheel needed)
make install      # ~/.local/bin/dfgt + login agent, no sudo
make uninstall    # unload, neutralise the wheel, remove files
```

`make install PREFIX=/usr/local` works too, if you would rather install system-wide.

Firmware (only needed for the GeForce NOW route):

```sh
pio run -d firmware/g29-gadget -t upload    # board on its CH343/UART port
```

See [firmware/g29-gadget/README.md](firmware/g29-gadget/README.md) for the wiring, the flashing
dance between the board's two USB ports, and the generator that validates the report descriptor
(`tools/make_g29_descriptor.py`, 12-byte input / 16-byte output).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- USAGE EXAMPLES -->
## Usage

```sh
dfgt status                    # current wheel state
dfgt watch                     # live decoded input; the tool for identifying controls
dfgt ffb autocenter 0xaaaa     # self-centring spring (what the daemon applies by default)
dfgt ffb autocenter-off
dfgt ffb constant 48           # gentle constant force; 0 or 'ffb off' clears it
dfgt range 200                 # lock-to-lock in degrees (40..900)
dfgt native                    # fix a wheel stuck in 200°/compat mode (no-op today)
dfgt probe                     # element dump + output-report self-test
dfgt watch --hex > tests/capture-controls.hex   # record reports as a regression fixture
dfgt relay                     # the GFN bridge: wheel state out, FFB back
```

Anything that can open a unix socket can drive the wheel (`$DFGT_SOCKET`, default
`/tmp/dfgt-daemon.sock`):

```sh
printf 'ffb constant 40\n' | nc -U /tmp/dfgt-daemon.sock     # -> ok constant 40
printf 'state\n'           | nc -U /tmp/dfgt-daemon.sock     # -> ok steer=… throttle=…
```

#### Three things worth knowing

1. **The wheel only sends reports when something changes.** `dfgt status` may answer "no input yet"
   until you turn the wheel or press a pedal once. That is the device, not a bug.
2. **The wheel holds a force until told otherwise** — there is no firmware timeout, so `kill -9` on
   the daemon leaves its last force applied. Clear it with `dfgt ffb off` (works with no daemon
   running) or unplug the wheel. The daemon neutralises the wheel on startup and on clean exit.
3. **Only one process should read the wheel.** Opening it from a second process silently stops
   report delivery to the first. Let the daemon own the device and send commands through the socket —
   `dfgt ffb …` does that automatically whenever a daemon is running.

#### Troubleshooting

| Symptom | Where to look |
|---|---|
| daemon log | `/tmp/dfgt-daemon.log` |
| relay log | `~/Library/Logs/dfgt-relay.log` |
| are the agents loaded? | `launchctl list \| grep dfgt` |
| wheel not found | `dfgt probe` (expects `046d:c29a`); `hidutil list \| grep -i 046d` |
| too stiff / too soft | `dfgt ffb autocenter 0x4000` … `0xffff`, or `autocenter-off` |
| no FFB in a game | most Mac games ignore wheels entirely; check whether the game sees the joystick at all |
| GFN ignores the wheel | the board must be plugged in — GFN looks for `046d:c24f`, never for the DFGT |
| GFN input freezes | the host stopped consuming the board's HID endpoint; unplug/replug the board |

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- ROADMAP -->
## Roadmap

| | | |
|---|---|---|
| ✅ | Wheel driven natively: native mode, 40–900° range, self-centring spring | L1 |
| ✅ | Unix socket: decoded state out, FFB commands in | L2 |
| ✅ | Every control identified from captures, decoder regression-tested | M1–M3 |
| ✅ | Package: `make install` / `make uninstall`, login agent | M5 |
| ✅ | GeForce NOW accepts the wheel as a G29 and streams it as a wheel | proxy |
| ❌ | Unmodified games see a virtual FFB device | needs an Apple-granted entitlement (G1) |
| ❌ | Force feedback inside GeForce NOW | macOS grants FFB to no device; see [docs/geforce-now.md](docs/geforce-now.md) |
| ⏸ | Emulated spring/damper/friction/periodic effects | deferred until something needs them (M4) |

See the [open issues](https://github.com/redesign3d/drivingforce-gt-macos/issues) for the full list
of proposed features and known issues, and [PLAN.md](PLAN.md) for the verified device facts and the
acceptance tests behind every ✅ above.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- CONTRIBUTING -->
## Contributing

Contributions are welcome. The useful ones tend to be evidence: a new fixture in `tests/`, a control
map correction, a game that behaves differently than documented, or a platform fact that has changed
since this was written.

1. Fork the project
2. Create your branch (`git checkout -b feature/AmazingFeature`)
3. Add your evidence — `make check` must pass, and captured reports belong in `tests/`
4. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
5. Push to the branch (`git push origin feature/AmazingFeature`)
6. Open a pull request

Please keep force commands out of anything that runs unattended: the wheel has no firmware watchdog,
so a leftover constant force keeps turning the wheel until it is cleared.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- LICENSE -->
## License

No license file yet — treat the code as all rights reserved until one is added.

Nothing was copied from the GPL projects this was built against: the force-feedback protocol was
reimplemented from their documented behaviour and confirmed against captures from the real wheel
(see [Acknowledgments](#acknowledgments) and [PLAN.md](PLAN.md) Appendix C).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- CONTACT -->
## Contact

Project Link: [https://github.com/redesign3d/drivingforce-gt-macos](https://github.com/redesign3d/drivingforce-gt-macos)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- ACKNOWLEDGMENTS -->
## Acknowledgments

* [Linux `hid-lg4ff` / `new-lg4ff`][newlg4ff-url] — the FFB command set, reimplemented here
* [`logiwheel2000`][logiwheel-url] — kext port of `lg4ff`, used to cross-check the protocol
* [`g923-mac-ffb`][g923-url] — userspace FFB on Apple Silicon, and the clearest statement of what macOS lacks
* [`fffb`][fffb-url] — classic Logitech FFB driven from a macOS process
* [`karrvel/g29-mac`][g29mac-url] — G29 output-report padding, poll-don't-callback
* [`botsofcog/LogiWheelHost`][logiwheelhost-url] — lg4ff wire offsets
* [`hicwic/g25-w11-driver`][g25w11-url] — the GFN-accepted G29 profile, and the finding that GFN keys
  wheels on `VID:PID:bcdDevice`
* [Best-README-Template][bestreadme-url] — the shape of this document
* [Img Shields][shields-url] — the badges

<p align="right">(<a href="#readme-top">back to top</a>)</p>

<!-- MARKDOWN LINKS & IMAGES -->
<!-- https://www.markdownguide.org/basic-syntax/#reference-style-links -->
[contributors-shield]: https://img.shields.io/github/contributors/redesign3d/drivingforce-gt-macos.svg?style=for-the-badge
[contributors-url]: https://github.com/redesign3d/drivingforce-gt-macos/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/redesign3d/drivingforce-gt-macos.svg?style=for-the-badge
[forks-url]: https://github.com/redesign3d/drivingforce-gt-macos/network/members
[stars-shield]: https://img.shields.io/github/stars/redesign3d/drivingforce-gt-macos.svg?style=for-the-badge
[stars-url]: https://github.com/redesign3d/drivingforce-gt-macos/stargazers
[issues-shield]: https://img.shields.io/github/issues/redesign3d/drivingforce-gt-macos.svg?style=for-the-badge
[issues-url]: https://github.com/redesign3d/drivingforce-gt-macos/issues
[platform-shield]: https://img.shields.io/badge/platform-macOS%20%7C%20ESP32--S3-blue.svg?style=for-the-badge
[platform-url]: https://github.com/redesign3d/drivingforce-gt-macos
[c-url]: https://en.wikipedia.org/wiki/C_(programming_language)
[iokit-url]: https://developer.apple.com/documentation/iokit
[platformio-url]: https://platformio.org/
[arduino-url]: https://github.com/espressif/arduino-esp32
[tinyusb-url]: https://github.com/hathach/tinyusb
[launchd-url]: https://www.launchd.info/
[newlg4ff-url]: https://github.com/berarma/new-lg4ff
[logiwheel-url]: https://gitlab.com/ColdPie/logiwheel2000
[g923-url]: https://github.com/CesarOvilla/g923-mac-ffb
[fffb-url]: https://github.com/eddieavd/fffb
[g29mac-url]: https://github.com/karrvel/g29-mac
[logiwheelhost-url]: https://github.com/botsofcog/LogiWheelHost
[g25w11-url]: https://github.com/hicwic/g25-w11-driver
[bestreadme-url]: https://github.com/othneildrew/Best-README-Template
[shields-url]: https://shields.io
