#!/usr/bin/env python3
"""Seed the CurrentHost DataMigration preference in a mounted fresh iOS root.

The fresh IPSW root can sit forever on the Apple-logo/progress-bar screen:
backboardd holds SpringBoard's system-shell check-in until DataMigration
finishes, and datamigrator asks for ``LastSystemVersion`` in
``com.apple.migration`` with ``kCFPreferencesCurrentUser/CurrentHost``.  That
lands in ``/var/mobile/Library/Preferences/ByHost/`` and needs a valid
IOPlatformUUID (gethostuuid).  Our ADT supplies ``/chosen/unique-chip-id`` and
``chip-id``, so the UUID is deterministic; write the ByHost plist before boot.

Host-only. This does not boot, push, kexec, flash, partition or SSH.

Usage (image mounted rw, force, uid=0, gid=0):
    sudo python3 tools/fix-fresh-image-migration.py MOUNTPOINT
"""
import hashlib
import os
import plistlib
import struct
import sys
import uuid

from mkadt import SWEET_ECID

CHIP_ID = 0x8015
# IOPlatformExpertDevice::generatePlatformUUID() namespace on arm64 (XNU).
UUID_SPACE = bytes.fromhex('A6DD4CCBB5E84AF5ACDDB6DC6A0542B8')
MOBILE_UID = 501
MOBILE_GID = 501


def platform_uuid() -> str:
    seed = UUID_SPACE + struct.pack('<Q', SWEET_ECID) + struct.pack('<I', CHIP_ID)
    digest = bytearray(hashlib.sha1(seed).digest()[:16])
    digest[6] = (digest[6] & 0x0F) | 0x50
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def main() -> int:
    if len(sys.argv) != 2:
        print(f'usage: {sys.argv[0]} MOUNTPOINT', file=sys.stderr)
        return 2
    if os.geteuid() != 0:
        print('STOP: run as root', file=sys.stderr)
        return 1

    root = os.path.abspath(sys.argv[1])
    system_version = os.path.join(
        root, 'System/Library/CoreServices/SystemVersion.plist')
    springboard = os.path.join(
        root, 'System/Library/CoreServices/SpringBoard.app')
    if not os.path.isfile(system_version) or not os.path.isdir(springboard):
        print(f'STOP: {root} does not look like an iOS root', file=sys.stderr)
        return 1

    with open(system_version, 'rb') as fh:
        build = plistlib.load(fh)['ProductBuildVersion']

    pref_dir = os.path.join(root, 'private/var/mobile/Library/Preferences')
    byhost = os.path.join(pref_dir, 'ByHost')
    os.makedirs(byhost, mode=0o755, exist_ok=True)
    os.chown(byhost, MOBILE_UID, MOBILE_GID)
    os.chmod(byhost, 0o755)

    ident = platform_uuid()
    written = []
    for name in (ident.upper(), ident.lower()):
        path = os.path.join(byhost, f'com.apple.migration.{name}.plist')
        with open(path, 'wb') as fh:
            plistlib.dump({'LastSystemVersion': build}, fh)
        os.chown(path, MOBILE_UID, MOBILE_GID)
        os.chmod(path, 0o600)
        written.append(os.path.relpath(path, root))

    # Keep the plain (non-ByHost) preference consistent if it already exists.
    plain = os.path.join(pref_dir, 'com.apple.migration.plist')
    if os.path.isfile(plain):
        with open(plain, 'rb') as fh:
            data = plistlib.load(fh)
        if data.get('LastSystemVersion') != build:
            data['LastSystemVersion'] = build
            with open(plain, 'wb') as fh:
                plistlib.dump(data, fh)
            os.chown(plain, MOBILE_UID, MOBILE_GID)
            os.chmod(plain, 0o600)
        else:
            written.append(os.path.relpath(plain, root) + ' (already correct)')

    os.sync()
    print(f'MIGRATION_BYHOST_PASS build={build} uuid={ident}')
    for path in written:
        print(f'  {path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
