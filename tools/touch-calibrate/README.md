# touch-calibrate

4-point touchscreen calibration for the SunFounder 3.5" IPS display on
Raspberry Pi OS (labwc / Wayland).

The stock `xinput-calibrator` only works on X11, and the `libinput calibrate`
sub-command is not shipped by the Debian package, so this small GTK3 tool
provides a Wayland-friendly calibration:

- shows four crosshairs on the panel;
- samples each target twice (8 taps total) and averages;
- solves the affine transform raw touch -> screen coordinates;
- installs it as a udev `LIBINPUT_CALIBRATION_MATRIX` property, which libinput
  (and therefore labwc/wlroots) applies on the next login/reboot.

## Files

| File | Purpose |
| --- | --- |
| `touch-calibrate` | GTK3 calibration UI (Python 3 + PyGObject + python-evdev) |
| `apply-touch-calibration` | root helper: writes the udev rule (`--reset`, `--reboot`) |
| `touch-calibrate.desktop` | desktop entry ("Touch Calibration") |

## Install (normally done by install-tft35ips.sh)

```bash
sudo install -m 755 touch-calibrate /usr/local/bin/touch-calibrate
sudo install -m 755 apply-touch-calibration /usr/local/sbin/apply-touch-calibration
echo "$USER ALL=(root) NOPASSWD: /usr/local/sbin/apply-touch-calibration" \
  | sudo tee /etc/sudoers.d/touch-calibrate
sudo chmod 440 /etc/sudoers.d/touch-calibrate
sudo install -m 644 touch-calibrate.desktop /usr/share/applications/
sudo apt-get install -y python3-evdev python3-gi python3-gi-cairo
```

## Use

Run **Touch Calibration** from the desktop menu, or `touch-calibrate` from a
terminal on the desktop. Tap the highlighted cross, then tap it once more
(averaging), and continue with the next three crosses. When finished, press
**REBOOT & APPLY** (or reboot later).

## How it works

For each target the raw `ABS_X`/`ABS_Y` values are normalised to 0..1 and the
target's screen position is normalised to 0..1. Six unknowns
(`a b c d e f`) are least-squares fitted from the four points:

```
screen_x = a*raw_x + b*raw_y + c
screen_y = d*raw_x + e*raw_y + f
```

The matrix `a b c d e f 0 0 1` is written as

```
SUBSYSTEM=="input", ENV{ID_INPUT_TOUCHSCREEN}=="1", \
  ATTRS{name}=="ADS7846 Touchscreen", \
  ENV{LIBINPUT_CALIBRATION_MATRIX}="a b c d e f 0 0 1"
```

and applied by libinput when the compositor opens the touch device.

## Reset

```bash
sudo /usr/local/sbin/apply-touch-calibration --reset
```
