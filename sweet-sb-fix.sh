#!/bin/bash
# sweet-sb-fix.sh: root-side SpringBoard fixes, applied to a COPY of the image.
#   usage: bash sweet-sb-fix.sh ~/ios-sweet/iosroot-trim.img [OUT.img]
# Never touches the source image or the phone. Pushing OUT.img is your usual step (push_tw7.sh style).
#
# What it writes (all under /private/var/mobile/Library/Preferences, owner mobile 501:501):
#   com.apple.assistant.support.plist  "Assistant Enabled" = false
#       -> y2s's first fix for the SpringBoard abort loop (Siri UI -> MPS/Metal -> MTLReportFailure).
#   com.apple.purplebuddy.plist        SetupDone / SetupFinishedAllSteps = true
#       -> try to skip the first-boot "hello" setup. May not be enough alone: y2s also forces
#          SBLockdownManager activation (needs the injected dylib).
#   com.apple.springboard.plist        SBShowBatteryPercentage = true (harmless; merged only if absent)
set -euo pipefail

SRC="${1:?usage: $0 SRC.img [OUT.img]}"
OUT="${2:-${SRC%.img}-sbfix.img}"
MNT=/mnt/iosfix
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

[ -f "$SRC" ] || { echo "no such image: $SRC"; exit 1; }
[ "$SRC" != "$OUT" ] || { echo "OUT must differ from SRC"; exit 1; }

echo "[1/5] copying $SRC -> $OUT"
cp --reflink=auto "$SRC" "$OUT"

echo "[2/5] mounting rw (journaled HFS+, forced)"
sudo mkdir -p "$MNT"
sudo mount -t hfsplus -o loop,rw,force "$OUT" "$MNT"
trap 'sudo umount "$MNT" 2>/dev/null || true' EXIT

P="$MNT/private/var/mobile/Library/Preferences"
echo "[3/5] writing plists into $P"
sudo mkdir -p "$P"

write_plist() {  # name, body
	sudo tee "$P/$1" >/dev/null <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
$2
</dict>
</plist>
EOF
	sudo chown 501:501 "$P/$1"
	sudo chmod 644 "$P/$1"
	echo "    wrote $1"
}

write_plist com.apple.assistant.support.plist \
'	<key>Assistant Enabled</key>
	<false/>
	<key>Dictation Enabled</key>
	<false/>'

write_plist com.apple.purplebuddy.plist \
'	<key>SetupDone</key>
	<true/>
	<key>SetupFinishedAllSteps</key>
	<true/>
	<key>SetupState</key>
	<string></string>'

# Apply the complete comparison-port setup/activation state while preserving
# the additional purplebuddy keys written above.
sudo python3 "$SCRIPT_DIR/tools/s21-setup-skip.py" "$MNT"

if [ ! -e "$P/com.apple.springboard.plist" ]; then
	write_plist com.apple.springboard.plist \
'	<key>SBShowBatteryPercentage</key>
	<true/>'
else
	echo "    com.apple.springboard.plist already exists, left alone"
fi
sudo chown 501:501 "$MNT/private/var/mobile/Library" "$P" 2>/dev/null || true

echo "[4/5] free space and listing"
df -h "$MNT" | tail -1
ls -l "$P"
sync
sudo umount "$MNT"
trap - EXIT

echo "[5/5] fsck + hash"
sudo fsck.hfsplus -fn "$OUT" | tail -3
sha256sum "$OUT"
echo
echo "Done: $OUT"
echo "Next: write it to the phone the same way as tw7 (dd conv=notrunc into iosroot.img), then boot ONCE and keep the console video running."
