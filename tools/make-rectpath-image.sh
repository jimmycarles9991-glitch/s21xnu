#!/bin/bash
# Build a read-only-to-device test image that forces UIKit's rectangular
# screen bounding-path utility.  This avoids the synthetic iPhone-X complex
# bounding-path asset lookup that crash-loops SpringBoard on the Wi-Fi/BT root.
#
# Source: current host Wi-Fi/BT image with the stock asset copy.
# New:    /mnt/7FEF-EA6C/ios/build/iosroot-s21-wifibt-v1.rectpath.img
#
# This script only creates a host image.  It never boots, kexecs, flashes,
# repartitions, SSHes or changes the active device image.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

SRC=${SRC:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-wifibt-v1.img}
OUT=${OUT:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-wifibt-v1.rectpath.img}
MNT=${MNT:-/mnt/iosroot-rectpath}
REPO=${REPO:-/home/tom/s21xnu-linux-mesa}
PATCHER=$REPO/tools/patch-uikit-rectangular-path.py

SIZE=17179869184
SRC_SHA=3d57c794d501464883a404d04c108e886d2fd2a953c5d10da29fd680860da7d7
DSC_SHA_BEFORE=511377c04e3054b905fcfb83bb2d1e6ffdb3b838ae21969fd1962dbfcab9880b

die() { echo "STOP: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }

[ -f "$SRC" ] || die "missing source image: $SRC"
[ -f "$PATCHER" ] || die "missing patcher: $PATCHER"
[ "$(hash "$SRC")" = "$SRC_SHA" ] || die "source image hash mismatch"
[ "$(stat -c %s "$SRC")" -eq "$SIZE" ] || die "source image size mismatch"
[ ! -e "$OUT" ] || die "$OUT already exists; move it aside first"
[ ! -e "$OUT.sha256" ] || die "$OUT.sha256 already exists"

avail=$(df -Pk "$(dirname "$OUT")" | awk 'NR==2 {print $4}')
[ "${avail:-0}" -gt 18000000 ] || die "need >18,000,000 KiB free, have ${avail:-0} KiB"

echo "source verified: $SRC_SHA"
cp --reflink=auto "$SRC" "$OUT"
chmod 644 "$OUT"

mounted=0
cleanup() {
    if [ "$mounted" -eq 1 ]; then
        sync || true
        umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

mkdir -p "$MNT"
mount -t hfsplus -o loop,force,rw "$OUT" "$MNT"
mounted=1

DSC=$MNT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.03
[ "$(hash "$DSC")" = "$DSC_SHA_BEFORE" ] || die "source DSC subcache hash mismatch"

python3 "$PATCHER" "$MNT"
python3 "$PATCHER" "$MNT" --check

sync
umount "$MNT"
mounted=0
trap - EXIT

fsck.hfsplus -fy "$OUT" > /tmp/opencode/hfs-rectpath.log 2>&1 || {
    tail -40 /tmp/opencode/hfs-rectpath.log >&2
    die "fsck.hfsplus failed"
}
grep -q 'appears to be OK' /tmp/opencode/hfs-rectpath.log || {
    tail -40 /tmp/opencode/hfs-rectpath.log >&2
    die "fsck.hfsplus did not report OK"
}

sha256sum "$OUT" > "$OUT.sha256"
new_sha=$(awk '{print $1}' "$OUT.sha256")
stat -c '%n %s %U:%G %a' "$OUT"
cat "$OUT.sha256"
echo "RECTPATH_IMAGE_READY sha=$new_sha src=$SRC_SHA patch=isScreenSupported:false"
