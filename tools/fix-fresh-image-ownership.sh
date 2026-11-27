#!/bin/bash
# Fix ownership on a mounted fresh IPSW-root HFS+ image.
#
# The fresh root was assembled by a non-root host user, so most directories are
# owned by uid/gid 1000. iOS daemons that run as mobile (uid 501) then cannot
# create their Library directories, which shows up as "Failed to create data
# vault directory", "No path" and "Failed to create essential directory" crashes.
#
# This script changes only ownership inside an already-mounted image. It does
# not boot, push, flash, partition or SSH to the device.
#
# Usage:
#   sudo mount -o loop,rw,force,uid=0,gid=0,nls=utf8 IMAGE.img MOUNTPOINT
#   sudo tools/fix-fresh-image-ownership.sh MOUNTPOINT
#   sudo umount MOUNTPOINT
set -euo pipefail

ROOT=${1:-}
[ -n "$ROOT" ] || { echo "usage: $0 MOUNTPOINT" >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo 'STOP: run as root' >&2; exit 1; }
[ -d "$ROOT" ] || { echo "STOP: not a directory: $ROOT" >&2; exit 1; }
[ -d "$ROOT/System/Library/CoreServices/SpringBoard.app" ] || {
    echo "STOP: $ROOT does not look like an iOS root" >&2
    exit 1
}

# -h keeps symlinks as symlinks; broken Apple symlinks must not abort the pass.
# GNU chown exits 123 when some paths vanish during the walk; ignore only that
# exact status after verifying the important directories below.
chown -R -h 0:0 "$ROOT" || [ $? -eq 123 ]
chown -R -h 501:501 "$ROOT/private/var/mobile" || [ $? -eq 123 ]

check_owner() {
    local path=$1 expect=$2 actual
    actual=$(stat -c '%u:%g' "$ROOT/$path")
    if [ "$actual" != "$expect" ]; then
        echo "STOP: $path is $actual, expected $expect" >&2
        exit 1
    fi
}

check_owner private/var 0:0
check_owner private/var/mobile 501:501
check_owner private/var/mobile/Library 501:501
check_owner private/var/mobile/Library/SpringBoard 501:501
check_owner private/var/preferences 0:0
check_owner private/var/preferences/FeatureFlags/Domain 0:0

sync
echo "OWNERSHIP_FIX_PASS root=$ROOT"
