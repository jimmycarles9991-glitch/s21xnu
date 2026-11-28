#!/bin/bash
# Build one fresh 16 GiB iOS 16.7.16 root image for the S21 with the latest
# userland, linux-mesa-ios artifacts and the com.s21.gputest LaunchDaemon.
#
# Host-only. This never boots, kexecs, flashes, repartitions, SSHes or touches
# the active device image. It writes only:
#   /mnt/7FEF-EA6C/ios/build/iosroot-s21-latest-gputest.img
#   /mnt/7FEF-EA6C/ios/build/iosroot-s21-latest-gputest.img.sha256
#
# The XNU kernel is not baked into the root image; boot it with the verified
# /home/tom/Downloads/kernel-wifi-bt/kernel.development.sweet.
#
# Run as root (the HFS+ mounts and chown require it):
#   sudo /home/tom/s21xnu-linux-mesa/tools/build-latest-s21-image.sh
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IPSW=${IPSW:-/home/tom/Downloads/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw}
REPO=${REPO:-/home/tom/s21xnu-linux-mesa}
WORK=${WORK:-/home/tom/s21-fresh-build/latest}
DMGDIR=${DMGDIR:-/home/tom/s21-fresh-build/dmg}
OUT=${OUT:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-latest-gputest.img}
SOURCE_IMAGE=${SOURCE_IMAGE:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-wifibt-v1.pre-asset-copy.fixed.img}
STAGE=$WORK/stage
# fresh-image-ui-fixes.py expects a source *root* containing Library/Wallpaper.
# Older runs staged the wallpaper tree directly at $WORK/wallpaper; migrate it
# automatically so a rerun does not need manual file moves.
WALL_SOURCE_ROOT=${WALL_SOURCE_ROOT:-$WORK}
WALL=$WALL_SOURCE_ROOT/Library/Wallpaper
LEGACY_WALL=$WORK/wallpaper
SRC_MNT=${SRC_MNT:-/mnt/iosroot-gputest-source}
BUILD_MNT=${BUILD_MNT:-/mnt/iosroot-latest-gputest}
FINAL_MNT=${FINAL_MNT:-/mnt/iosroot-latest-final}
TOOLS=${TOOLS:-/home/tom/Downloads/ios-tools-wifi-bt}
MESA=${MESA:-/home/tom/Downloads/ios-mesa-g78-v33}
BLOBS=${BLOBS:-/home/tom/s21}
KERNEL=${KERNEL:-/home/tom/Downloads/kernel-wifi-bt/kernel.development.sweet}
OPALINE_IPA=${OPALINE_IPA:-/home/tom/Downloads/s21-trial-ipas/Opaline_1.12.0.ipa}

IPSW_SHA=ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb
OS_SHA=80f426d5b8fca8552f95be427434f1911dbb4c2c6d28aad94f2426ddd0c92483
SYSOS_SHA=9cd738f3b0169055549df84ad6d8d13e50a2da39af618ce0522ca4e06df86849
APPS_SHA=391f2df322c3172b021b2874f6da5c3e78b70b3df8f502e1a4af2b1d715c4608
STAGE_HASHES='55da3071197242625de090599c17ef6013cf474f02629fca2ba24db2ada6e05d fcmap.txt
4ee25214d00aac12e6361cfe284f6105080e39dd90ff26183bd4211b34272a69 shaders.smsp
0c06367e267e268896715120c116b6a5500303f63cbe70d364d8321cb77e0d81 variants.txt'
# Exact nested-installer hash from the run that stopped before the UI fixes.
# Only this known stale partial is removed automatically; any other existing
# output is left alone and the script refuses to overwrite it.
KNOWN_PARTIAL_SHA=8293431c3686fad89ead2eff2cd0359fdce08f67343c503de250f4c5ae82d538

die() { echo "STOP: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
check_hash() { [ "$(hash "$1")" = "$2" ]; }
need_hash() { check_hash "$1" "$2" || die "hash mismatch: $1"; }
png_heic_count() {
  local dir=${1:-$WALL}
  [ -d "$dir" ] || { echo 0; return 0; }
  # The trailing "exit 0" is required: pipefail otherwise aborts on the last
  # batched file when it is not one of the PNG-as-HEIC payloads.
  LC_ALL=C find "$dir" -type f -iname '*.heic' -exec sh -c '
    for f do
      magic=$(head -c 8 "$f" | od -An -tx1 | tr -d " \n")
      [ "$magic" = 89504e470d0a1a0a ] && echo x
    done
    exit 0
  ' _ {} + | wc -l
}
known_failed_partial() {
  [ -f "$OUT" ] || return 1
  [ -f "$OUT.sha256" ] || return 1
  [ "$(stat -c %s "$OUT")" = 17179869184 ] || return 1
  [ "$(wc -l < "$OUT.sha256")" -eq 1 ] || return 1
  grep -qxF "$KNOWN_PARTIAL_SHA  $OUT" "$OUT.sha256" || return 1
  [ "$(hash "$OUT")" != "$KNOWN_PARTIAL_SHA" ] || return 1
}
remove_known_failed_partial() {
  if [ -e "$OUT" ] || [ -e "$OUT.sha256" ]; then
    if known_failed_partial; then
      echo "removing stale failed partial image from the interrupted UI-fix build"
      rm -f -- "$OUT" "$OUT.sha256"
    fi
  fi
}
migrate_wallpaper_stage() {
  [ "$WALL" != "$LEGACY_WALL" ] || return 0
  [ ! -e "$WALL" ] || return 0
  [ -d "$LEGACY_WALL" ] || return 0
  echo "migrating staged wallpaper to $WALL"
  mkdir -p -- "$(dirname "$WALL")"
  mv -- "$LEGACY_WALL" "$WALL"
}
cleanup() {
  for m in "$FINAL_MNT" "$BUILD_MNT" "$SRC_MNT"; do
    if mountpoint -q "$m" 2>/dev/null; then
      sync || true
      umount "$m" 2>/dev/null || umount -l "$m" 2>/dev/null || true
    fi
    [ ! -d "$m" ] || rmdir "$m" 2>/dev/null || true
  done
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || die 'must run as root via sudo'
[ -f "$IPSW" ] || die "missing IPSW: $IPSW"
[ -f "$REPO/tools/mkiosroot.sh" ] || die "missing mkiosroot.sh in $REPO"
[ -f "$REPO/tools/build-fresh-image.sh" ] || die "missing build-fresh-image.sh in $REPO"
[ -f "$REPO/tools/s21-gputest-job.py" ] || die "missing s21-gputest-job.py in $REPO"
remove_known_failed_partial
[ ! -e "$OUT" ] || die "refusing to overwrite $OUT; move it aside first"
[ ! -e "$OUT.sha256" ] || die "refusing to overwrite $OUT.sha256"
for m in "$SRC_MNT" "$BUILD_MNT" "$FINAL_MNT"; do
  [ ! -e "$m" ] || die "mountpoint already exists: $m"
done
command -v apfs-fuse >/dev/null 2>&1 || die 'apfs-fuse required'
command -v mkfs.hfsplus >/dev/null 2>&1 || die 'mkfs.hfsplus required'
command -v fsck.hfsplus >/dev/null 2>&1 || die 'fsck.hfsplus required'
command -v rsync >/dev/null 2>&1 || die 'rsync required'
command -v unzip >/dev/null 2>&1 || die 'unzip required'
echo "verifying IPSW: $(hash "$IPSW")"
need_hash "$IPSW" "$IPSW_SHA"

mkdir -p "$WORK" "$STAGE" "$DMGDIR"
migrate_wallpaper_stage
mkdir -p "$WALL"
OS=$DMGDIR/087-86698-044.dmg
SYSOS=$DMGDIR/087-86593-047.dmg
APPS=$DMGDIR/087-86715-047.dmg
for pair in "$OS:$OS_SHA" "$SYSOS:$SYSOS_SHA" "$APPS:$APPS_SHA"; do
  dmg=${pair%%:*}
  want=${pair#*:}
  if [ ! -f "$dmg" ] || ! check_hash "$dmg" "$want"; then
    name=${dmg##*/}
    echo "extracting $name from IPSW"
    unzip -p "$IPSW" "$name" > "$dmg"
  fi
  need_hash "$dmg" "$want"
done
echo 'IPSW filesystem DMGs verified'

# Recover the Mac-generated shader data and proven wallpaper payloads from an
# existing image. Pin every shader file; wallpaper files are content-checked
# by tools/fresh-image-ui-fixes.py later.
need_source=0
while read -r want name; do
  [ -n "$name" ] || continue
  if [ ! -f "$STAGE/$name" ] || ! check_hash "$STAGE/$name" "$want"; then
    need_source=1
  fi
done <<< "$STAGE_HASHES"
[ "$(png_heic_count)" -ge 16 ] || need_source=1
if [ "$need_source" -eq 1 ]; then
  [ -f "$SOURCE_IMAGE" ] || die "missing shader/wallpaper source image: $SOURCE_IMAGE"
  echo "recovering shader data and wallpaper from $SOURCE_IMAGE"
  rm -rf -- "$WALL"
  mkdir -p "$WALL" "$SRC_MNT"
  if ! mount -t hfsplus -o loop,ro,force,uid=0,gid=0,nls=utf8 "$SOURCE_IMAGE" "$SRC_MNT" 2>"$WORK/source-mount.err"; then
    echo 'read-only HFS mount failed; retrying rw on the source (journal metadata only)'
    mount -t hfsplus -o loop,rw,force,uid=0,gid=0,nls=utf8 "$SOURCE_IMAGE" "$SRC_MNT"
  fi
  bundle="$SRC_MNT/System/Library/Extensions/SweetMetal.bundle"
  for name in fcmap.txt shaders.smsp variants.txt; do
    [ -f "$bundle/$name" ] || die "missing $bundle/$name in the source image"
    install -D -m 644 "$bundle/$name" "$STAGE/$name"
  done
  [ -d "$SRC_MNT/Library/Wallpaper" ] || die "missing $SRC_MNT/Library/Wallpaper"
  # HFS+ cannot preserve every Mac metadata bit here; cp -a fails with
  # "No data available". Only file contents and relative names are needed.
  cp -R --no-preserve=all "$SRC_MNT/Library/Wallpaper/." "$WALL/"
  sync
  umount "$SRC_MNT" || umount -l "$SRC_MNT" || true
  rmdir "$SRC_MNT" 2>/dev/null || true
fi
(cd "$STAGE" && printf '%s\n' "$STAGE_HASHES" | sha256sum -c - >/dev/null) || die 'shader data hash mismatch'
png_wallpapers=$(png_heic_count)
[ "$png_wallpapers" -ge 16 ] || die "expected at least 16 PNG-as-HEIC wallpapers, found $png_wallpapers"
echo "shader data and $png_wallpapers wallpaper payloads recovered"

echo 'creating stock 16 GiB HFS+ base from the pinned IPSW'
"$REPO/tools/mkiosroot.sh" "$OS" "$SYSOS" "$APPS" "$OUT" 16G > "$WORK/mkiosroot.log" 2>&1 || {
  tail -80 "$WORK/mkiosroot.log" >&2
  die 'mkiosroot.sh failed'
}
base_sha=$(hash "$OUT")
echo "stock base: $base_sha"

echo 'installing latest artifacts and launchd jobs'
BASE_SHA="$base_sha" IMG="$OUT" MNT="$BUILD_MNT" REPO="$REPO" STAGE="$STAGE" \
  TOOLS="$TOOLS" MESA="$MESA" BLOBS="$BLOBS" KERNEL="$KERNEL" \
  OPALINE_IPA="$OPALINE_IPA" "$REPO/tools/build-fresh-image.sh" > "$WORK/install.log" 2>&1 || {
  tail -120 "$WORK/install.log" >&2
  die 'build-fresh-image.sh failed'
}
tail -5 "$WORK/install.log"

echo 'applying UI, migration, gputest and ownership repairs'
mkdir -p "$FINAL_MNT"
mount -t hfsplus -o loop,rw,force,uid=0,gid=0,nls=utf8 "$OUT" "$FINAL_MNT"
python3 "$REPO/tools/fresh-image-ui-fixes.py" "$FINAL_MNT" --wallpaper-source "$WALL_SOURCE_ROOT" > "$WORK/ui-fixes.log" 2>&1 || {
  cat "$WORK/ui-fixes.log" >&2
  die 'fresh-image-ui-fixes.py failed'
}
grep -q 'missing_targets=0' "$WORK/ui-fixes.log" || die 'wallpaper targets missing'
PYTHONPATH="$REPO/tools" python3 "$REPO/tools/fix-fresh-image-migration.py" "$FINAL_MNT"
python3 "$REPO/tools/s21-gputest-job.py" "$FINAL_MNT"
"$REPO/tools/fix-fresh-image-ownership.sh" "$FINAL_MNT"

python3 - "$FINAL_MNT" <<'PY'
import glob, os, plistlib, sys
root = sys.argv[1]
key = '/System/Library/LaunchDaemons/com.s21.gputest.plist'
job = {'Label': 'com.s21.gputest', 'ProgramArguments': ['/usr/bin/s21-gpu-test'],
       'RunAtLoad': True, 'KeepAlive': False,
       'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console',
       'EnvironmentVariables': {'S21_GPU_TEST_PAINT_ONCE': '1'}}
with open(os.path.join(root, 'System/Library/xpc/launchd.plist'), 'rb') as f:
    cached = plistlib.load(f)
if cached.get('LaunchDaemons', {}).get(key) != job:
    raise SystemExit('STOP: cached com.s21.gputest job wrong')
with open(os.path.join(root, key.lstrip('/')), 'rb') as f:
    if plistlib.load(f) != job:
        raise SystemExit('STOP: standalone com.s21.gputest plist wrong')
for rel in ('System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist',
            'System/Library/LaunchDaemons/com.apple.jetsamproperties.D221.plist'):
    with open(os.path.join(root, rel), 'rb') as f:
        pl = plistlib.load(f)
    if pl['Version4']['Daemon']['Override'].get('com.s21.gputest', {}).get('ActiveHardMemoryLimit') != 128:
        raise SystemExit('STOP: jetsam override missing in ' + rel)
feature = os.path.join(root, 'private/var/preferences/FeatureFlags/Domain/SpringBoard.plist')
with open(feature, 'rb') as f:
    if plistlib.load(f).get('Jindo') != {'Enabled': False}:
        raise SystemExit('STOP: Jindo flag wrong')
owner = {}
for rel in ('private/var', 'private/var/mobile', 'private/var/mobile/Library/SpringBoard',
            'private/var/preferences/FeatureFlags/Domain'):
    st = os.stat(os.path.join(root, rel))
    owner[rel] = f'{st.st_uid}:{st.st_gid}'
expected = {'private/var': '0:0', 'private/var/mobile': '501:501',
            'private/var/mobile/Library/SpringBoard': '501:501',
            'private/var/preferences/FeatureFlags/Domain': '0:0'}
if owner != expected:
    raise SystemExit('STOP: ownership wrong: ' + repr(owner))
hits = 0
for path in glob.glob(os.path.join(root, 'private/var/mobile/Library/Preferences/ByHost/com.apple.migration.*.plist')):
    with open(path, 'rb') as f:
        if plistlib.load(f).get('LastSystemVersion') == '20H392':
            hits += 1
if hits < 2:
    raise SystemExit('STOP: ByHost migration preference missing')
print('FINAL_IMAGE_STATE_PASS gputest=jetsam+paint-once jindo=off owner=ok migration=ok')
PY

sync
umount "$FINAL_MNT" || umount -l "$FINAL_MNT" || true
rmdir "$FINAL_MNT" 2>/dev/null || true
fsck.hfsplus -fy "$OUT" > "$WORK/fsck-final.log" 2>&1 || {
  tail -80 "$WORK/fsck-final.log" >&2
  die 'final fsck failed'
}
grep -q 'appears to be OK' "$WORK/fsck-final.log" || die 'final fsck did not report OK'
sha256sum "$OUT" > "$OUT.sha256"
final_sha=$(awk '{print $1}' "$OUT.sha256")
stat -c '%n %s %U:%G %a' "$OUT"
cat "$OUT.sha256"
echo "LATEST_S21_IMAGE_READY sha=$final_sha base=$base_sha kernel=$(hash "$KERNEL") gpu_job=com.s21.gputest"
