# promicro-vesa-stereo-firmware

Firmware for a Pro Micro (ATmega32U4) that turns vsync timestamps sent over
USB into a **VESA stereo sync** signal for 3D emitters: the 3-pin mini-DIN
VESA stereo connector, or a 3.5 mm mono sync input. The emitter (RF, Bluetooth or IR) then drives the
shutter glasses.

It lets the shutter glasses of an existing **3D emitter kit**, infrared or
radio, work with a TV that has no 3D: the board takes the place of a 3D
projector's sync output. Tested with the Optoma BC100B (Monster Vision)
emitter; other VESA-input emitters (XPAND, Volfoni, CrystalEyes…) should
work and are listed in [docs/GUIDE.md](docs/GUIDE.md).

It was written for a box running the
[CoreELEC yacer-patched build](https://github.com/allolive/CoreELEC/tree/yacer),
which shows 3D frame-sequentially on a 2D TV that takes 1080p at 120 Hz
with VRR (Kodi add-on [Active 3D for 120 Hz VRR displays](https://github.com/allolive/kodi_addons),
`service.shutter3d.allolive`), but any host that can
timestamp its vsyncs can drive it.

> **Starting from scratch? Use the Raspberry Pi emitter instead.**
> [pi-bt3dsp-firmware](https://github.com/allolive/pi-bt3dsp-firmware) turns a
> Raspberry Pi Zero W (v1.1) into a Bluetooth 3D emitter that the same Kodi
> add-on boots and drives, with Bluetooth RF 3D glasses (tested: Hi-Shock RF
> Pro).
>
> - **Easier to build:** one USB cable, no SD card, no soldering, no wiring and
>   no emitter kit to buy.
> - **More powerful adjustments:** the add-on sets when the lenses open, so
>   they stay shut while the TV redraws (no ghosting), tuned live from Kodi
>   over a test pattern. With this board, the lens timing is up to the
>   emitter's own controls, if it has any.
>
> This board remains for people who already own an emitter with a VESA
> stereo input.

**Building one: [docs/GUIDE.md](docs/GUIDE.md)** walks through parts,
wiring, flashing and using it with the Kodi add-on, step by step.

## Hardware

- **Board:** ATmega32U4 at 5 V / 16 MHz. Tested on a HiLetgo Pro Micro
  5V/16MHz (USB-C);
  the Arduino Leonardo and Micro should work (same chip), untested. The
  3.3 V / 8 MHz Pro Micro does not.
- **Parts:** a resistor for the sync wire, 220 Ω (two 100 Ω in series do;
  one 100 Ω works but leaves less margin against a short) and the emitter's
  3-pin VESA cable. Optional: an AO3401A P-channel MOSFET on the emitter's
  +5 V, gate on pin 8, so the board can switch the emitter off when no 3D
  plays (firmware 0.2.0 and add-on 0.8.6 or newer; see the guide, step
  3.1).
- **Tested setup:** an Optoma BC100B RF emitter (the Monster Vision kit's)
  on its 3-pin VESA plug. Other VESA emitters and the 3.5 mm sync cable are
  untested.
- **For a TV, pick an emitter whose lens opening can be shortened.** A TV
  redraws the picture top to bottom, so a lens open the whole frame sees the
  other eye's picture: an emitter with a fixed opening ghosts on an OLED at
  every delay, while the Optoma BC100B with its opening shortened looked
  free of it. The
  board's delay only moves the ghost. See *Ghosting* in
  [docs/GUIDE.md](docs/GUIDE.md).

### Wiring

| Board | VESA 3-pin mini-DIN | 3.5 mm mono sync jack (untested) |
|---|---|---|
| GND | ground pin **and** the plug's metal shell | sleeve |
| VCC (5 V) | +5 V: powers the emitter | — (power the emitter as its manual says) |
| pin 9 → 220 Ω | sync, the pin next to the notch (high = left eye) | tip |

Published pinouts number the mini-DIN's pins differently;
[docs/GUIDE.md](docs/GUIDE.md) shows them by position and how to confirm
ground with a multimeter before the first power-up. Plug the cable in with
the power off. You build and use this at your own risk.

## Flashing

From a [release](https://github.com/allolive/promicro-vesa-stereo-firmware/releases)
(the latest), with avrdude (a Pro Micro enters its bootloader for 8 s
after its reset pin is shorted to ground twice; the port can change while
it does):

```
avrdude -p atmega32u4 -c avr109 -P /dev/ttyACM0 -U flash:w:promicro-vesa-stereo-leonardo.hex:i
```

From source, with arduino-cli:

```
arduino-cli core install arduino:avr@1.8.8
make build                      # FQBN=arduino:avr:micro for an Arduino Micro
make upload PORT=/dev/ttyACM0
```

## Protocol

USB serial, one command per line:

| Command | Meaning |
|---|---|
| `S <vsync> <us> <L\|R\|->` | the host's vsync number, its time in microseconds (mod 2^32), and the eye it shows |
| `M <0\|1\|2>` | off / free-run / follow the host (default) |
| `F <millihertz>` | free-run rate, `96000` = 96 Hz |
| `D <us>` | delay from vsync to the eye switch, may be negative |
| `I <0\|1>` | swap the eyes |
| `E <us>` | move only the switches into the right eye (probes whether an emitter reads both edges) |
| `P <0\|1>` | the emitter's power through the optional MOSFET on pin 8: `1` on (the default at every start-up, never saved), `0` off; nothing changes on a board without it |
| `W` | save mode, rate, delay and swap to EEPROM |
| `?` | status line, starting with `promicro-vesa-stereo <version>` |

## How the timing works

USB delivers each message up to a few milliseconds late, by a varying amount,
so the board never switches when a message arrives:

- **The vsync times are exact.** The host stamps each vsync in its video
  interrupt and sends that time.
- **The frame period is exact.** It is the host's time over the vsync count
  since the span began, not an average; a rate change (VRR between films)
  starts a new span, and the period is exact again within a frame.
- **Only the clock conversion is fitted:** the host clock's offset and drift
  against the board's crystal, from the earliest-arriving messages over the
  last 8 seconds.
- **The switch is hardware:** timer 1's compare unit flips pin 9 at the
  vsync plus the delay, with no code running at that moment.
- **A host that goes quiet** is coasted through on the last vsync and the
  exact period; after 2 s without a message the glasses stop.

The simulation runs this same code (`sync_core.h`) against USB jitter, 5 ms
stalls, dropped vsyncs, ±150 ppm crystal skew, both clocks wrapping, VRR rate
changes, a host silent for up to 1.9 s, and breaks between films. Every edge
lands within ±60 µs of the true vsync (typically ±35 µs), with none missed,
doubled or on the wrong eye; glasses allow about ±500 µs.

## The light rig

`tools/lightrig` turns a second Pro Micro into a four-channel light timer
(TEMT6000 modules on A0..A3), and `tools/lightrig/lightrig.py` on a laptop
summarises it:

- **Drawing time:** one sensor at the top of the screen and one at the bottom,
  playing a left-white, right-black 3D clip. The lag between the two rising
  edges is how long the display takes to draw a frame.
- **Lens timing:** a sensor behind each lens, looking at the same white patch
  as the top sensor. Shows when each lens opens and closes against the
  picture, and so where to set the delay.

```
make upload-lightrig PORT=/dev/ttyACM1
tools/lightrig/lightrig.py --port /dev/ttyACM1 --ref 0 --seconds 5
tools/lightrig/lightrig.py --raw 2            # one raw trace of sensor 2
```

## Tests and builds

```
make test        # needs g++ and pytest
```

GitHub Actions runs the simulation and builds both sketches for the Leonardo
(also the Pro Micro) and the Micro on every push and pull request. Every
build of `main` is published as a
[release](https://github.com/allolive/promicro-vesa-stereo-firmware/releases),
tagged `v<version>-<date>-<commit>` and marked the latest, with the `.hex`
images and their `SHA256SUMS`; a `v*` tag matching the firmware's version
publishes a release under that name.

## License

Free for personal, hobby and other non-commercial use; **not for sale**, and
not for any commercial use, without the author's written permission.

- **Code** (the firmware, the light rig, the tests and tools): PolyForm
  Noncommercial 1.0.0, see `LICENSE`.
- **Documentation and drawings** (`docs/`, this README): Creative Commons
  BY-NC-SA 4.0, see `docs/LICENSE`. Share them with credit, non-commercially,
  under the same terms.

For commercial use (selling boards, kits or the firmware, or building it
into a product), ask allolive on GitHub.

The released `.hex` files also contain the Arduino AVR core, which keeps its
own licence (LGPL-2.1).
