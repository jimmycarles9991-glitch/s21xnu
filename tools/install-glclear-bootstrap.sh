#!/bin/bash
# Install the Mesa/Panfrost GPU proof (/usr/bin/s21-gl-clear) into the cached
# launchd bootstrap of an iOS root image. A plist dropped into
# /System/Library/LaunchDaemons/ alone is ignored on this image; the job must
# be present in /System/Library/xpc/launchd.plist -> LaunchDaemons.
#
# Usage: OUT=/path/to/iosroot.img sudo bash tools/install-glclear-bootstrap.sh
# Defaults to build/iosroot-20H392-full.img next to the repo checkout.
set -euo pipefail

[ "$(id -u)" -eq 0 ] || exec sudo env "OUT=${OUT:-}" "$0" "$@"

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HOME/ios-sweet/build/iosroot-20H392-full.img}
MNT=/tmp/s21-iosroot-glclear
LOGPRE=$(mktemp /tmp/s21-glclear-fsck-pre.XXXXXX)
LOGPOST=$(mktemp /tmp/s21-glclear-fsck-post.XXXXXX)

[ -s "$OUT" ] || { echo "STOP: image $OUT missing or empty"; exit 1; }

fsck.hfsplus -fy "$OUT" > "$LOGPRE" 2>&1 || true
mkdir -p "$MNT"
mount -t hfsplus -o loop,force,rw "$OUT" "$MNT"
trap 'sync; umount "$MNT" 2>/dev/null || umount -l "$MNT"' EXIT

[ -x "$MNT/usr/bin/s21-gl-clear" ] || { echo "STOP: /usr/bin/s21-gl-clear missing in $OUT"; exit 1; }
P="$MNT/System/Library/xpc/launchd.plist"
[ -f "$P" ] || { echo "STOP: cached launchd.plist missing in $OUT"; exit 1; }

python3 - "$P" <<'PY'
import os, plistlib, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    pl = plistlib.load(f)
job = {
    'Label': 'com.s21.glclear',
    'ProgramArguments': ['/usr/bin/s21-gl-clear'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'StandardOutPath': '/dev/console',
    'StandardErrorPath': '/dev/console',
}
jobs = pl.setdefault('LaunchDaemons', {})
jobs['com.s21.glclear'] = job
st = os.stat(p)
with open(p, 'wb') as f:
    plistlib.dump(pl, f, fmt=plistlib.FMT_BINARY)
os.chown(p, st.st_uid, st.st_gid)
os.chmod(p, st.st_mode & 0o7777)
print('injected com.s21.glclear into cached launchd bootstrap (%d jobs)' % len(jobs))
PY

sync
umount "$MNT"
trap - EXIT

fsck.hfsplus -fy "$OUT" > "$LOGPOST" 2>&1 && tail -3 "$LOGPOST"
sha256sum "$OUT" > "$OUT.sha256"
if command -v img2simg >/dev/null 2>&1; then
  img2simg "$OUT" "${OUT%.img}.simg"
  sha256sum "${OUT%.img}.simg" >> "$OUT.sha256"
fi
cat "$OUT.sha256"
