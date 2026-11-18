#!/bin/bash
#
# SunFounder TFT 3.5" IPS display (ST7796S) installer
#
#   curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-tft35ips/main/install.sh | sudo bash
#
# Installs:
#   - the DRM driver: a prebuilt module from the GitHub release when one
#     matches the running kernel, otherwise built from source (DKMS when
#     available)
#   - the device-tree overlay
#   - sf-tft (doctor / calibrate / status) and the touch calibration tool
#   - dtoverlay=sunfounder-tft35ips in config.txt
#
# Environment overrides:
#   TFT35IPS_LOCAL=/path/to/checkout   build from a local checkout
#   TFT35IPS_REPO=... TFT35IPS_BRANCH=... TFT35IPS_DIR=...
#   TFT35IPS_NO_PREBUILT=1             always build from source
#   TFT35IPS_NO_DKMS=1                 plain make install instead of DKMS
#   TFT35IPS_NO_REBOOT_PROMPT=1        do not ask to reboot at the end

set -e

DRIVER_REPO="${TFT35IPS_REPO:-https://github.com/sunfounder/sunfounder-tft35ips.git}"
DRIVER_BRANCH="${TFT35IPS_BRANCH:-main}"
RELEASE_URL="${TFT35IPS_RELEASES:-https://github.com/sunfounder/sunfounder-tft35ips/releases/latest/download}"
DKMS_VERSION="1.0.0"
DKMS_SRC="/usr/src/sunfounder-tft35ips-$DKMS_VERSION"

KVER="$(uname -r)"
case "$(uname -m)" in
    aarch64) ARCH=arm64 ;;
    armv7l|armv6l) ARCH=arm ;;
    *) ARCH="$(uname -m)" ;;
esac

USER_NAME="${SUDO_USER:-$USER}"
USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6)
[ -z "$USER_HOME" ] && USER_HOME="$HOME"
INSTALL_DIR="${TFT35IPS_DIR:-$USER_HOME/sunfounder-tft35ips}"

log() { printf '\033[34m%s\033[0m\n' "$1"; }
ok()  { printf '\033[32m[✓]\033[0m %s\n' "$1"; }
warn() { printf '\033[33m[!]\033[0m %s\n' "$1"; }
die() { printf '\033[31m[✗]\033[0m %s\n' "$1" >&2; exit 1; }

if [ "$EUID" -ne 0 ]; then
    die "Please run as root: curl -sSL ... | sudo bash"
fi

CONFIG_TXT=""
for p in /boot/firmware/config.txt /boot/config.txt; do
    if [ -f "$p" ]; then CONFIG_TXT="$p"; break; fi
done
[ -z "$CONFIG_TXT" ] && die "config.txt not found"

OVERLAY_DIR=""
for p in /boot/firmware/overlays /boot/overlays; do
    if [ -d "$p" ]; then OVERLAY_DIR="$p"; break; fi
done
[ -z "$OVERLAY_DIR" ] && die "overlay directory not found"

log "1/4 Install dependencies"
apt-get update -qq || warn "apt-get update failed, continuing"
apt-get install -y git build-essential device-tree-compiler dkms \
    python3-evdev python3-gi python3-gi-cairo >/dev/null || warn "some packages were not installed"
ok "dependencies"

PREBUILT=0
log "2/4 Driver module"
if [ -z "${TFT35IPS_NO_PREBUILT:-}" ]; then
    MOD_ASSET="sunfounder-tft35ips-${KVER}-${ARCH}.ko.xz"
    OVL_ASSET="sunfounder-tft35ips-${KVER}-${ARCH}.dtbo"
    if curl -fsSL --retry 2 -o "/tmp/$MOD_ASSET" "$RELEASE_URL/$MOD_ASSET"; then
        xz -df "/tmp/$MOD_ASSET"
        mkdir -p "/lib/modules/$KVER/updates"
        install -m 644 "/tmp/${MOD_ASSET%.xz}" "/lib/modules/$KVER/updates/sunfounder-tft35ips.ko"
        depmod -a
        if curl -fsSL --retry 2 -o "/tmp/$OVL_ASSET" "$RELEASE_URL/$OVL_ASSET"; then
            install -m 644 "/tmp/$OVL_ASSET" "$OVERLAY_DIR/sunfounder-tft35ips.dtbo"
        fi
        PREBUILT=1
        ok "prebuilt module installed (kernel $KVER, $ARCH)"
    fi
fi

if [ "$PREBUILT" -eq 0 ]; then
    log "2/4 Build driver from source"
    if [ ! -d "/usr/src/linux-headers-$KVER" ]; then
        apt-get install -y "linux-headers-$KVER" >/dev/null 2>&1 \
            || apt-get install -y raspberrypi-kernel-headers >/dev/null 2>&1 \
            || true
    fi
    [ -d "/usr/src/linux-headers-$KVER" ] || die "kernel headers for $KVER are missing (install linux-headers-$KVER or use a prebuilt release)"

    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
    if [ -n "${TFT35IPS_LOCAL:-}" ] && [ -d "$TFT35IPS_LOCAL" ]; then
        SRC="$TFT35IPS_LOCAL"
    elif [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/driver/sunfounder-tft35ips.c" ]; then
        SRC="$SCRIPT_DIR"
    else
        rm -rf "$INSTALL_DIR"
        git clone --depth=1 --branch "$DRIVER_BRANCH" "$DRIVER_REPO" "$INSTALL_DIR"
        chown -R "$USER_NAME:$USER_NAME" "$INSTALL_DIR"
        SRC="$INSTALL_DIR"
    fi
    make -C "$SRC/driver" clean >/dev/null 2>&1 || true
    make -C "$SRC/driver" || die "driver build failed"

    if [ -z "${TFT35IPS_NO_DKMS:-}" ] && command -v dkms >/dev/null 2>&1; then
        rm -rf "$DKMS_SRC"
        install -d "$DKMS_SRC"
        cp -a "$SRC/driver/." "$DKMS_SRC/"
        cp "$SRC/dkms.conf" "$DKMS_SRC/dkms.conf"
        dkms remove -m sunfounder-tft35ips -v "$DKMS_VERSION" --all >/dev/null 2>&1 || true
        dkms add -m sunfounder-tft35ips -v "$DKMS_VERSION" >/dev/null
        dkms build -m sunfounder-tft35ips -v "$DKMS_VERSION" >/dev/null
        dkms install --force -m sunfounder-tft35ips -v "$DKMS_VERSION" >/dev/null
        ok "driver built and installed with DKMS"
    else
        make -C "$SRC/driver" modules_install
        depmod -a
        ok "driver built and installed"
    fi
    install -m 644 "$SRC/driver/sunfounder-tft35ips.dtbo" "$OVERLAY_DIR/sunfounder-tft35ips.dtbo"
fi

if [ ! -f "$OVERLAY_DIR/sunfounder-tft35ips.dtbo" ]; then
    # prebuilt module was used but the overlay asset was missing: build it
    SRC="${SRC:-$INSTALL_DIR}"
    if [ ! -f "$SRC/driver/sunfounder-tft35ips-overlay.dts" ]; then
        rm -rf "$INSTALL_DIR"
        git clone --depth=1 --branch "$DRIVER_BRANCH" "$DRIVER_REPO" "$INSTALL_DIR"
        SRC="$INSTALL_DIR"
    fi
    dtc -@ -I dts -O dtb -o /tmp/sunfounder-tft35ips.dtbo \
        "$SRC/driver/sunfounder-tft35ips-overlay.dts"
    install -m 644 /tmp/sunfounder-tft35ips.dtbo "$OVERLAY_DIR/sunfounder-tft35ips.dtbo"
fi
ok "overlay installed"

log "3/4 Install tools"
SRC="${SRC:-$INSTALL_DIR}"
if [ ! -f "$SRC/tools/sf-tft" ]; then
    rm -rf "$INSTALL_DIR"
    git clone --depth=1 --branch "$DRIVER_BRANCH" "$DRIVER_REPO" "$INSTALL_DIR"
    chown -R "$USER_NAME:$USER_NAME" "$INSTALL_DIR"
    SRC="$INSTALL_DIR"
fi
install -m 755 "$SRC/tools/sf-tft" /usr/local/bin/sf-tft
install -m 755 "$SRC/tools/touch-calibrate/touch-calibrate" /usr/local/bin/touch-calibrate
install -m 755 "$SRC/tools/touch-calibrate/apply-touch-calibration" /usr/local/sbin/apply-touch-calibration
printf '%s ALL=(root) NOPASSWD: /usr/local/sbin/apply-touch-calibration\n' "$USER_NAME" \
    > /etc/sudoers.d/touch-calibrate
chmod 440 /etc/sudoers.d/touch-calibrate
visudo -cf /etc/sudoers.d/touch-calibrate >/dev/null
install -m 644 "$SRC/tools/touch-calibrate/touch-calibrate.desktop" \
    /usr/share/applications/touch-calibrate.desktop
ok "sf-tft and touch-calibrate installed"

log "4/4 Enable overlay"
if grep -q '^dtoverlay=sunfounder-tft35ips' "$CONFIG_TXT"; then
    ok "dtoverlay=sunfounder-tft35ips already enabled"
else
    cp "$CONFIG_TXT" "$CONFIG_TXT.bak-$(date +%Y%m%d%H%M%S)"
    printf 'dtoverlay=sunfounder-tft35ips\n' >> "$CONFIG_TXT"
    ok "added dtoverlay=sunfounder-tft35ips to $CONFIG_TXT"
fi

echo
ok "Install finished. Reboot to apply the driver."
echo "   Check with:  sf-tft doctor"
echo "   Calibrate:   sf-tft calibrate"

if [ -z "${TFT35IPS_NO_REBOOT_PROMPT:-}" ] && [ -r /dev/tty ]; then
    read -r -p "Reboot now? (y/n) " -n 1 REPLY < /dev/tty || true
    echo
    case "$REPLY" in
        [Yy]) echo "Rebooting..."; sleep 1; reboot ;;
        *) echo "Skipping reboot." ;;
    esac
fi
