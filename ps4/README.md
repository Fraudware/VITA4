# VITA4

**Play your PS Vita on the TV through a PS4, over one USB cable.**

VITA4 is a homebrew app for jailbroken PS4 consoles. Plug a PS Vita into the
PS4 and VITA4 shows the Vita's screen on your TV, plays its sound through the
PS4 and lets you play with a DualShock 4, touchpad included.

**Latest version: 1.0.0**

---

## Table of Contents

- [Overview](#overview)
- [Installation](#installation)
- [How to Use](#how-to-use)
- [Controls](#controls)
- [Documentation](#documentation)
- [Building from Source](#building-from-source)
- [Current Status of VITA4](#current-status-of-vita4)
- [Credits](#credits)
- [License & Disclaimer](#license--disclaimer)

---

## Overview

VITA4 turns a PS4 into a dock for a PS Vita. The Vita sends its picture and
sound to the PS4 over a USB cable, and the PS4 sends your DualShock 4's
buttons, sticks and touches back to the Vita over the same cable. No
Bluetooth pairing, no capture card.

It is the PS4 version of **VITA5** (the PS5 Vita dock) and uses the same setup
on the Vita: **VitaUSBStream**, which makes the Vita act as a USB webcam and
microphone, and the **`input_receiver`** plugin, which lets the console
control the Vita.

### Main features

- **Picture** - the Vita's 960x544 screen on your TV, full screen or in a
  window, with three picture sizes (sharp 2x, fit to screen, original size).
- **Sound** - the Vita's 48 kHz stereo sound through the PS4's audio output,
  with a capped backlog so it stays close to the picture.
- **DualShock 4 controls** - every Vita button and both analog sticks.
- **Touchpad to touchscreen** - the DS4 touchpad drives the Vita's front
  screen (or rear touchpad). Taps are shaped so games register them, and every
  tap arrives as a new touch, like a finger on the real screen.
- **Stick dead zone** - worn sticks that rest slightly off centre no longer
  creep or make one direction faster; a full push still reaches the full range.
- **Upscaling** - *Smooth* (sharpen + 2x smooth scaling at full frame rate),
  plus *TV scaler*, *Sharp* and *FSR* (AMD FidelityFX FSR 1.0) modes, with
  adjustable sharpness.
- **Fast picture mode** - stream the Vita's picture at 480x272 for lower delay
  and a smoother feed (softer picture).
- **Stats overlay** - frame rates, late pictures, sound backlog, delay and
  live stick readings.
- **L3 shortcuts** - open the menu, cycle upscaling and switch full screen /
  windowed without the Vita seeing those buttons.
- **Remembers your settings**, and switches off an upscale mode automatically
  if it crashed the previous session.

---

## Installation

### What you need

- A **jailbroken PS4** running **GoldHEN**.
- A **PS Vita** with HENkaku / Ensō and:
  - **VitaUSBStream** (the USB webcam / microphone plugin and app), and
  - the **`input_receiver.skprx`** plugin, v2.0.3 (included in every VITA4
    release).
- A USB cable that carries data (the Vita's own cable works), plugged
  straight into the PS4, not through a hub.
- A DualShock 4.

### On the PS4

1. Download `VITA4-1.0.0.pkg` from the Releases page.
2. Copy it to a USB drive or to the PS4.
3. Install it with **GoldHEN's Package Installer**. Installing over an older
   VITA4 keeps your settings.

### On the Vita (once)

1. In **VitaShell**, copy `input_receiver.skprx` to `ur0:tai/`.
2. Open `ur0:tai/config.txt` and, under `*KERNEL`, add this line **above** the
   `VitaUSBStream.skprx` line:

   ```
   ur0:tai/input_receiver.skprx
   ```
3. Reboot the Vita.

---

## How to Use

1. On the Vita, open **VitaUSBStream** and switch streaming **on**. Do this
   again after every Vita reboot.
2. Connect the Vita to the PS4 with the USB cable.
3. Start **VITA4** on the PS4. It finds the Vita and the picture appears.
4. Play with the DualShock 4. Press **L3 + R3** together for the menu.

### Menu

| Setting | Options | What it does |
|---|---|---|
| Controller to Vita | **On** / Off | Off keeps the controller in VITA4 only |
| Touchpad | **Front screen** / Rear touchpad / Off | which Vita touch surface the DS4 touchpad drives |
| Touch taps | **Steady** / Slow games / Raw | Steady holds each tap long enough for games to see it; try Slow games if a game still misses taps |
| Picture size | **Sharp (2x)** / Fit to screen / Original size | set automatically while upscaling |
| Picture resolution | **Full 960x544** / Fast 480x272 | Fast: less delay, softer picture |
| Upscale | **Off** / Smooth / TV scaler / Sharp / FSR | Smooth is the recommended mode |
| Sharpness | Off / Low / Medium / High / **Max** | for Smooth and TV scaler |
| Sound | **On** / Off | |
| Stick dead zone | **On** / Off | ignores small resting drift on both sticks |
| Low lag | **Off** / On | experimental timing mode (see Current Status) |
| Stats | **Off** / On | the diagnostics overlay |
| Press Vita PS button | | presses the Vita's own PS button |
| Input test | | sends test taps, swipes and button presses to the Vita |
| Restart stream | | restarts the picture if it freezes |
| Exit VITA4 | | |

Defaults are in **bold**.

### Recommended settings

- **Sharpest picture:** Picture resolution *Full*, Upscale *Smooth*,
  Low lag *Off*.
- **Lowest delay / smoothest feed:** Picture resolution *Fast*, Low lag *Off*
  (upscaling doesn't apply to the Fast picture).
- Turn your TV's **Game Mode** on. TV picture processing often adds more delay
  than everything else combined.

### The Stats overlay

Turn on **Stats** in the menu to see:

| Line | Meaning |
|---|---|
| `960x544  38 fps in / 60 fps out` | picture size, pictures arriving from the Vita per second, and TV refreshes per second |
| `Frames … dropped … late …` | pictures received, broken pictures discarded, and refreshes VITA4 missed |
| `Sound on (X ms)` | how much sound is waiting to be played |
| `Lag  USB picture … to screen …` | time for one picture to cross the cable, and from arrival to the TV |
| `Low lag …` | what the Low lag mode is doing |
| `Sticks  left X Y  right X Y` | raw stick readings (128 = centre, 0 / 255 = full push) |

---

## Controls

| DualShock 4 | PS Vita |
|---|---|
| D-pad | D-pad |
| Cross / Circle / Square / Triangle | the same |
| Left and right sticks | left and right sticks |
| L2 / R2 | L / R |
| L1 / R1 | the PS TV's extra L1 / R1 (most handheld games ignore these) |
| Options | Start |
| Touchpad click | Select |
| Touchpad (touch) | front touchscreen, or rear touchpad (menu) |
| PS button | stays with the PS4 (use *Press Vita PS button* in the menu) |

**VITA4 shortcuts** (the Vita never sees these):

| Buttons | Action |
|---|---|
| **L3 + R3** | open / close the VITA4 menu |
| **L3 + D-pad Left / Right** | cycle Upscale |
| **L3 + D-pad Up** | full screen / windowed |

---

## Documentation

| Document | Contents |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | the release history and every development build before it |
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | release notes for the current version |
| [REFERENCE.md](REFERENCE.md) | how VITA4 reaches the Vita's USB on the PS4 (the 1.01 known-good reference) |
| [docs/VITA_INPUT_API.md](../docs/VITA_INPUT_API.md) | the controller / touch protocol between console and Vita |
| [vita-side/input-receiver/README.md](../vita-side/input-receiver/README.md) | the Vita plugin: what it does and how it is built |

### Reporting a problem

Please open an issue with:

1. the VITA4 version, your PS4 model and firmware;
2. the game and what happened;
3. a photo of the screen with **Stats** on;
4. if possible, the log file `/data/vita4/vita4.log` from the PS4 (via FTP).

---

## Building from Source

You need the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
(v0.5.2 or newer), `clang` / `ld.lld`, and on Linux `libssl1.1` for the
packager.

```sh
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
cd ps4
make            # -> IV0000-VITA00004_00-VITA4DOCK0000000.pkg
make test       # host unit tests (with AddressSanitizer / UBSan)
make preview    # renders every screen to build/preview/*.png
```

`make font` rebuilds the embedded UI font (Inter) and `make art` rebuilds the
icon and backgrounds.

### Source layout

| Path | Role |
|---|---|
| `ps4/src/main.cpp` | threads (UI, USB link, controller, sound) and the menu |
| `ps4/src/vita_link.cpp` | the Vita over `libSceUsbd` (and the GoldHEN raw-USB path): detection, video, sound, controller and touch sends |
| `ps4/src/vita_desc.c` | USB descriptor parsing |
| `ps4/src/nv12_scale.c` | picture conversion and the picture sizes |
| `ps4/src/smooth.c` | the Smooth upscale (SSE2) |
| `ps4/src/upscale.c`, `upscale_worker.cpp` | Sharp / FSR on the CPU, in a background thread |
| `ps4/src/pacing.c` | Low lag draw timing |
| `ps4/src/pad_map.c` | DualShock 4 to Vita reports, touch tap shaping, stick dead zone, L3 shortcuts |
| `ps4/src/settings_io.cpp` | saved settings |
| `ps4/src/display.cpp`, `row_pool.cpp`, `cpu_ps4.cpp` | video out, render threads, CPU core pinning |
| `ps4/src/malloc_lock.c` | thread-safe memory allocation for PS4 threads |
| `ps4/src/ui.cpp`, `canvas.cpp` | screens, menu and drawing |
| `ps4/tests/host_tests.cpp` | unit tests |
| `core/` | USB / UVC / UAC code shared with VITA5 |
| `vita-side/input-receiver/` | the Vita plugin source |

---

## Current Status of VITA4

**Version 1.0.0** (first public release). Tested on a real jailbroken PS4 (GoldHEN) with a PS Vita
running VitaUSBStream and `input_receiver` v2.0.3, mostly with Killzone
Mercenary.

### Working

- Detecting the Vita, picture and sound over one USB cable.
- All buttons and both sticks, with the stick dead zone.
- Touchpad taps and swipes in menus and games (including Killzone
  Mercenary's pause menu).
- Smooth upscaling at full frame rate.
- Picture resolution Fast (480x272) with Low lag Off.
- Saved settings, menu, shortcuts, Stats overlay.

### Known issues

- **Frame rate at Full 960x544 tops out around 38 fps.** Each full-size
  picture takes about 23 ms to cross USB 2.0. Games that run at 30 fps on the
  Vita (most, including Killzone Mercenary) show every frame; 60 fps games
  are capped at about 38.
- **Fast 480x272 makes the Vita send 200+ pictures a second** (VitaUSBStream
  ignores the requested 60 fps at that size). It has worked well in testing,
  but one session had a choppy game and no sound. If that happens, power the
  Vita fully off and on, restart VitaUSBStream, and if it persists use Full.
  Upscaling doesn't apply to the Fast picture.
- **Low lag is experimental.** At Full 960x544 the Vita's frames usually
  arrive too unevenly for it to save anything (Stats shows "irregular: Off
  timing"), and with Fast it made camera turns stutter. Keep it Off.
- **Delay.** Picture and sound reach the TV a little later than on the Vita
  itself (the USB transfer plus the game's own frame delay), and sound runs
  slightly behind the picture.
- **Sharp and FSR are slow** (about 10 fps). PS4 homebrew can't run them on
  the GPU, so they run on the CPU. Use Smooth for playing.
- **TV scaler** is refused by some PS4s; VITA4 then skips it automatically.
- **Looking up and down can feel slower than turning** in some games, usually
  because of the game's own vertical sensitivity.
- **The touchpad is not a touchscreen.** It is smaller and less precise;
  tiny buttons can need a careful tap, and some games need Touch taps set to
  *Slow games*.
- **The Vita must be streaming** (VitaUSBStream on) after every reboot, or
  VITA4 waits for it.
- Tested on one PS4 / Vita pair and a small number of games. Reports from
  other setups are welcome.

### Not supported

- PS4s without GoldHEN, and PS Vitas without HENkaku / Ensō.
- Connecting through a USB hub or a charge-only cable.
- The DualShock 4's motion sensors (the Vita's motion controls are not forwarded).

---

## Credits

VITA4 is built on the work of others: VITA5's `input_receiver` plugin,
xerpi's vita-udcd-uvc (VitaUSBStream), GoldHEN and the OpenOrbis toolchain.

---

## License & Disclaimer

### License

VITA4 is free software, released under the **GNU General Public License
v3.0 or later** - see [LICENSE](../LICENSE).

Third-party parts:

- AMD FidelityFX FSR 1.0 algorithms (EASU, RCAS): MIT -
  [FSR1-MIT.txt](licenses/FSR1-MIT.txt)
- Inter font: SIL Open Font License 1.1 -
  [Inter-LICENSE.txt](licenses/Inter-LICENSE.txt)
- stb_image: public domain / MIT -
  [LICENSE](third_party/stb/LICENSE)

Credits: VITA5 (the PS5 dock this is ported from, and the `input_receiver`
plugin), xerpi's [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc)
(the USB video gadget VitaUSBStream builds on), the
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
and [GoldHEN](https://github.com/GoldHEN/GoldHEN).

### Disclaimer

- VITA4 is an unofficial homebrew project. It is **not affiliated with,
  endorsed by or supported by Sony Interactive Entertainment**. PlayStation,
  PS4, PS Vita and DualShock are trademarks of Sony Interactive Entertainment.
- It requires a **jailbroken PS4 and a modded PS Vita**. Modifying consoles
  may void warranties and may break the terms of service of online services;
  keep modded consoles offline from PlayStation Network.
- VITA4 is provided **as is, without any warranty**. Use it at your own risk;
  the authors are not responsible for any damage to consoles, data or saves.
- VITA4 does not include or help obtain any games. Only play games you own.
