#!/bin/bash
#
# SunFounder TFT 3.5" IPS display (ST7796S) installer
#
#   curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-tft35ips/main/install.sh | sudo bash
#
# Installs:
#   - the DRM driver (sunfounder-tft35ips.ko) and its device-tree overlay
#   - the touch calibration tool (touch-calibrate) and its desktop entry
#   - dtoverlay=sunfounder-tft35ips in config.txt
#
# Environment overrides (mainly for development):
#   TFT35IPS_LOCAL=/path/to/checkout   use a local checkout instead of cloning
#   TFT35IPS_REPO=... TFT35IPS_BRANCH=... TFT35IPS_DIR=...
#   TFT35IPS_NO_REBOOT_PROMPT=1        do not ask to reboot at the end

set -e

DRIVER_REPO="${TFT35IPS_REPO:-https://github.com/sunfounder/sunfounder-tft35ips.git}"
DRIVER_BRANCH="${TFT35IPS_BRANCH:-main}"

USER_NAME="${SUDO_USER:-$USER}"
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
[ -z "$USER_HOME" ] && USER_HOME="$HOME"
INSTALL_DIR="${TFT35IPS_DIR:-$USER_HOME/sunfounder-tft35ips}"

log() { printf '\033[34m%s\033[0m\n' "$1"; }
ok()  { printf '\033[32m[✓]\033[0m %s\n' "$1"; }
die() { printf '\033[31m[✗]\033[0m %s\n' "$1" >&2; exit 1; }

if [ "$EUID" -ne 0 ]; then
    die "Please run as root: curl -sSL ... | sudo bash"
fi

CONFIG_TXT=""
for p in /boot/firmware/config.txt /boot/config.txt; do
    if [ -f "$p" ]; then CONFIG_TXT="$p"; break; fi
done
[ -z "$CONFIG_TXT" ] && die "config.txt not found"

log "1/6 Install dependencies"
apt-get update -qq || die "apt-get update failed"
apt-get install -y git build-essential device-tree-compiler \
    python3-evdev python3-gi python3-gi-cairo || die "dependency install failed"
if [ ! -d "/usr/src/linux-headers-$(uname -r)" ]; then
    apt-get install -y "linux-headers-$(uname -r)" \
        || apt-get install -y raspberrypi-kernel-headers \
        || die "kernel headers not found"
fi
ok "dependencies"

log "2/6 Download driver"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
if [ -n "${TFT35IPS_LOCAL:-}" ] && [ -d "$TFT35IPS_LOCAL" ]; then
    SRC="$TFT35IPS_LOCAL"
    ok "using local checkout $SRC"
elif [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/driver/sunfounder-tft35ips.c" ]; then
    SRC="$SCRIPT_DIR"
    ok "using local checkout $SRC"
else
    rm -rf "$INSTALL_DIR"
    git clone --depth=1 --branch "$DRIVER_BRANCH" "$DRIVER_REPO" "$INSTALL_DIR"
    chown -R "$USER_NAME:$USER_NAME" "$INSTALL_DIR"
    SRC="$INSTALL_DIR"
    ok "cloned to $INSTALL_DIR"
fi

log "3/6 Build driver"
make -C "$SRC/driver" clean >/dev/null 2>&1 || true
make -C "$SRC/driver" || die "driver build failed"
ok "driver built"

log "4/6 Install driver"
make -C "$SRC/driver" install || die "driver install failed"
ok "driver installed"

log "5/6 Install touch calibration tool"
install -m 755 "$SRC/tools/touch-calibrate/touch-calibrate" /usr/local/bin/touch-calibrate
install -m 755 "$SRC/tools/touch-calibrate/apply-touch-calibration" /usr/local/sbin/apply-touch-calibration
printf '%s ALL=(root) NOPASSWD: /usr/local/sbin/apply-touch-calibration\n' "$USER_NAME" \
    > /etc/sudoers.d/touch-calibrate
chmod 440 /etc/sudoers.d/touch-calibrate
visudo -cf /etc/sudoers.d/touch-calibrate >/dev/null
install -m 644 "$SRC/tools/touch-calibrate/touch-calibrate.desktop" \
    /usr/share/applications/touch-calibrate.desktop
ok "touch-calibrate installed"

log "6/6 Enable overlay"
if grep -q '^dtoverlay=sunfounder-tft35ips' "$CONFIG_TXT"; then
    ok "dtoverlay=sunfounder-tft35ips already enabled"
else
    cp "$CONFIG_TXT" "$CONFIG_TXT.bak-$(date +%Y%m%d%H%M%S)"
    printf 'dtoverlay=sunfounder-tft35ips\n' >> "$CONFIG_TXT"
    ok "added dtoverlay=sunfounder-tft35ips to $CONFIG_TXT"
fi

echo
ok "Install finished. Reboot to apply the driver."
echo "   After reboot, run 'Touch Calibration' from the desktop menu if the touch position is off."

if [ -z "${TFT35IPS_NO_REBOOT_PROMPT:-}" ] && [ -r /dev/tty ]; then
    read -r -p "Reboot now? (y/n) " -n 1 REPLY < /dev/tty || true
    echo
    case "$REPLY" in
        [Yy]) echo "Rebooting..."; sleep 1; reboot ;;
        *) echo "Skipping reboot." ;;
    esac
fi
