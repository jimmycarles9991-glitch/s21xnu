#!/bin/bash
# Build a COMPLETELY FRESH S21 root image from the stock IPSW base root.
#
# This script takes the freshly built base image from tools/mkiosroot.sh
# (stock iOS 16.7.16 OS + SystemOS + AppOS from the pinned IPSW) and installs:
#   - latest ios-tools userland (run 45936492665)
#   - latest linux-mesa-ios V33 artifacts (CI run 45822449444)
#   - SEMCO-only BCM4375 Wi-Fi/BT firmware from /fw
#   - BlueTool stub, H4 transport, early clock restore, EAGL shim via s21-root-fixes.py
#   - com.s21.gputest LaunchDaemon (paint once, jetsam override)
#   - Opaline trial app
#   - Mac-generated SweetMetal shader data from the proven image
#   - host epoch for the malinop clock restore
#
# It NEVER boots, kexecs, flashes, repartitions, SSHes or changes the active
# device image. It only writes the new host image and its .sha256.
#
# Run: pkexec /home/tom/Downloads/build-fresh-ipsw-image.sh
# Prints: FRESH_IPSW_IMAGE_READY sha=...
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
[ "$(id -u)" -eq 0 ] || { echo 'STOP: must run as root via pkexec' >&2; exit 1; }

IMG=${IMG:-/mnt/7FEF-EA6C/ios/build/iosroot-s21-fresh-v1.img}
MNT=${MNT:-/mnt/iosroot-fresh-build}
REPO=${REPO:-/home/tom/s21xnu-linux-mesa}
TOOLS=${TOOLS:-/home/tom/Downloads/ios-tools-wifi-bt}
MESA=${MESA:-/home/tom/Downloads/ios-mesa-g78-v33}
BLOBS=${BLOBS:-/home/tom/s21}
STAGE=${STAGE:-/home/tom/s21-fresh-build/stage}
KERNEL=${KERNEL:-/home/tom/Downloads/kernel-wifi-bt/kernel.development.sweet}
OPALINE_IPA=${OPALINE_IPA:-/home/tom/Downloads/s21-trial-ipas/Opaline_1.12.0.ipa}

SIZE=17179869184
BASE_SHA=${BASE_SHA:-4ad2e35a51e4e70221325799b0b88c8077cf23dc933c58175bcfe3654a9f86bb}

# --- Userland hashes (ios-tools-wifi-bt, CI 45936492665) ---
TOOL_HASHES=(
  'e193fb5674fdbfc120cd4d381ec5154de658dda151e85a0cf44844cce59deb5a bluetool_stub'
  '6df152173a17b405a4df41d3216c8416137b30afd5816719a5a12c6c4f9fd3fc malicomp'
  '08f84862fe78e007764a746624f6875bcb86124af9eb399d4a17c7c962e41d17 malidisp'
  '807d21d841838aa354b9473b3683ceb434fd9cda83d6f04bb17d0db2fc599393 malifb'
  '6c2f05456e71de9d684e5acfeef265444fb2b108ace37bc7f38a25b41c3fdca5 malinop'
  '9e71c09fe371ffdef5fd4e6ed654c235ca6eea435a76725fbf53fa1f4c083005 malitest'
  'cf09d598d59cc7d836148b292632acbf150c7d32f0743c1052193c43a87f7613 malitri'
  'b892d4ebfd9c15b2a5a4cfe531d595da90d8ece1fbdd3d917db2d88d402d03e5 mtltest'
  'e96eba89c6fb15570bdfc66a9a78e0f1c5e91475abbb65ad858dc761237e8ac5 vtprobe'
)

# --- Mesa V33 hashes (CI 45822449444, artifact 15120455408) ---
MESA_HASHES=(
  '72ae3d6e5a174897b11487439310f71c5ddbcffc63f84e3c3091530fc44357ed libEGL.1.dylib'
  '273bf66741266f47af5274a4268d18e57464a0ae936c07b441accafec2600b98 libEGL.dylib'
  '8abca4e119e0681c6a1c9bb78b423d2b52618d107420ae0ebbf9bf2a4157c3c0 libGLESv2.2.dylib'
  '42294d28442171ee9eb64f128d4a3e2d6c7fefd5a59dc1b7b7e58080a472d6b6 libGLESv2.dylib'
  'ee914f4008aec912d654564e6332bc2d07b805c99bcf0185ca7f16b12edb9b49 libgallium-26.2.4.dylib'
  '01ab5b6bde5273ca09fc609a1d3be01839658d7caefd396276f3257ad0ffab55 libs21drm.dylib'
  'b8b79eb8a2bbaf06427d92b11840e5a37d1157b4c4b280b72a7fc15e316d6b0a libs21eagl.dylib'
  'a00d00ff4584e7b637d2c13619244895c3debae8fded33ffb9063ef7c6f3b6ea s21-gpu-test'
)

# --- SEMCO blobs ---
STA_SHA=1676f46ce56b96f58dc70de08beaab4ab3362ee6dd751465a8d6a0023c3c54ad
CLM_SHA=d7b8028059cf481cc796fa6c2b582d0d1e661d5ac44455da72aefcd052d9f3b4
NVRAM_SHA=d9c419ecba8f7905252449c67680d508e9286c21ea036fbab9ea9b6d4c848a59
HCD_SHA=ecfcf69851663f782f3ef00aae921bbfb41095938f8f151da4ea0da2c6f3779f

SWEET_AUDIO_SHA=9f4291801b362260728db0413d7c0dd115f172845c215138aa9b7aca392503b7
SWEET_METAL_SHA=fe506c38956e947cdec80157bc64407684eb2cb963a59332e72c00129c06f2f2
OPALINE_SHA=a254306109b09bcf4d3ba4de80592f7a89b3932e7484f41f570c4a81913141d1
OPALINE_EXE_SHA=29205fc7fd18b44959fbe2e1aaf7a6baeb864368d3ceb99cdb87759ef50ec941
KERNEL_SHA=cccee161d2d194c903770d236b3d5e2e0cd4508f151e910c95bdd4f73215b783

die() { echo "STOP: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
need_file() { [ -f "$1" ] && [ -s "$1" ] || die "missing or empty: $1"; }
need_hash() { [ "$(hash "$1")" = "$2" ] || die "hash mismatch: $1"; }

# --- Verify base image / final output preconditions ---
need_file "$IMG"
[ "$(stat -c %s "$IMG")" -eq "$SIZE" ] || die "base image size mismatch"
need_hash "$IMG" "$BASE_SHA"
[ ! -e "$MNT" ] || die "mountpoint $MNT already exists; clean it up"
command -v unzip >/dev/null 2>&1 || die 'unzip required'
for f in "$REPO/tools/sweet-root-tweaks.py" "$REPO/tools/s21-setup-skip.py" "$REPO/tools/s21-root-fixes.py" "$REPO/tools/s21-gputest-job.py"; do
  [ -f "$f" ] || die "missing $f"
done

echo "base verified: $BASE_SHA"

# --- Verify all input artifacts before writing anything ---
for entry in "${TOOL_HASHES[@]}"; do
  expected=${entry%% *}; name=${entry#* }
  need_file "$TOOLS/$name"; need_hash "$TOOLS/$name" "$expected"
done
for entry in "${MESA_HASHES[@]}"; do
  expected=${entry%% *}; name=${entry#* }
  need_file "$MESA/$name"; need_hash "$MESA/$name" "$expected"
done
for f in SweetAudio.driver/SweetAudio SweetMetal.bundle/SweetMetal \
         SweetAudio.driver/Info.plist SweetMetal.bundle/Info.plist \
         SweetAudio.driver/_CodeSignature/CodeResources SweetMetal.bundle/_CodeSignature/CodeResources; do
  need_file "$TOOLS/$f"
done
need_hash "$TOOLS/SweetAudio.driver/SweetAudio" "$SWEET_AUDIO_SHA"
need_hash "$TOOLS/SweetMetal.bundle/SweetMetal" "$SWEET_METAL_SHA"
need_hash "$KERNEL" "$KERNEL_SHA"
need_hash "$OPALINE_IPA" "$OPALINE_SHA"

for f in fcmap.txt shaders.smsp variants.txt; do need_file "$STAGE/$f"; done
(cd "$STAGE" && printf '%s  %s\n' 55da3071197242625de090599c17ef6013cf474f02629fca2ba24db2ada6e05d fcmap.txt 4ee25214d00aac12e6361cfe284f6105080e39dd90ff26183bd4211b34272a69 shaders.smsp 0c06367e267e268896715120c116b6a5500303f63cbe70d364d8321cb77e0d81 variants.txt | sha256sum -c - >/dev/null) || die 'shader data hash mismatch'

STA_SRC="$BLOBS/fw/bcmdhd_sta.bin_b1"
CLM_SRC="$BLOBS/fw/bcmdhd_clm.blob"
NVRAM_SRC="$BLOBS/fw/nvram.txt_CS01_semco_b1"
HCD_SRC="$BLOBS/bcm4375B1_semco_sem.hcd"
need_hash "$STA_SRC" "$STA_SHA"
need_hash "$CLM_SRC" "$CLM_SHA"
need_hash "$NVRAM_SRC" "$NVRAM_SHA"
need_hash "$HCD_SRC" "$HCD_SHA"
echo 'all input hashes verified'

# --- Mount the fresh image rw ---
fsck.hfsplus -fy "$IMG" > /tmp/opencode/hfs-fresh-pre.log 2>&1 || true
mkdir -p "$MNT"
mount -t hfsplus -o loop,force,rw "$IMG" "$MNT"
mounted=0
cleanup() {
  if [ "$mounted" -eq 1 ]; then
    sync || true
    umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
  fi
}
trap cleanup EXIT
mounted=1

# --- Confirm this really is the fresh stock root ---
[ ! -e "$MNT/usr/bin/malinop" ] || die 'malinop already present; not a fresh root'
[ ! -e "$MNT/Applications/Opaline.app" ] || die 'Opaline already present; not a fresh root'
[ -f "$MNT/System/Library/xpc/launchd.plist" ] || die 'cached launchd.plist missing'
DSC=$MNT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.03
need_hash "$DSC" 511377c04e3054b905fcfb83bb2d1e6ffdb3b838ae21969fd1962dbfcab9880b
[ -e "$MNT/usr/share/zoneinfo.default/Europe/London" ] || die 'Europe/London missing from zoneinfo.default'
echo 'fresh stock root confirmed'

# --- Base root tweaks ---
python3 "$REPO/tools/sweet-root-tweaks.py" "$MNT"
python3 "$REPO/tools/s21-setup-skip.py" "$MNT"

# --- Install userland tools to /usr/bin ---
for entry in "${TOOL_HASHES[@]}"; do
  name=${entry#* }
  install -m 755 "$TOOLS/$name" "$MNT/usr/bin/$name"
  chown root:wheel "$MNT/usr/bin/$name"
done
# vtprobe is also looked up at /usr/local/bin by SweetMetal.m
mkdir -p "$MNT/usr/local/bin"
install -m 755 "$TOOLS/vtprobe" "$MNT/usr/local/bin/vtprobe"
chown root:wheel "$MNT/usr/local/bin/vtprobe"

# --- Install Mesa V33 artifacts to /usr/bin ---
for entry in "${MESA_HASHES[@]}"; do
  name=${entry#* }
  install -m 755 "$MESA/$name" "$MNT/usr/bin/$name"
  chown root:wheel "$MNT/usr/bin/$name"
done

# --- Install SweetMetal bundle (latest binary + proven shader data) ---
rm -rf "$MNT/System/Library/Extensions/SweetMetal.bundle"
cp -a "$TOOLS/SweetMetal.bundle" "$MNT/System/Library/Extensions/SweetMetal.bundle"
for f in fcmap.txt shaders.smsp variants.txt; do
  install -m 644 "$STAGE/$f" "$MNT/System/Library/Extensions/SweetMetal.bundle/$f"
done
chown -R root:wheel "$MNT/System/Library/Extensions/SweetMetal.bundle"
need_hash "$MNT/System/Library/Extensions/SweetMetal.bundle/SweetMetal" "$SWEET_METAL_SHA"

# --- Install SweetAudio driver ---
rm -rf "$MNT/System/Library/Audio/Plug-Ins/HAL/SweetAudio.driver"
cp -a "$TOOLS/SweetAudio.driver" "$MNT/System/Library/Audio/Plug-Ins/HAL/SweetAudio.driver"
chown -R root:wheel "$MNT/System/Library/Audio/Plug-Ins/HAL/SweetAudio.driver"
need_hash "$MNT/System/Library/Audio/Plug-Ins/HAL/SweetAudio.driver/SweetAudio" "$SWEET_AUDIO_SHA"
echo 'userland, Mesa, SweetMetal and SweetAudio installed'

# --- Install SEMCO-only Wi-Fi/BT blobs to /fw ---
mkdir -p "$MNT/fw"
install -m 644 "$STA_SRC"   "$MNT/fw/bcm4375-sta.bin"
install -m 644 "$CLM_SRC"   "$MNT/fw/bcm4375-clm.blob"
install -m 644 "$NVRAM_SRC" "$MNT/fw/bcm4375-nvram-semco.txt"
install -m 644 "$HCD_SRC"   "$MNT/fw/bcm4375B1-semco.hcd"
chown root:wheel "$MNT/fw" "$MNT/fw"/*
need_hash "$MNT/fw/bcm4375-sta.bin" "$STA_SHA"
need_hash "$MNT/fw/bcm4375-clm.blob" "$CLM_SHA"
need_hash "$MNT/fw/bcm4375-nvram-semco.txt" "$NVRAM_SHA"
need_hash "$MNT/fw/bcm4375B1-semco.hcd" "$HCD_SHA"
[ ! -e "$MNT/fw/bcm4375-nvram-murata.txt" ] || die 'unexpected Murata NVRAM'
[ ! -e "$MNT/fw/bcm4375B1-murata.hcd" ] || die 'unexpected Murata HCD'

# --- Host epoch for malinop clock restore ---
mkdir -p "$MNT/usr/local/etc"
date +%s > "$MNT/usr/local/etc/sweet_epoch"
chmod 644 "$MNT/usr/local/etc/sweet_epoch"
chown root:wheel "$MNT/usr/local/etc/sweet_epoch"

# --- Root fixes: Bluetooth stub + H4 transport, EAGL shim, clock job, wifid ---
S21_BT=1 S21_EAGL_SHIM=1 python3 "$REPO/tools/s21-root-fixes.py" "$MNT"

# --- GPU self-test: paint one frame, then let backboardd present the UI ---
python3 "$REPO/tools/s21-gputest-job.py" "$MNT"

# --- Install Opaline trial app ---
tmpdir=$(mktemp -d /tmp/opencode/fresh-opaline.XXXXXX)
trap 'cleanup; rm -rf "$tmpdir"' EXIT
unzip -q "$OPALINE_IPA" -d "$tmpdir"
[ -d "$tmpdir/Payload/Opaline.app" ] || die 'Opaline.app missing from IPA'
cp -a "$tmpdir/Payload/Opaline.app" "$MNT/Applications/Opaline.app"
chown -R root:wheel "$MNT/Applications/Opaline.app"
need_hash "$MNT/Applications/Opaline.app/Opaline" "$OPALINE_EXE_SHA"

# --- Verify launchd final state ---
python3 - "$MNT" <<'PY'
import glob, os, plistlib, sys
root = sys.argv[1]
job = {'Label': 'com.apple.BlueTool', 'ProgramArguments': ['/usr/bin/bluetool_stub'],
       'MachServices': {'com.apple.BlueTool': True}, 'UserName': 'mobile',
       'StandardOutPath': '/dev/console', 'StandardErrorPath': '/dev/console'}
with open(os.path.join(root, 'System/Library/xpc/launchd.plist'), 'rb') as f:
    pl = plistlib.load(f)
key = '/System/Library/LaunchDaemons/com.apple.BlueTool.plist'
if pl.get('LaunchDaemons', {}).get(key) != job:
    raise SystemExit('STOP: cached BlueTool job is not the stub')
if pl.get('LaunchDaemons', {}).get('/System/Library/LaunchDaemons/000.s21.clockrestore.plist', {}).get('ProgramArguments') != ['/usr/bin/malinop']:
    raise SystemExit('STOP: clock restore job missing')
bb = [j for j in pl.get('LaunchDaemons', {}).values()
      if isinstance(j, dict) and j.get('Label') == 'com.apple.backboardd']
if not bb or bb[0].get('EnvironmentVariables', {}).get('DYLD_INSERT_LIBRARIES') != '/usr/bin/libs21eagl.dylib':
    raise SystemExit('STOP: backboardd EAGL shim missing')
bts = [j for j in pl.get('LaunchDaemons', {}).values()
       if isinstance(j, dict) and j.get('Label') == 'com.apple.bluetoothd']
if not bts or bts[0].get('EnvironmentVariables', {}).get('BT_HCI_TRANSPORT') != 'H4:POSIX:com.apple.uart.bluetooth':
    raise SystemExit('STOP: bluetoothd H4 transport missing')
gputest_key = '/System/Library/LaunchDaemons/com.s21.gputest.plist'
gputest_job = pl.get('LaunchDaemons', {}).get(gputest_key)
if not gputest_job or gputest_job.get('ProgramArguments') != ['/usr/bin/s21-gpu-test']:
    raise SystemExit('STOP: com.s21.gputest cached job missing')
if gputest_job.get('EnvironmentVariables', {}).get('S21_GPU_TEST_PAINT_ONCE') != '1':
    raise SystemExit('STOP: com.s21.gputest would paint the framebuffer forever')
for p in glob.glob(os.path.join(root, 'System/Library/LaunchDaemons', '*BlueTool*.plist')):
    with open(p, 'rb') as f:
        if plistlib.load(f) != job:
            raise SystemExit('STOP: standalone BlueTool job wrong: ' + p)
print('launchd final state: PASS')
PY

# --- Final in-image hash spot-checks ---
need_hash "$MNT/usr/bin/bluetool_stub" "${TOOL_HASHES[0]%% *}"
need_hash "$MNT/usr/bin/libs21eagl.dylib" b8b79eb8a2bbaf06427d92b11840e5a37d1157b4c4b280b72a7fc15e316d6b0a
need_hash "$MNT/usr/bin/libs21drm.dylib" 01ab5b6bde5273ca09fc609a1d3be01839658d7caefd396276f3257ad0ffab55
need_hash "$MNT/usr/bin/malinop" 6c2f05456e71de9d684e5acfeef265444fb2b108ace37bc7f38a25b41c3fdca5

avail=$(df -Pk "$MNT" | awk 'NR==2 {print $4}')
[ "${avail:-0}" -gt 200000 ] || die "too little free space after install: ${avail:-0} KiB"

sync
umount "$MNT"
mounted=0
rm -rf "$tmpdir"
trap - EXIT

fsck.hfsplus -fy "$IMG" > /tmp/opencode/hfs-fresh-post.log 2>&1 || die 'post fsck failed'
grep -q 'appears to be OK' /tmp/opencode/hfs-fresh-post.log || die 'post fsck not OK'

sha256sum "$IMG" > "$IMG.sha256"
final_sha=$(awk '{print $1}' "$IMG.sha256")
[ "$final_sha" != "$BASE_SHA" ] || die 'final image identical to base'
stat -c '%n %s %U:%G %a' "$IMG"
cat "$IMG.sha256"
echo "FRESH_IPSW_IMAGE_READY sha=$final_sha base=$BASE_SHA kernel=$KERNEL_SHA mesa=V33 tools=wifi-bt wifi=SEMCO bt=H4 gpu_job=paint-once apps=Opaline"
