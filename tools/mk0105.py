#!/usr/bin/env python3
import os, subprocess, sys, tempfile, shutil
H = os.path.expanduser("~/ios-sweet/xnu/patches/")
TREE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/xnu-scratch-001220e")
PRE = [("0102", sys.argv[2] if len(sys.argv) > 2 else H + "0102-sweet-hid-move-range-touch.patch"),
       ("0103", sys.argv[3] if len(sys.argv) > 3 else H + "0103-sweet-hid-clean-first-sample.patch"),
       ("0104", sys.argv[4] if len(sys.argv) > 4 else H + "0104-sweet-hid-field-toggles.patch")]
OUT = sys.argv[5] if len(sys.argv) > 5 else "0105-sweet-hid-edge-toggle-log.patch"
REL = "iokit/Kernel/arm/hid/SweetHID.cpp"
MARK = "at %d,%d%s f=0x%x"
EXPECT_REMOVED = 3
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
    for name, pp in PRE:
        r = git(tmp, "apply", inp=open(pp, "rb").read())
        if r.returncode:
            die(name + " does not apply: " + r.stderr.decode())
        git(tmp, "add", "."); git(tmp, "commit", "-qm", name)
    src = open(p, "rb").read().decode()
    new = src
    def edit(old, repl, label):
        global new
        n = new.count(old)
        if n != 1:
            die("anchor '%s' matched %d times" % (label, n))
        new = new.replace(old, repl)
    edit("UInt32 mask = fEdge[i] ? kIOHIDDigitizerEventFromEdgeTip : 0;",
         "UInt32 mask = (fEdge[i] && !(sweet_hid_f & 32)) ? kIOHIDDigitizerEventFromEdgeTip : 0;", "edge mask")
    edit("\t\tIOLog(\"SWEET HID: touch %d %s at %d,%d%s\\n\", finger, down ? \"down\" : \"up\", x, y,\n"
         "\t\t    fEdge[finger] ? \" edge\" : \"\");\n",
         "\t\tIOLog(\"SWEET HID: touch %d %s " + MARK + "\\n\", finger, down ? \"down\" : \"up\", x, y,\n"
         "\t\t    fEdge[finger] ? \" edge\" : \"\", (unsigned)sweet_hid_f);\n", "down/up log")
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
