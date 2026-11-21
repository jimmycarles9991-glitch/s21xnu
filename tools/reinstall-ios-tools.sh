#!/bin/bash
# Install ios-tools.zip (SweetMetal.bundle + mali test tools) into
# build/iosroot-20H392-full.img, overwriting the old tools.
#
#   ./reinstall-ios-tools.sh          # prompts for sudo when needed
#   ./reinstall-ios-tools.sh fix      # same thing; repairs an interrupted run
#
# Defaults now match the 2026-11-19 CI ios-tools artifact (SweetMetal 363216 B,
# sha256 8e8e45d3...214). Env overrides work through sudo:
#   EXPECT_SIZE=... EXPECT_SHA=... ZIP=... FORCE=1 ./reinstall-ios-tools.sh
set -euo pipefail

# Set defaults before the sudo re-exec: sudo resets the environment, so the
# overrides have to be passed through explicitly (plain `FORCE=1 ./script`
# used to be silently dropped when this re-executed as root).
ZIP=${ZIP:-/home/tom/Downloads/ios-tools.zip}
EXPECT_SIZE=${EXPECT_SIZE:-363216}
EXPECT_SHA=${EXPECT_SHA:-8e8e45d34e300dfd42137bc6b6f59e5778d7a79de228b90a5abb891bebbea214}
FORCE=${FORCE:-0}
OUT=${OUT:-}

[ "$(id -u)" -eq 0 ] || exec sudo env \
  "ZIP=$ZIP" "EXPECT_SIZE=$EXPECT_SIZE" "EXPECT_SHA=$EXPECT_SHA" "FORCE=$FORCE" "OUT=$OUT" \
  "$0" "$@"

cd "$(dirname "$0")/.."
OUT=${OUT:-$HOME/ios-sweet/build/iosroot-20H392-full.img}
MNT=/mnt/iosroot-build
TOOLS="malitest malitri malicomp malifb malinop malidisp mtltest"
# New Mesa/Panfrost files are optional until the iOS cross-build lands; the
# script still accepts older ios-tools zips without them.
OPTIONAL_TOOLS="s21-gl-clear"
OPTIONAL_LIBS="libs21sweetdrm.dylib"
DATA="shaders.smsp fcmap.txt variants.txt"
EXT=System/Library/Extensions

# Clean up root-owned logs from earlier runs, then use /tmp for new ones.
rm -f imgfsck-pre.log imgfsck.log
LOGPRE=$(mktemp /tmp/imgfsck-pre.XXXXXX)
LOGPOST=$(mktemp /tmp/imgfsck.XXXXXX)

# 1. Validate the ZIP before touching the image (the old script deleted the
#    bundle first and then STOPped, leaving the image without SweetMetal).
[ -s "$ZIP" ] || { echo "STOP: $ZIP missing or empty"; exit 1; }
SZ=$(unzip -l "$ZIP" | awk '$NF ~ /SweetMetal\.bundle\/SweetMetal$/ {print $1}')
[ -n "$SZ" ] || { echo "STOP: no SweetMetal.bundle/SweetMetal in $ZIP"; exit 1; }
echo "zip: SweetMetal binary $SZ B (expected $EXPECT_SIZE)"
if [ "$SZ" != "$EXPECT_SIZE" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "STOP: size differs. For a newer zip rerun with EXPECT_SIZE=$SZ (or FORCE=1)"; exit 1
fi
KEEP=$(mktemp -d)
unzip -o -q "$ZIP" -d "$KEEP"
[ -f "$KEEP/SweetMetal.bundle/SweetMetal" ] || { echo "STOP: unexpected zip layout"; exit 1; }
GOT_SHA=$(sha256sum "$KEEP/SweetMetal.bundle/SweetMetal" | awk '{print $1}')
echo "zip: SweetMetal sha256 $GOT_SHA"
if [ "$GOT_SHA" != "$EXPECT_SHA" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "STOP: sha256 differs; rerun with EXPECT_SHA=$GOT_SHA (or FORCE=1)"; exit 1
fi
for t in $TOOLS; do
  [ -f "$KEEP/$t" ] || { echo "STOP: $t missing in zip"; exit 1; }
done

# The data files are generated separately and are normally not in the zip;
# recover them from the image (or the local build tree) later.

# 2. fsck, mount, install.
fsck.hfsplus -fy "$OUT" > "$LOGPRE" 2>&1 || true
mkdir -p "$MNT"
mount -t hfsplus -o loop,force,rw "$OUT" "$MNT"
trap 'sync; umount "$MNT" 2>/dev/null || umount -l "$MNT"' EXIT

B="$MNT/$EXT/SweetMetal.bundle"
SAVE=$(mktemp -d)
for f in $DATA; do
  [ -f "$B/$f" ] && cp -a "$B/$f" "$SAVE/"
done
rm -rf "$B"
cp -a "$KEEP/SweetMetal.bundle" "$B"
for f in $DATA; do
  if [ ! -f "$B/$f" ] && [ -f "$SAVE/$f" ]; then
    cp -a "$SAVE/$f" "$B/$f"
    echo "kept $f from old bundle"
  fi
done
# Repair artifacts of the earlier failed run: restore generated shader/variant
# files from the local build tree when the image no longer has them.
[ -f "$B/shaders.smsp" ] || [ ! -f /home/tom/s21xnu-push/b/shaders.smsp ] || \
  { cp /home/tom/s21xnu-push/b/shaders.smsp "$B/shaders.smsp"; echo "restored shaders.smsp"; }
[ -f "$B/variants.txt" ] || [ ! -f /home/tom/s21xnu-push/build/variants.txt ] || \
  { cp /home/tom/s21xnu-push/build/variants.txt "$B/variants.txt"; echo "restored variants.txt"; }

install -m755 "$KEEP/malitest" "$KEEP/malitri" "$KEEP/malicomp" \
    "$KEEP/malifb" "$KEEP/malinop" "$KEEP/malidisp" "$KEEP/mtltest" \
    "$MNT/usr/bin/"

for t in $OPTIONAL_TOOLS; do
  if [ -f "$KEEP/$t" ]; then
    install -m755 "$KEEP/$t" "$MNT/usr/bin/"
    echo "installed optional tool $t"
  else
    echo "optional tool $t not in zip; skipped"
  fi
done
for l in $OPTIONAL_LIBS; do
  if [ -f "$KEEP/$l" ]; then
    install -m644 "$KEEP/$l" "$MNT/usr/lib/"
    echo "installed optional library $l"
  else
    echo "optional library $l not in zip; skipped"
  fi
done

echo "== GPU files in image =="
ls -la "$B" "$MNT/usr/bin/malitest"
echo "== in-image SweetMetal sha256 (expect $EXPECT_SHA) =="
sha256sum "$B/SweetMetal"
for f in $DATA; do
  [ -f "$B/$f" ] && echo "  $f: present" || echo "  WARNING: $f MISSING"
done

sync
umount "$MNT"
trap - EXIT

fsck.hfsplus -fy "$OUT" > "$LOGPOST" 2>&1 && tail -3 "$LOGPOST"
sha256sum "$OUT" > "$OUT.sha256"
img2simg "$OUT" "${OUT%.img}.simg"
sha256sum "${OUT%.img}.simg" >> "$OUT.sha256"
echo "== done =="
ls -la "$OUT" "${OUT%.img}.simg" "$OUT.sha256"
cat "$OUT.sha256"
