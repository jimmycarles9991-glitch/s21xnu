#!/usr/bin/env python3
import os, subprocess, sys, tempfile, shutil
TREE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/xnu-scratch-001220e")
P102 = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/ios-sweet/xnu/patches/0102-sweet-hid-move-range-touch.patch")
OUT = sys.argv[3] if len(sys.argv) > 3 else "0103-sweet-hid-clean-first-sample.patch"
REL = "iokit/Kernel/arm/hid/SweetHID.cpp"
MARKER = "sweet-hid clean first sample"
EXPECT_REMOVED = 1
def die(m):
    print("REFUSE:", m); sys.exit(1)
def git(cwd, *a, inp=None):
    return subprocess.run(["git", "-c", "user.name=x", "-c", "user.email=x@x", *a], cwd=cwd, input=inp, capture_output=True)
tmp = tempfile.mkdtemp()
try:
    os.makedirs(os.path.join(tmp, os.path.dirname(REL)))
    p = os.path.join(tmp, REL)
    shutil.copy(os.path.join(TREE, REL), p)
    git(tmp, "init", "-q"); git(tmp, "add", "."); git(tmp, "commit", "-qm", "base")
    r = git(tmp, "apply", inp=open(P102, "rb").read())
    if r.returncode:
        die("0102 does not apply on scratch file: " + r.stderr.decode())
    git(tmp, "add", "."); git(tmp, "commit", "-qm", "0102")
    src = open(p, "rb").read().decode()
    old = "\tbool moved = down && (nx != fX[finger] || ny != fY[finger]);\n"
    if src.count(old) != 1:
        die("moved anchor matched %d times" % src.count(old))
    new = src.replace(old,
        "\tstatic const char sweet_first_fix[] __attribute__((used)) = \"" + MARKER + "\";\n"
        "\tbool moved = down && was && (nx != fX[finger] || ny != fY[finger]);\n")
    if MARKER not in new:
        die("marker missing")
    open(p, "wb").write(new.encode())
    patch = git(tmp, "diff", "-U1", "--", REL).stdout
    if not patch:
        die("empty diff")
    removed = sum(1 for l in patch.decode().split("\n") if l.startswith("-") and not l.startswith("---"))
    if removed != EXPECT_REMOVED:
        die("removed lines = %d, expected %d" % (removed, EXPECT_REMOVED))
    open(p, "wb").write(src.encode())
    r = git(tmp, "apply", inp=patch)
    if r.returncode or open(p, "rb").read() != new.encode():
        die("round-trip mismatch: " + r.stderr.decode())
finally:
    shutil.rmtree(tmp)
open(OUT, "wb").write(patch)
print("wrote %s (%d bytes, %d removed lines)" % (OUT, len(patch), removed))
