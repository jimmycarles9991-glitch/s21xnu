#!/usr/bin/env python3
"""Apply the comparison port's setup/activation flags to an S21 root mount.

Usage: sudo python3 tools/s21-setup-skip.py MOUNTED_ROOT
This only changes setup-related preferences and the mobileactivationd data ark.
The root must be the writable HFS+ mount of a copied image; it never accesses
the phone or edits SpringBoard binaries.
"""
import glob
import os
import plistlib
import sys

MOBILE = 501
PREFS = 'private/var/mobile/Library/Preferences'
ACTIVATION_CONTAINER = 'D90C94C4-F94E-4FCF-8A28-A93BE3846D8E'


def save(path, data, mode=0o644, owner=(MOBILE, MOBILE)):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, *owner)
    os.chmod(path, mode)


def merge(root, name, values):
    path = os.path.join(root, PREFS, name + '.plist')
    if os.path.exists(path):
        with open(path, 'rb') as f:
            data = plistlib.load(f)
    else:
        data = {}
    data.update(values)
    save(path, data, 0o600)
    print('updated', name, sorted(values))


def main(root):
    if not os.path.isdir(os.path.join(root, 'System/Library/xpc')):
        raise SystemExit('not an iOS root mount: missing System/Library/xpc')
    merge(root, 'com.apple.purplebuddy', {
        'SetupDone': True,
        'SetupFinishedAllSteps': True,
        'SetupVersion': 11,
        'daemonizeNeedsToRun': False,
    })
    merge(root, 'com.apple.purplebuddy.notbackedup', {
        key: False for key in (
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
            'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
            'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'SilentICDPUpgrade',
        )
    })
    merge(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False,
    })

    metadata = glob.glob(os.path.join(
        root, 'private/var/containers/Data/System/*',
        '.com.apple.mobile_container_manager.metadata.plist'))
    container = None
    for path in metadata:
        try:
            with open(path, 'rb') as f:
                if plistlib.load(f).get('MCMMetadataIdentifier') != 'com.apple.mobileactivationd':
                    continue
        except (OSError, plistlib.InvalidFileException):
            continue
        container = os.path.dirname(path)
        break
    if container is None:
        # A clean IPSW root has no app data containers yet. Use the same
        # container UUID and metadata shape observed in the working S21 root.
        container = os.path.join(
            root, 'private/var/containers/Data/System', ACTIVATION_CONTAINER)
        os.makedirs(container, exist_ok=True)
        os.chown(container, MOBILE, MOBILE)
        os.chmod(container, 0o755)
        save(os.path.join(container,
                          '.com.apple.mobile_container_manager.metadata.plist'),
             {'MCMMetadataIdentifier': 'com.apple.mobileactivationd'},
             0o644, owner=(0, 0))
        print('created mobileactivationd container metadata:', container)

    directory = os.path.join(container, 'Library/internal')
    os.makedirs(directory, exist_ok=True)
    os.chown(directory, MOBILE, MOBILE)
    os.chmod(directory, 0o755)
    save(os.path.join(directory, 'data_ark.plist'), {
        '-ActivationState': 'Activated',
        '-BrickState': False,
        '-BuildVersion': '20H392',
        '-ProductVersion': '16.7.16',
        '-DataMigrationCompleted': True,
        '-PreSydroDataMigrationCompleted': True,
    }, 0o600)
    print('seeded mobileactivationd data ark:', directory)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
