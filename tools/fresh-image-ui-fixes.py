#!/usr/bin/env python3
"""Host-only UI state repairs for the fresh IPSW-root image.

The image must be mounted read-write on the host, for example at
/run/media/tom/iOSRoot1. Nothing here touches the device.

Repairs:
  * replace the iPhone X (375w-812h@3x) wallpaper HEIC payloads with the
    PNG-as-.HEIC bytes proven on the previous image. ImageIO goes by content,
    not by extension, and the S21 has no Apple HEVC decoder.
  * disable the SpringBoard/Jindo feature flag to stop UIKit choosing nil for
    the status-bar visual provider at native 360x800 points (rdar:45025538).
  * seed IconState/DesiredIconState from the stock default and add Opaline.
    A fresh IPSW root has no runtime SpringBoard state, so only a few apps
    appear on the home screen.

Idempotent: files are rewritten only when their bytes differ.
"""

import argparse
import copy
import hashlib
import os
import plistlib
import sys
from pathlib import Path

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

WALLPAPER_REL = Path("Library/Wallpaper")
DEFAULT_ICONS_REL = Path(
    "System/Library/CoreServices/SpringBoard.app/DefaultIconState~iphone.plist"
)
OPALINE_INFO_REL = Path("Applications/Opaline.app/Info.plist")
FEATURE_DIR_REL = Path("private/var/preferences/FeatureFlags/Domain")
FEATURE_FILE_REL = FEATURE_DIR_REL / "SpringBoard.plist"
SPRINGBOARD_DIR_REL = Path("private/var/mobile/Library/SpringBoard")
ICON_FILE_RELS = (
    SPRINGBOARD_DIR_REL / "IconState.plist",
    SPRINGBOARD_DIR_REL / "DesiredIconState.plist",
)
FEATURE_FLAGS = {"Jindo": {"Enabled": False}}
NEW_DIR_MODE = 0o777
NEW_FILE_MODE = 0o666


def digest(data):
    return hashlib.sha256(data).hexdigest()


def ensure_parent(path):
    """Create missing ancestors and make them traversable by mobile on iOS."""
    parent = path.parent
    missing = []
    cursor = parent
    while not cursor.exists():
        missing.append(cursor)
        if cursor.parent == cursor:
            break
        cursor = cursor.parent
    parent.mkdir(parents=True, exist_ok=True)
    for directory in reversed(missing):
        os.chmod(directory, NEW_DIR_MODE)


def write_if_changed(path, data, mode=NEW_FILE_MODE, preserve_mode=False):
    """Write bytes only on change; keep existing metadata where requested."""
    path = Path(path)
    if path.exists() and path.read_bytes() == data:
        return False
    old_mode = path.stat().st_mode & 0o777 if path.exists() else None
    ensure_parent(path)
    with open(path, "wb") as handle:
        handle.write(data)
    if preserve_mode and old_mode is not None:
        os.chmod(path, old_mode)
    else:
        os.chmod(path, mode)
    return True


def plist_format(sample):
    return plistlib.FMT_BINARY if sample.startswith(b"bplist00") else plistlib.FMT_XML


def load_plist(path):
    with open(path, "rb") as handle:
        return plistlib.load(handle)


def dump_plist(value, fmt):
    return plistlib.dumps(value, fmt=fmt, sort_keys=False)


def iter_strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for child in value.values():
            yield from iter_strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_strings(child)


def add_bundle_to_icon_state(state, bundle_id):
    pages = state.setdefault("iconLists", [])
    if bundle_id in set(iter_strings(pages)):
        return False, "already present"

    for index, page in enumerate(pages):
        if not isinstance(page, list):
            continue
        for slot, value in enumerate(page):
            if value == "":
                page[slot] = bundle_id
                return True, f"filled empty slot in page {index}"
        if len(page) < 18:
            page.append(bundle_id)
            return True, f"appended to page {index}"

    pages.append([bundle_id])
    return True, f"created page {len(pages) - 1}"


def fix_wallpaper(root, old_root):
    source_root = Path(old_root) / WALLPAPER_REL
    target_root = Path(root) / WALLPAPER_REL
    if not source_root.is_dir():
        raise SystemExit(f"missing wallpaper source: {source_root}")
    if not target_root.is_dir():
        raise SystemExit(f"missing wallpaper directory in image: {target_root}")

    replaced = already = missing = 0
    for source in sorted(source_root.rglob("*")):
        if not source.is_file() or source.suffix.lower() != ".heic":
            continue
        data = source.read_bytes()
        if not data.startswith(PNG_MAGIC):
            continue
        target = target_root / source.relative_to(source_root)
        if not target.exists():
            print(f"WARN wallpaper target missing: {target}", file=sys.stderr)
            missing += 1
            continue
        if write_if_changed(target, data, preserve_mode=True):
            replaced += 1
            print(f"wallpaper PNG payload: {target.relative_to(root)} {digest(data)}")
        else:
            already += 1
    return replaced, already, missing


def fix_feature_flags(root):
    target = Path(root) / FEATURE_FILE_REL
    data = dump_plist(FEATURE_FLAGS, plistlib.FMT_XML)
    if write_if_changed(target, data):
        print(f"Jindo disabled: {FEATURE_FILE_REL} {digest(data)}")
        return True
    print(f"Jindo already disabled: {FEATURE_FILE_REL}")
    return False


def fix_icon_state(root):
    root = Path(root)
    default_path = root / DEFAULT_ICONS_REL
    opaline_path = root / OPALINE_INFO_REL
    if not default_path.is_file():
        raise SystemExit(f"missing stock default icon state: {default_path}")
    if not opaline_path.is_file():
        raise SystemExit(f"missing Opaline Info.plist: {opaline_path}")

    bundle_id = load_plist(opaline_path).get("CFBundleIdentifier")
    if not bundle_id:
        raise SystemExit(f"no CFBundleIdentifier in {opaline_path}")

    raw = default_path.read_bytes()
    fmt = plist_format(raw)
    base = plistlib.loads(raw)

    changed = False
    for rel in ICON_FILE_RELS:
        target = root / rel
        state = copy.deepcopy(base)
        did_add, where = add_bundle_to_icon_state(state, bundle_id)
        data = dump_plist(state, fmt)
        if write_if_changed(target, data):
            changed = True
            print(f"icon state {rel}: wrote {bundle_id} ({where}) {digest(data)}")
        else:
            print(f"icon state {rel}: unchanged")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", help="host mount point of the writable iOS root image")
    parser.add_argument(
        "--wallpaper-source",
        default="/tmp/opencode/wifibt-scan",
        help="old image mount containing the proven PNG-as-.HEIC wallpapers",
    )
    args = parser.parse_args()

    root = Path(args.root)
    if not root.is_dir():
        raise SystemExit(f"not a directory: {root}")

    replaced, already, missing = fix_wallpaper(root, args.wallpaper_source)
    fix_feature_flags(root)
    fix_icon_state(root)

    print(
        f"done: wallpaper replaced={replaced} already_ok={already} "
        f"missing_targets={missing}"
    )
    if missing:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
