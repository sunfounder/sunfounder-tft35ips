# SunFounder TFT 3.5 inch IPS TFT display

## Inatall

Quick install

```bash
curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-installer-scripts/main/install-tft35ips.sh | sudo bash
```

Manual install

```bash
git clone https://github.com/sunfounder/sunfounder-tft35ips
cd sunfounder-tft35ips

cd driver
make all
sudo make install

cd ../
sudo cp 99-calibration.conf /etc/X11/xorg.conf.d/

sudo reboot
```