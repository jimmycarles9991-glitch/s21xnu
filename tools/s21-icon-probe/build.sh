#!/bin/bash
# Build a tiny ad-hoc signed iOS app that paints exact RGBA test images.
# Intended to run on the macOS linux-mesa-ios builder.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT_DIR:-$ROOT/out/s21-icon-probe}
APP="$OUT/Payload/S21IconProbe.app"

command -v xcrun >/dev/null || { echo "ERROR: xcrun is required" >&2; exit 1; }
command -v codesign >/dev/null || { echo "ERROR: codesign is required" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$APP"

python3 "$ROOT/tools/s21-icon-probe/make_icons.py" "$APP"
cp "$ROOT/tools/s21-icon-probe/Info.plist" "$APP/Info.plist"

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun -sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 \
    -O2 -fobjc-arc -std=gnu11 -framework UIKit -framework Foundation \
    "$ROOT/tools/s21-icon-probe/main.m" -o "$APP/S21IconProbe"
chmod 755 "$APP/S21IconProbe"

codesign --force --sign - --timestamp=none "$APP"
codesign --verify --strict "$APP"

(cd "$OUT" && zip -qry S21IconProbe.ipa Payload)
(cd "$OUT" && shasum -a 256 S21IconProbe.ipa > SHA256SUMS)
echo "S21IconProbe IPA ready:"
cat "$OUT/SHA256SUMS"
