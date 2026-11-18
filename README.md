# SunFounder TFT 3.5 inch IPS display (ST7796S, 16-bit command variant)

DRM driver and 4-point touch calibration for the SunFounder 3.5" IPS TFT
display (480x320, ST7796S + XPT2046 resistive touch) on Raspberry Pi 4/5.

## Install

Quick install (Raspberry Pi OS Bookworm / Trixie):

```bash
curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-tft35ips/main/install.sh | sudo bash
```

The installer

1. installs `build-essential`, `dtc`, kernel headers and the Python deps;
2. clones this repository to `~/sunfounder-tft35ips`;
3. builds and installs `sunfounder-tft35ips.ko` + the device-tree overlay;
4. installs the touch calibration tool (`touch-calibrate`);
5. adds `dtoverlay=sunfounder-tft35ips` to `config.txt`.

Then reboot. The panel comes up as a standard DRM output (480x320). For the
desktop run **Touch Calibration** once from the menu.

Manual install:

```bash
git clone https://github.com/sunfounder/sunfounder-tft35ips
cd sunfounder-tft35ips
sudo bash install.sh
```

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

- **kernel 6.18 compatibility**: dropped `DRIVER_PRIME` / `.date`, added
  `DRM_FBDEV_DMA_DRIVER_OPS`, replaced `drm_fbdev_dma_setup()` with
  `drm_client_setup_with_fourcc()`;
- **30 Hz display mode** (480x320 at 30 Hz). Declaring 60 Hz makes the
  compositor queue frames faster than the SPI can push them, which shows up as
  laggy input ("clicks do nothing", text input stalls). 30 Hz matches the
  panel's SPI bandwidth;
- **SPI 125 MHz** for pixel data (the module's bridge supports it). Lower it
  in `config.txt` if the ribbon is long or the power supply is weak:

  ```
  dtoverlay=sunfounder-tft35ips,speed=48000000
  ```

- **touch SPI 2 MHz** (the XPT2046 maximum is 3.25 MHz).

## Touch calibration

`tools/touch-calibrate/` contains a small GTK3 tool (Wayland/labwc friendly)
that samples four targets twice, solves the affine raw->screen transform and
installs it as the libinput calibration matrix via udev. See
`tools/touch-calibrate/README.md`.

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
