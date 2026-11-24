#!/bin/bash
#
# SunFounder TFT 3.5" IPS display (ST7796S) installer
#
#   curl -sSL https://raw.githubusercontent.com/sunfounder/sunfounder-tft35ips/main/install.sh | sudo bash
#
# Installs:
#   - the DRM kernel module: downloaded prebuilt from the GitHub release, built
#     by CI for the running kernel (no kernel headers needed)
#   - the device-tree overlay
#   - sf-tft (doctor / calibrate / status) and the touch calibration tool
#   - dtoverlay=sunfounder-tft35ips in config.txt
#
# Environment overrides:
#   TFT35IPS_BUILD_FROM_SOURCE=1   compile locally instead of downloading (needs
#                                  kernel headers; DKMS is used when available)
#   TFT35IPS_COMMIT=<sha>          install tools/overlay from a specific commit
#   TFT35IPS_REPO=... TFT35IPS_BRANCH=... TFT35IPS_DIR=...
#   TFT35IPS_RELEASES=<url>        release download base URL
#   TFT35IPS_NO_REBOOT_PROMPT=1    do not ask to reboot at the end

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

download() {
    # download <url> <dest>; curl, wget or python3, whichever is available
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$1" "$2"
    else
        return 1
    fi
}


if [ "$EUID" -ne 0 ]; then
    die "Please run as root: curl -sSL ... | sudo bash"
fi

CONFIG_TXT=""
for p in /boot/firmware/config.txt /boot/config.txt; do
    if [ -f "$p" ]; then CONFIG_TXT="$p"; break; fi
done
[ -z "$CONFIG_TXT" ] && die "config.txt not found"

# Ubuntu keeps the firmware files under /boot/firmware/<os_prefix>/, so honour
# the os_prefix set in config.txt before falling back to the usual paths
OS_PREFIX=""
if [ -n "$CONFIG_TXT" ]; then
    OS_PREFIX=$(sed -n 's/^[[:space:]]*os_prefix=\([^[:space:]]*\).*/\1/p' "$CONFIG_TXT" | tail -1)
fi
BASE_DIR=/boot/firmware
[ -d "$BASE_DIR" ] || BASE_DIR=/boot
OVERLAY_DIR=""
for p in "$BASE_DIR/${OS_PREFIX}overlays" "$BASE_DIR/overlays" /boot/overlays; do
    if [ -d "$p" ]; then OVERLAY_DIR="$p"; break; fi
done
if [ -z "$OVERLAY_DIR" ]; then
    mkdir -p "$BASE_DIR/${OS_PREFIX}overlays" 2>/dev/null && OVERLAY_DIR="$BASE_DIR/${OS_PREFIX}overlays"
fi
[ -z "$OVERLAY_DIR" ] && die "overlay directory not found"

log "1/4 Install dependencies"
apt-get update -qq || warn "apt-get update failed, continuing"
apt-get install -y git device-tree-compiler python3-evdev python3-gi python3-gi-cairo \
    >/dev/null || warn "some packages were not installed"
command -v git >/dev/null 2>&1 || die "git is required but could not be installed (apt-get install -y git)"
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || \
    command -v python3 >/dev/null 2>&1 || die "curl, wget or python3 is required to download files"
ok "dependencies"

log "2/4 Download files"
rm -rf "$INSTALL_DIR"
git clone --depth=1 --branch "$DRIVER_BRANCH" "$DRIVER_REPO" "$INSTALL_DIR"
chown -R "$USER_NAME:$USER_NAME" "$INSTALL_DIR"
SRC="$INSTALL_DIR"
ok "repository cloned to $INSTALL_DIR"

log "3/4 Install driver module"
if [ "${TFT35IPS_BUILD_FROM_SOURCE:-}" = "1" ]; then
    apt-get install -y build-essential dkms >/dev/null || warn "some packages were not installed"
    if [ ! -d "/usr/src/linux-headers-$KVER" ]; then
        apt-get install -y "linux-headers-$KVER" >/dev/null 2>&1 \
            || apt-get install -y raspberrypi-kernel-headers >/dev/null 2>&1 \
            || true
    fi
    [ -d "/usr/src/linux-headers-$KVER" ] || die "kernel headers for $KVER are missing"

    make -C "$SRC/driver" clean >/dev/null 2>&1 || true
    make -C "$SRC/driver" || die "driver build failed"
    rm -rf "$DKMS_SRC"
    install -d "$DKMS_SRC"
    cp -a "$SRC/driver/." "$DKMS_SRC/"
    cp "$SRC/dkms.conf" "$DKMS_SRC/dkms.conf"
    if command -v dkms >/dev/null 2>&1; then
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
else
    MOD_ASSET="sunfounder-tft35ips-${KVER}-${ARCH}.ko.xz"
    DEB_ASSET="sunfounder-tft35ips-dkms_${DKMS_VERSION}_all.deb"
    if download "$RELEASE_URL/$MOD_ASSET" "/tmp/$MOD_ASSET"; then
        xz -df "/tmp/$MOD_ASSET"
        mkdir -p "/lib/modules/$KVER/updates"
        install -m 644 "/tmp/${MOD_ASSET%.xz}" "/lib/modules/$KVER/updates/sunfounder-tft35ips.ko"
        depmod -a
        ok "prebuilt module installed for $KVER ($ARCH)"
    elif download "$RELEASE_URL/$DEB_ASSET" "/tmp/$DEB_ASSET"; then
        # no binary for this exact kernel: use the DKMS package, which builds
        # the module on this machine
        apt-get install -y "/tmp/$DEB_ASSET" || dpkg -i "/tmp/$DEB_ASSET" || apt-get -f install -y
        if ! ls /lib/modules/$KVER/updates/dkms/sunfounder-tft35ips.ko* >/dev/null 2>&1; then
            die "the DKMS build failed for $KVER.
     The driver does not compile against this kernel yet; see:
       dkms status
       tail -40 /var/lib/dkms/sunfounder-tft35ips/1.0.0/build/make.log"
        fi
        depmod -a >/dev/null 2>&1 || true
        ok "DKMS package installed (module built for $KVER)"
    else
        die "no prebuilt module for kernel $KVER ($ARCH) and the DKMS package
     could not be downloaded.
     - build locally instead (needs kernel headers for $KVER):
         curl -sSL ... | sudo TFT35IPS_BUILD_FROM_SOURCE=1 bash
     - or ask us to cover this kernel (paste the output of 'uname -r'):
         https://github.com/sunfounder/sunfounder-tft35ips/issues"
    fi
fi

if command -v update-initramfs >/dev/null 2>&1; then
    # some distros ship the module inside the initramfs; without this the old
    # copy keeps loading after an upgrade
    update-initramfs -u -k all >/dev/null 2>&1 || warn "update-initramfs failed"
    ok "initramfs updated"
fi

OVL_ASSET="sunfounder-tft35ips-${KVER}-${ARCH}.dtbo"
if ! download "$RELEASE_URL/$OVL_ASSET" "/tmp/$OVL_ASSET"; then
    dtc -@ -I dts -O dtb -o "/tmp/$OVL_ASSET" "$SRC/driver/sunfounder-tft35ips-overlay.dts" \
        || die "overlay build failed"
fi
install -m 644 "/tmp/$OVL_ASSET" "$OVERLAY_DIR/sunfounder-tft35ips.dtbo"
[ -f "$OVERLAY_DIR/sunfounder-tft35ips.dtbo" ] || die "overlay was not installed into $OVERLAY_DIR"
ok "overlay installed in $OVERLAY_DIR"

log "4/4 Install tools and enable the overlay"
install -m 755 "$SRC/tools/sf-tft" /usr/local/bin/sf-tft
install -m 755 "$SRC/tools/touch-calibrate/touch-calibrate" /usr/local/bin/touch-calibrate
install -m 755 "$SRC/tools/touch-calibrate/apply-touch-calibration" /usr/local/sbin/apply-touch-calibration
printf '%s ALL=(root) NOPASSWD: /usr/local/sbin/apply-touch-calibration\n' "$USER_NAME" \
    > /etc/sudoers.d/touch-calibrate
chmod 440 /etc/sudoers.d/touch-calibrate
visudo -cf /etc/sudoers.d/touch-calibrate >/dev/null
install -m 644 "$SRC/tools/touch-calibrate/touch-calibrate.desktop" \
    /usr/share/applications/touch-calibrate.desktop
install -m 755 "$SRC/tools/sf-tft-x11-mode" /usr/local/bin/sf-tft-x11-mode
install -m 755 "$SRC/tools/sf-tft-calibrate-once" /usr/local/bin/sf-tft-calibrate-once
mkdir -p /etc/xdg/autostart
install -m 644 "$SRC/tools/sunfounder-tft35ips-x11.desktop" \
    /etc/xdg/autostart/sunfounder-tft35ips-x11.desktop
install -m 644 "$SRC/tools/sunfounder-tft35ips-calibrate.desktop" \
    /etc/xdg/autostart/sunfounder-tft35ips-calibrate.desktop

# the driver only works after a reboot, so the calibration is offered by the
# autostart helper on the first desktop login (it stops once a matrix exists)

# X11-only desktops (Kali and friends) need the fbdev driver: the modesetting
# driver flips without damage and the panel would freeze on the first frame.
if [ -d /etc/X11 ] && [ -d /usr/share/xsessions ] && \
   [ ! -d /usr/share/wayland-sessions ]; then
    FB=$(grep -l sunfounder /sys/class/graphics/fb*/name 2>/dev/null | head -1 |
         sed 's#/sys/class/graphics/\(fb[0-9]*\)/name#/dev/\1#')
    if [ -z "$FB" ]; then
        # module may not be loaded yet; the vc4 GPU usually owns fb0
        if grep -qs vc4 /sys/class/graphics/fb0/name; then FB=/dev/fb1; else FB=/dev/fb0; fi
    fi
    mkdir -p /etc/X11/xorg.conf.d
    cat > /etc/X11/xorg.conf.d/20-sunfounder-tft-x11.conf <<EOF
Section "Device"
    Identifier "SunFounder TFT panel"
    Driver     "fbdev"
    Option     "fbdev" "$FB"
EndSection
EOF
    ok "X11 fallback installed (fbdev $FB, single display)"
fi
ok "sf-tft and touch-calibrate installed"

if grep -q '^dtoverlay=sunfounder-tft35ips' "$CONFIG_TXT"; then
    ok "dtoverlay=sunfounder-tft35ips already enabled"
else
    cp "$CONFIG_TXT" "$CONFIG_TXT.bak-$(date +%Y%m%d%H%M%S)"
    printf 'dtoverlay=sunfounder-tft35ips\n' >> "$CONFIG_TXT"
    ok "added dtoverlay=sunfounder-tft35ips to $CONFIG_TXT"
fi

# the tools are installed under /usr/local, the clone is no longer needed
rm -rf "$SRC"

echo
ok "Install finished. Reboot to apply the driver."
echo "   Touch calibration opens automatically on the first desktop login."
echo "   Check with:  sf-tft doctor"
echo "   Calibrate:   sf-tft calibrate"
echo "   Uninstall:   sudo sf-tft uninstall"

if [ -z "${TFT35IPS_NO_REBOOT_PROMPT:-}" ] && [ -r /dev/tty ]; then
    read -r -p "Reboot now? (y/n) " -n 1 REPLY < /dev/tty || true
    echo
    case "$REPLY" in
        [Yy]) echo "Rebooting..."; sleep 1; reboot ;;
        *) echo "Skipping reboot." ;;
    esac
fi
