# SunFounder TFT 3.5 inch IPS display (ST7796S)

DRM driver, touch calibration and a `doctor` self-check tool for the
SunFounder 3.5" IPS TFT display (480x320, ST7796S + XPT2046 resistive touch)
on Raspberry Pi 4 / 5.

## Features

- standard **DRM/KMS** output (no fbtft), works with labwc/Wayland, the
  console and X11
- **30 Hz display mode** matched to the panel's SPI bandwidth (declaring
  60 Hz over-drives the SPI and shows up as laggy input: "clicks do nothing",
  text input stalls)
- display SPI **125 MHz**, touch SPI **2 MHz**
- **DKMS**: the module is rebuilt automatically after a kernel update
- **4-point touch calibration** that works on Wayland (libinput calibration
  matrix through udev)
- **`sf-tft doctor`** self-check for users and support (`--fix`, `status`,
  `calibrate`)

## Supported systems

- Raspberry Pi 4 / 5
- Raspberry Pi OS Bullseye (kernel 6.1), Bookworm (6.6), Trixie (6.18)
- Debian 64-bit
- Kali (X11) with the fbdev fallback described below

## X11 desktops (Kali and friends)

On Wayland the driver runs through KMS and every compositor frame reaches the
panel. X11's `modesetting` driver flips the framebuffer **without reporting
damage**, so the panel keeps showing the first frame and looks frozen.

The installer therefore adds an fbdev Xorg config on systems that only have an
X11 session:

```
/etc/X11/xorg.conf.d/20-sunfounder-tft-x11.conf
-> Option "fbdev" "/dev/fbN"   (the framebuffer that belongs to the panel)
```

fbdev drives a single framebuffer, so **multi-monitor (HDMI + panel) is not
available** on X11; use Wayland for that. If the distribution also ships an
OutputClass that forces the modesetting driver, move it aside as well — Kali
does:

```bash
sudo mv /etc/X11/xorg.conf.d/99-vc4.conf /etc/X11/xorg.conf.d/99-vc4.conf.disabled
sudo systemctl restart lightdm
```

## Install

```bash
curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-tft35ips/main/install.sh | sudo bash
```

The installer

1. installs `build-essential`, `dtc`, `dkms`, kernel headers and the Python
   dependencies (`python3-evdev`, `python3-gi`);
2. clones this repository to `~/sunfounder-tft35ips`;
3. builds the module and the device-tree overlay;
4. installs the module through **DKMS** (or `make install` when DKMS is not
   available);
5. installs `sf-tft` and `touch-calibrate` (plus the desktop menu entry);
6. adds `dtoverlay=sunfounder-tft35ips` to `config.txt`.

When a prebuilt module for the running kernel is available in the GitHub
release, the installer downloads it instead of compiling (no kernel headers
needed); otherwise it builds from source. Releases are produced by the
`build-kernel-module` workflow.

Then reboot. The panel comes up as a standard DRM output at 480x320.

Manual install:

```bash
git clone https://github.com/sunfounder/sunfounder-tft35ips
cd sunfounder-tft35ips
sudo bash install.sh
```

## Check the installation

```bash
sudo sf-tft doctor
```

```
SunFounder TFT 3.5" display — doctor
  raspberrypi  kernel 6.6.31+rpt-rpi-v8  module 1.0.0

  [ OK ] config.txt: dtoverlay=sunfounder-tft35ips
  [ OK ] kernel module: sunfounder_tft35ips (1.0.0)
  [ OK ] DRM output: card2-SPI-1
  [ OK ] modes: 480x320
  [ OK ] touch device: /dev/input/event6
  [ OK ] calibration: 1.103244 0.002193 -0.031371 0.000596 1.124846 -0.064564 0 0 1
  [WARN] power: throttled=0x50000 (EXT5V 4.91 V)
         -> use the official 5V/5A power supply
  [ OK ] dmesg: no SPI/driver errors

no errors, 1 warning(s)
```

Every line is `[ OK ]`, `[WARN]` or `[FAIL]` followed by what to do next, so a
photo or screenshot of the output is usually enough for support.

```bash
sudo sf-tft doctor --fix      # repair the safe, idempotent problems
sudo sf-tft status            # one-screen summary
sudo sf-tft calibrate         # run the 4-point calibration
```

`doctor --fix` re-adds a missing `dtoverlay`, `modprobe`s the module, reloads
the udev rules (which re-applies the touch calibration) and then re-runs the
checks. It never reboots, flashes firmware or deletes anything.

The doctor convention (required checks, output format, `--fix` boundaries) is
described in the `doctor` skill.

## Touch calibration

`tools/touch-calibrate/` contains a small GTK3 tool (Wayland/labwc friendly)
that samples four targets twice, solves the affine raw->screen transform and
installs it as the libinput calibration matrix through udev. Run
**Touch Calibration** from the menu, or `sudo sf-tft calibrate`.

See `tools/touch-calibrate/README.md` for details.

## DKMS

`install.sh` registers the module with DKMS as `sunfounder-tft35ips/1.0.0`, so
it is rebuilt automatically after a kernel update. Check with `dkms status`;
set `TFT35IPS_NO_DKMS=1` to force a plain `make install` instead.

## Pin definition

| Pin (BCM) | Function |
| --- | --- |
| 17 | Touch pen down (XPT2046 IRQ) |
| 25 | TFT reset |
| 24 | TFT DC |
| 22 | Backlight (module has it tied on; not software controlled) |
| SPI0 CE0 | LCD chip select |
| SPI0 CE1 | Touch chip select |

## Display timing and SPI clock

The panel is a 16-bit-command variant: the module converts SPI to a 16-bit
parallel bus, so every command/parameter byte is sent as a 16-bit word
(`waveshare_command()`). Because of that the controller is **not** driven by
the generic `panel-mipi-dbi` driver (8-bit DBI).

The stock kernel `ili9486.c` driver was forked for this panel. Changes made
here:

- **kernel compatibility**: the fbdev setup API is chosen at compile time
  (`drm_client_setup_with_fourcc` on 6.11+, `drm_fbdev_dma_setup` on 6.4-6.10,
  `drm_fbdev_generic_setup` below), `DRIVER_PRIME` is only used when the
  kernel still defines it;
- **30 Hz display mode** (480x320 at 30 Hz). Declaring 60 Hz makes the
  compositor queue frames faster than the SPI can push them, which shows up
  as laggy input. 30 Hz matches the panel's SPI bandwidth;
- **SPI 125 MHz** for pixel data (the module's bridge supports it). Lower it
  in `config.txt` if the ribbon is long or the power supply is weak:

  ```
  dtoverlay=sunfounder-tft35ips,speed=48000000
  ```

- **touch SPI 2 MHz** (the XPT2046 maximum is 3.25 MHz).

## Uninstall

```bash
sudo dkms remove -m sunfounder-tft35ips -v 1.0.0 --all
sudo rm -f /usr/local/bin/sf-tft /usr/local/bin/touch-calibrate
sudo rm -f /usr/local/sbin/apply-touch-calibration
sudo rm -f /etc/sudoers.d/touch-calibrate
sudo rm -f /usr/share/applications/touch-calibrate.desktop
sudo rm -f /boot/firmware/overlays/sunfounder-tft35ips.dtbo
sudo sed -i '/dtoverlay=sunfounder-tft35ips/d' /boot/firmware/config.txt
sudo reboot
```

## Debug

### ST7796 boot sequence

```
0x1000011
0x20000ff
0x1000036 0x28
0x100003a 0x55
0x10000f0 0xc3
0x10000f0 0x96
0x10000b4 0x01
0x10000b7 0xc6
0x10000c0 0x80 0x45
0x10000c1 0x13
0x10000c2 0xa7
0x10000c5 0x0a
0x10000e8 0x40 0x8a 0x00 0x00 0x29 0x19 0xa5 0x33
0x10000e0 0xd0 0x08 0x0f 0x06 0x06 0x33 0x30 0x33 0x47 0x17 0x13 0x13 0x2b 0x31
0x10000e1 0xd0 0x0a 0x11 0x0b 0x09 0x07 0x2f 0x33 0x47 0x38 0x15 0x16 0x2c 0x32
0x10000f0 0x3c
0x10000f0 0x69
0x20000ff
0x1000021
0x1000029
```
