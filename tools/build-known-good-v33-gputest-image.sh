#!/bin/bash
# Build a host-only S21 root image from the known-good Wi-Fi/BT backup lineage,
# with only the two changes that are actually missing from it:
#
#   - linux-mesa-ios V33 libs21eagl.dylib (all other V33 artifacts are already
#     byte-identical in the known-good image)
#   - com.s21.gputest cached/standalone launchd job with PAINT_ONCE and the
#     proven jetsam override
#
# This deliberately does NOT start from the fresh IPSW root.  The fresh root
# reached only the Apple progress bar on 2026-11-27/28; the pre-asset-copy
# backup previously reached the iOS UI.  Comparing the two images shows the
# cached launchd jobs and activation/migration plists are structurally
# identical, so the fresh root's data lineage is the outstanding variable.
#
# Host-only. This never boots, kexecs, flashes, repartitions, SSHes or touches
# the active device image.  It writes only:
#   /mnt/7FEF-EA6C/ios/build/iosroot-s21-known-good-v33-gputest.img
#   /mnt/7FEF-EA6C/ios/build/iosroot-s21-known-good-v33-gputest.img.sha256
#
# Run as root:
#   sudo /home/tom/s21xnu-linux-mesa/tools/build-known-good-v33-gputest-image.sh
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BASE=${BASE:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-wifibt-v1.img.pre-asset-copy}
OUT=${OUT:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-known-good-v33-gputest.img}
MNT=${MNT:-/mnt/iosroot-known-good-v33-gputest}
REPO=${REPO:-/home/tom/s21xnu-linux-mesa}
MESA=${MESA:-/home/tom/Downloads/ios-mesa-g78-v33}

SIZE=17179869184
BASE_SHA=76600e35d06fa53a50d357d64affa6d90897a133694f5905945a5caa3b466daf
EAGL_SHA=b8b79eb8a2bbaf06427d92b11840e5a37d1157b4c4b280b72a7fc15e316d6b0a
GPU_TEST_SHA=a00d00ff4584e7b637d2c13619244895c3debae8fded33ffb9063ef7c6f3b6ea
KNOWN_OLD_EAGL_SHA=d4f8b1a88b647ee4b6266676069d8e85ba6168d3eef802a976ef43b918e3e998

die() { echo "STOP: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
need_hash() { [ "$(hash "$1")" = "$2" ] || die "hash mismatch: $1"; }

cleanup() {
  if mountpoint -q "$MNT" 2>/dev/null; then
    sync || true
    umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
  fi
  [ ! -d "$MNT" ] || rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || die 'must run as root via sudo'
[ -f "$BASE" ] || die "missing known-good base: $BASE"
[ "$(stat -c %s "$BASE")" = "$SIZE" ] || die 'base image size mismatch'
need_hash "$BASE" "$BASE_SHA"
[ ! -e "$OUT" ] || die "refusing to overwrite $OUT; move it aside first"
[ ! -e "$OUT.sha256" ] || die "refusing to overwrite $OUT.sha256"
[ ! -e "$MNT" ] || die "mountpoint already exists: $MNT"
[ -f "$MESA/libs21eagl.dylib" ] || die "missing $MESA/libs21eagl.dylib"
need_hash "$MESA/libs21eagl.dylib" "$EAGL_SHA"
[ -f "$REPO/tools/s21-gputest-job.py" ] || die 'missing tools/s21-gputest-job.py'
command -v fsck.hfsplus >/dev/null 2>&1 || die 'fsck.hfsplus required'

echo "known-good base verified: $BASE_SHA"
echo "copying sparse image (base lineage preserved)"
cp --sparse=always -- "$BASE" "$OUT"
[ "$(stat -c %s "$OUT")" = "$SIZE" ] || die 'copied image size mismatch'

mkdir -p "$MNT"
mount -t hfsplus -o loop,force,rw,nls=utf8 "$OUT" "$MNT"

# The base must still be the proven Wi-Fi/BT image before anything is changed.
[ -f "$MNT/System/Library/xpc/launchd.plist" ] || die 'cached launchd.plist missing'
[ -f "$MNT/usr/bin/s21-gpu-test" ] || die 's21-gpu-test missing in base'
need_hash "$MNT/usr/bin/s21-gpu-test" "$GPU_TEST_SHA"
[ -f "$MNT/usr/bin/libs21eagl.dylib" ] || die 'libs21eagl.dylib missing in base'
need_hash "$MNT/usr/bin/libs21eagl.dylib" "$KNOWN_OLD_EAGL_SHA"
[ -f "$MNT/usr/bin/bluetool_stub" ] || die 'bluetool_stub missing in base'
[ -d "$MNT/Applications/Opaline.app" ] || die 'Opaline missing in base'
python3 - "$MNT" <<'PY'
import os, plistlib, sys
root = sys.argv[1]
with open(os.path.join(root, 'System/Library/xpc/launchd.plist'), 'rb') as f:
    jobs = plistlib.load(f).get('LaunchDaemons', {})
bb = [j for j in jobs.values() if isinstance(j, dict) and j.get('Label') == 'com.apple.backboardd']
if not bb or bb[0].get('EnvironmentVariables', {}).get('DYLD_INSERT_LIBRARIES') != '/usr/bin/libs21eagl.dylib':
    raise SystemExit('STOP: base backboardd EAGL shim missing')
bt = [j for j in jobs.values() if isinstance(j, dict) and j.get('Label') == 'com.apple.BlueTool']
if not bt or bt[0].get('ProgramArguments') != ['/usr/bin/bluetool_stub']:
    raise SystemExit('STOP: base BlueTool stub missing')
clock = jobs.get('/System/Library/LaunchDaemons/000.s21.clockrestore.plist')
if not clock or clock.get('ProgramArguments') != ['/usr/bin/malinop']:
    raise SystemExit('STOP: base clock restore job missing')
print('BASE_LAUNCHD_PASS')
PY

echo "installing V33 EAGL and com.s21.gputest"
install -m 644 "$MESA/libs21eagl.dylib" "$MNT/usr/bin/libs21eagl.dylib"
chown 0:0 "$MNT/usr/bin/libs21eagl.dylib"
need_hash "$MNT/usr/bin/libs21eagl.dylib" "$EAGL_SHA"
python3 "$REPO/tools/s21-gputest-job.py" "$MNT"

python3 - "$MNT" <<'PY'
import os, plistlib, sys
root = sys.argv[1]
KEY = '/System/Library/LaunchDaemons/com.s21.gputest.plist'
JOB = {
    'Label': 'com.s21.gputest',
    'ProgramArguments': ['/usr/bin/s21-gpu-test'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'StandardOutPath': '/dev/console',
    'StandardErrorPath': '/dev/console',
    'EnvironmentVariables': {'S21_GPU_TEST_PAINT_ONCE': '1'},
}
OVERRIDE = {
    'JetsamPriority': 40,
    'ActiveHardMemoryLimit': 128,
    'InactiveHardMemoryLimit': 128,
    'ActiveSoftMemoryLimit': 96,
}
with open(os.path.join(root, 'System/Library/xpc/launchd.plist'), 'rb') as f:
    jobs = plistlib.load(f).get('LaunchDaemons', {})
if jobs.get(KEY) != JOB:
    raise SystemExit('STOP: cached com.s21.gputest job wrong')
if [j for j in jobs.values() if isinstance(j, dict) and j.get('Label') == 'com.s21.gputest' and j != JOB]:
    raise SystemExit('STOP: duplicate com.s21.gputest job in cached launchd')
with open(os.path.join(root, KEY.lstrip('/')), 'rb') as f:
    if plistlib.load(f) != JOB:
        raise SystemExit('STOP: standalone com.s21.gputest plist wrong')
for rel in ('System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist',
            'System/Library/LaunchDaemons/com.apple.jetsamproperties.D221.plist'):
    with open(os.path.join(root, rel), 'rb') as f:
        over = plistlib.load(f)['Version4']['Daemon']['Override'].get('com.s21.gputest')
    if over != OVERRIDE:
        raise SystemExit('STOP: com.s21.gputest jetsam override wrong in ' + rel)
bb = [j for j in jobs.values() if isinstance(j, dict) and j.get('Label') == 'com.apple.backboardd']
if not bb or bb[0].get('EnvironmentVariables', {}).get('DYLD_INSERT_LIBRARIES') != '/usr/bin/libs21eagl.dylib':
    raise SystemExit('STOP: backboardd EAGL shim was disturbed')
print('KNOWN_GOOD_V33_GPUTEST_PASS')
PY

sync
umount "$MNT"
mounted=0
rmdir "$MNT" 2>/dev/null || true

fsck.hfsplus -fy "$OUT" > /tmp/opencode/hfs-known-good-v33-gputest.log 2>&1 || {
  tail -80 /tmp/opencode/hfs-known-good-v33-gputest.log >&2
  die 'post fsck failed'
}
grep -q 'appears to be OK' /tmp/opencode/hfs-known-good-v33-gputest.log || die 'post fsck did not report OK'

sha256sum "$OUT" > "$OUT.sha256"
final_sha=$(awk '{print $1}' "$OUT.sha256")
[ "$final_sha" != "$BASE_SHA" ] || die 'final image identical to base'
stat -c '%n %s %U:%G %a' "$OUT"
cat "$OUT.sha256"
echo "KNOWN_GOOD_V33_GPUTEST_READY sha=$final_sha base=$BASE_SHA eagl=$EAGL_SHA gpu_job=paint-once+jetsam"
