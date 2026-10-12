# VITA4 1.0.0

The first public release of VITA4.

Play your PS Vita on the TV through a jailbroken PS4, over one USB cable:
picture, sound and DualShock 4 controls (including the touchpad).

## Downloads

| File | What it is |
|---|---|
| `VITA4-1.0.0.pkg` | the PS4 app (install with GoldHEN's Package Installer) |
| `input_receiver.skprx` | the Vita controller plugin, v2.0.3 (goes in `ur0:tai/`) |
| `VITA4-1.0.0-source.zip` | full source code |
| `SHA256SUMS.txt` | checksums |

Setup and the full menu reference are in the [README](README.md).

## Highlights

- **Picture, sound and controls over one USB cable**: no Bluetooth pairing,
  no capture card.
- **Touchpad that works in games**: taps are held long enough for games to see
  them, and every tap arrives as a new touch (fixes menus that only
  highlighted, e.g. Killzone Mercenary's pause menu). Three modes: Steady /
  Slow games / Raw.
- **Stick dead zone**: worn DualShock 4 sticks no longer creep or make one
  direction turn faster than the other; a full push still reaches the full
  range.
- **Smooth upscaling** at full frame rate, plus TV scaler, Sharp and FSR modes
  and a sharpness setting.
- **Picture resolution: Fast 480x272** for lower delay and a smoother feed, or
  Full 960x544 for the sharpest picture.
- **Steadier sound**: the sound backlog is capped so it stays close to the
  picture.
- **Stats overlay**: frame rates, late pictures, sound backlog, delay and live
  stick readings.
- **L3 shortcuts**: L3 + R3 menu, L3 + D-pad Left/Right upscale, L3 + D-pad Up
  full screen / windowed.

## Recommended settings

- Sharpest picture: Picture resolution **Full**, Upscale **Smooth**, Low lag **Off**.
- Lowest delay: Picture resolution **Fast**, Low lag **Off**.
- TV in **Game Mode**.

## Known issues

- At Full 960x544 the USB cable carries about **38 pictures a second**.
  30 fps games (most Vita games) are unaffected; 60 fps games are capped.
- **Fast 480x272** makes the Vita send 200+ pictures a second. It has worked
  well in testing, but one session had a choppy game and no sound; if that
  happens, power the Vita off and on and restart VitaUSBStream, or use Full.
  Upscaling doesn't apply to the Fast picture.
- **Low lag** is experimental. Keep it Off; with Fast it made camera turns
  stutter.
- **Sharp and FSR are slow** (about 10 fps, CPU only). Use Smooth.
- **TV scaler** is refused by some PS4s and is then skipped.
- Picture and sound arrive a little later than on the Vita itself (USB
  transfer plus the game's own delay), and sound runs slightly behind.
- Looking up/down can feel slower than turning in some games (the game's own
  vertical sensitivity).
- Tested on one PS4 and Vita, mostly with Killzone Mercenary. Please report
  other games and setups, ideally with a photo of the Stats overlay.

## Install / update

- **PS4:** install `VITA4-1.0.0.pkg` with GoldHEN's Package Installer.
  If you tested a development build of VITA4 (numbered 1.xx or 2.00), delete
  it first (PS4 home screen, Options, Delete) - your VITA4 settings are kept.
- **Vita:** if `input_receiver.skprx` v2.0.3 is already installed, there's
  nothing to do. Otherwise copy it to `ur0:tai/`, add
  `ur0:tai/input_receiver.skprx` under `*KERNEL` in `ur0:tai/config.txt`
  (above the `VitaUSBStream.skprx` line), then reboot the Vita.
- On the Vita, turn VitaUSBStream streaming **on** after every reboot.
