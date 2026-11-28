#!/usr/bin/env python3
"""Install the com.s21.gputest LaunchDaemon into a mounted S21 iOS root.

Host-only; it never boots, pushes, flashes or SSHes to the device.

The cached plist /System/Library/xpc/launchd.plist is what launchd reads with
launchd_unsecure_cache=1, so a standalone LaunchDaemons plist alone is not
enough.  Both are written here.  S21_GPU_TEST_PAINT_ONCE=1 is essential: the
otherwise identical plist recovered from the old build paints the framebuffer
forever and hides the iOS UI.

The old JetsamMemoryLimit keys in com.s21.gputest.plist were proven ineffective;
the working V9 fix was a Daemon/Override entry in the two jetsamproperties
plists.  That override is applied here as well.

Usage:
    sudo python3 tools/s21-gputest-job.py MOUNTED_ROOT
"""
import os
import plistlib
import sys

LABEL = "com.s21.gputest"
KEY = "/System/Library/LaunchDaemons/com.s21.gputest.plist"
JOB = {
    "Label": LABEL,
    "ProgramArguments": ["/usr/bin/s21-gpu-test"],
    "RunAtLoad": True,
    "KeepAlive": False,
    "StandardOutPath": "/dev/console",
    "StandardErrorPath": "/dev/console",
    "EnvironmentVariables": {"S21_GPU_TEST_PAINT_ONCE": "1"},
}
JETSAM_OVERRIDE = {
    "JetsamPriority": 40,
    "ActiveHardMemoryLimit": 128,
    "InactiveHardMemoryLimit": 128,
    "ActiveSoftMemoryLimit": 96,
}
JETSAM_FILES = (
    "System/Library/LaunchDaemons/com.apple.jetsamproperties.D22.plist",
    "System/Library/LaunchDaemons/com.apple.jetsamproperties.D221.plist",
)


def plist_format(path):
    with open(path, "rb") as handle:
        magic = handle.read(8)
    return plistlib.FMT_BINARY if magic.startswith(b"bplist00") else plistlib.FMT_XML


def read_plist(path):
    with open(path, "rb") as handle:
        return plistlib.load(handle)


def write_plist(path, value, fmt=None, mode=None):
    if fmt is None:
        fmt = plist_format(path) if os.path.exists(path) else plistlib.FMT_BINARY
    old_mode = None
    if os.path.exists(path):
        old_mode = os.stat(path).st_mode & 0o7777
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        plistlib.dump(value, handle, fmt=fmt, sort_keys=False)
    os.chmod(path, mode if mode is not None else (old_mode if old_mode is not None else 0o644))


def install_job(root):
    binary = os.path.join(root, "usr/bin/s21-gpu-test")
    if not os.path.isfile(binary):
        raise SystemExit(f"STOP: missing {binary}; install the Mesa artifact first")

    cached_path = os.path.join(root, "System/Library/xpc/launchd.plist")
    if not os.path.isfile(cached_path):
        raise SystemExit(f"STOP: cached launchd plist missing: {cached_path}")
    cached = read_plist(cached_path)
    changed = False
    for section in ("LaunchDaemons", "LaunchAgents"):
        jobs = cached.get(section)
        if not isinstance(jobs, dict):
            continue
        for key in [k for k, job in jobs.items()
                    if k != KEY and isinstance(job, dict) and job.get("Label") == LABEL]:
            del jobs[key]
            changed = True
            print(f"removed duplicate {LABEL} from cached {section}: {key}")
    jobs = cached.setdefault("LaunchDaemons", {})
    if jobs.get(KEY) != JOB:
        jobs[KEY] = dict(JOB)
        changed = True
        print("cached launchd: com.s21.gputest installed")
    else:
        print("cached launchd: com.s21.gputest already correct")
    if changed:
        write_plist(cached_path, cached, fmt=plistlib.FMT_BINARY)

    standalone = os.path.join(root, KEY.lstrip("/"))
    if not os.path.isfile(standalone) or read_plist(standalone) != JOB:
        write_plist(standalone, dict(JOB), fmt=plistlib.FMT_BINARY, mode=0o644)
        print("standalone plist: com.s21.gputest installed")
    else:
        print("standalone plist: com.s21.gputest already correct")

    # Re-read both to prove the on-disk state, not just the in-memory dict.
    after = read_plist(cached_path).get("LaunchDaemons", {}).get(KEY)
    if after != JOB or read_plist(standalone) != JOB:
        raise SystemExit("STOP: com.s21.gputest verification failed")


def install_jetsam_override(root):
    for rel in JETSAM_FILES:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            print(f"WARN: no {rel}; skipping jetsam override")
            continue
        data = read_plist(path)
        versions = data.setdefault("Version4", {})
        overrides = versions.setdefault("Daemon", {}).setdefault("Override", {})
        if overrides.get(LABEL) == JETSAM_OVERRIDE:
            print(f"jetsam: {LABEL} already correct in {rel}")
            continue
        overrides[LABEL] = dict(JETSAM_OVERRIDE)
        write_plist(path, data)
        print(f"jetsam: {LABEL} override installed in {rel}")
        if read_plist(path)["Version4"]["Daemon"]["Override"].get(LABEL) != JETSAM_OVERRIDE:
            raise SystemExit(f"STOP: jetsam verification failed: {rel}")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} MOUNTED_ROOT", file=sys.stderr)
        return 2
    if os.geteuid() != 0:
        print("STOP: run as root", file=sys.stderr)
        return 1
    root = os.path.abspath(sys.argv[1])
    if not os.path.isdir(os.path.join(root, "System/Library/CoreServices/SpringBoard.app")):
        print(f"STOP: {root} does not look like an iOS root", file=sys.stderr)
        return 1
    install_job(root)
    install_jetsam_override(root)
    print("GPUTEST_LAUNCHD_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
