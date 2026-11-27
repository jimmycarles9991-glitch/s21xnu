#!/usr/bin/env python3
"""Force UIKit to use the rectangular screen bounding-path utility.

The S21 presents a synthetic 1125x2436 (375x812@3x) display.  SpringBoard
crashes in UIKitCore's _UIScreenComplexBoundingPathUtilities when it asks the
asset manager for BoundingPathBitmap-1125x2436-375x812-3.00x.  The stock
Assets.car is byte-identical to the one in the IPSW, so replacing it is a no-op.

+[_UIScreenBoundingPathUtilities boundingPathUtilitiesForScreen:] calls
+[_UIScreenComplexBoundingPathUtilities isScreenSupported:] and selects the
complex utility when it returns true, otherwise the rectangular utility.  This
patch replaces the first instruction pair of isScreenSupported: with:

    mov w0, #0
    ret

The rest of the function is unreachable.  This avoids the complex bitmap path
entirely instead of trying to synthesize a missing asset.  It is intended for
the existing root image only; no kernel, partition or device change is made.
"""

import argparse
import hashlib
import os
import sys

SUBCACHE = "dyld_shared_cache_arm64.03"
VADDR = 0x189089B38
# Subcache-relative offset from `ipsw dyld a2o` for VADDR.  The absolute cache
# offset 0x9089b38 is not the file offset in the split dyld4 cache.
OFFSET = 0x22FDB38
OLD = bytes.fromhex("f44fbea9fd7b01a9")
NEW = bytes.fromhex("00008052c0035fd6")


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mountpoint", help="rw mountpoint of the iOS root image")
    ap.add_argument("--check", action="store_true", help="only inspect the bytes")
    args = ap.parse_args()

    path = os.path.join(
        args.mountpoint,
        "System/Library/Caches/com.apple.dyld",
        SUBCACHE,
    )
    if not os.path.isfile(path):
        print(f"STOP: missing {path}", file=sys.stderr)
        return 1

    before_hash = sha256(path)
    with open(path, "r+b") as f:
        f.seek(OFFSET)
        actual = f.read(len(OLD))
        if actual == NEW:
            state = "patched"
        elif actual == OLD:
            state = "stock"
        else:
            print(
                f"STOP: unexpected bytes at {SUBCACHE}+0x{OFFSET:x}: {actual.hex()}",
                file=sys.stderr,
            )
            return 1
        print(f"vaddr=0x{VADDR:x} file={SUBCACHE}+0x{OFFSET:x} state={state}")
        print(f"before={actual.hex()}")
        print(f"before_sha256={before_hash}")

        if args.check or state == "patched":
            return 0

        f.seek(OFFSET)
        f.write(NEW)
        f.flush()
        os.fsync(f.fileno())

    with open(path, "rb") as f:
        f.seek(OFFSET)
        after = f.read(len(NEW))
    if after != NEW:
        print(f"STOP: post-write verify failed: {after.hex()}", file=sys.stderr)
        return 1
    print(f"after={after.hex()}")
    print(f"after_sha256={sha256(path)}")
    print("UIKIT_RECTANGULAR_PATH_PATCH_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
