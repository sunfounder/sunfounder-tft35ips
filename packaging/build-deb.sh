#!/bin/bash
# Build the DKMS .deb for the SunFounder TFT 3.5" display.
#
#   bash packaging/build-deb.sh
#
# Output: dist/sunfounder-tft35ips-dkms_<version>_all.deb
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=sunfounder-tft35ips
VERSION="$(awk -F'"' '/^PACKAGE_VERSION/ {print $2}' "$ROOT/dkms.conf")"
STAGE="$ROOT/build/deb"
OUT="$ROOT/dist"

rm -rf "$STAGE"
mkdir -p "$OUT" "$STAGE/DEBIAN" \
    "$STAGE/usr/src/$PKG-$VERSION" \
    "$STAGE/usr/local/bin" "$STAGE/usr/local/sbin" \
    "$STAGE/usr/share/applications" "$STAGE/etc/sudoers.d"

cp -a "$ROOT/packaging/DEBIAN/." "$STAGE/DEBIAN/"
cp -a "$ROOT/driver/." "$STAGE/usr/src/$PKG-$VERSION/"
cp "$ROOT/dkms.conf" "$STAGE/usr/src/$PKG-$VERSION/"
dtc -@ -I dts -O dtb -o "$STAGE/usr/src/$PKG-$VERSION/$PKG.dtbo" \
    "$ROOT/driver/$PKG-overlay.dts"
rm -f "$STAGE/usr/src/$PKG-$VERSION/$PKG.dtbo.tmp"

install -m 755 "$ROOT/tools/sf-tft" "$STAGE/usr/local/bin/sf-tft"
install -m 755 "$ROOT/tools/touch-calibrate/touch-calibrate" \
    "$STAGE/usr/local/bin/touch-calibrate"
install -m 755 "$ROOT/tools/touch-calibrate/apply-touch-calibration" \
    "$STAGE/usr/local/sbin/apply-touch-calibration"
install -m 644 "$ROOT/tools/touch-calibrate/touch-calibrate.desktop" \
    "$STAGE/usr/share/applications/touch-calibrate.desktop"
install -m 755 "$ROOT/tools/sf-tft-x11-mode" "$STAGE/usr/local/bin/sf-tft-x11-mode"
mkdir -p "$STAGE/etc/xdg/autostart" "$STAGE/usr/share/sunfounder-tft35ips"
install -m 644 "$ROOT/tools/sunfounder-tft35ips-x11.desktop" \
    "$STAGE/etc/xdg/autostart/sunfounder-tft35ips-x11.desktop"
install -m 644 "$ROOT/tools/20-sunfounder-tft-x11.conf" \
    "$STAGE/usr/share/sunfounder-tft35ips/20-sunfounder-tft-x11.conf"
printf '%%sudo ALL=(root) NOPASSWD: /usr/local/sbin/apply-touch-calibration\n' \
    > "$STAGE/etc/sudoers.d/touch-calibrate"
chmod 440 "$STAGE/etc/sudoers.d/touch-calibrate"

# control file version
sed -i "s/^Version: .*/Version: $VERSION/" "$STAGE/DEBIAN/control"
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm"

dpkg-deb --root-owner-group --build "$STAGE" "$OUT/${PKG}-dkms_${VERSION}_all.deb"
echo "built: $OUT/${PKG}-dkms_${VERSION}_all.deb"
