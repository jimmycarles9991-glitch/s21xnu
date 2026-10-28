#!/usr/bin/env python3
import os, subprocess, sys, tempfile, shutil
TREE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/xnu-scratch-001220e")
OUT = sys.argv[2] if len(sys.argv) > 2 else "0102-sweet-hid-move-range-touch.patch"
REL = "iokit/Kernel/arm/hid/SweetHID.cpp"
MARKER = "sweet-hid move range-touch fix"
EXPECT_REMOVED = 5
def die(m):
    print("REFUSE:", m); sys.exit(1)
src = open(os.path.join(TREE, REL), "rb").read().decode()
new = src
def edit(old, repl, label):
    global new
    n = new.count(old)
    if n != 1:
        die("anchor '%s' matched %d times (want 1)" % (label, n))
    new = new.replace(old, repl)
edit("#define SWEET_TOUCH_EDGE_PX 48\n",
     "#define SWEET_TOUCH_EDGE_PX 48\n#define SWEET_TOUCH_EDGE_BOTTOM_PX 100\n", "edge define")
edit("y < SWEET_TOUCH_EDGE_PX || y > 2399 - SWEET_TOUCH_EDGE_PX;",
     "y < SWEET_TOUCH_EDGE_PX || y > 2399 - SWEET_TOUCH_EDGE_BOTTOM_PX;", "edge y")
edit("\tcollection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);\n",
     "\tcollection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);\n"
     "\tcollection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n"
     "\tcollection->setIntegerValue(kIOHIDEventFieldIsBuiltIn, 1);\n", "collection")
edit("\tUInt32 cmask = 0;\n", "\tUInt32 cmask = 0;\n\tUInt32 fmask = 0;\n\tstatic const char sweet_move_fix[] __attribute__((used)) = \"" + MARKER + "\";\n", "cmask")
edit("\t\t\tif (was != down) {\n"
     "\t\t\t\tmask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;\n"
     "\t\t\t}\n"
     "\t\t\tif (moved) {\n",
     "\t\t\t/* " + MARKER + ": Touch|Range on every sample while tracked */\n"
     "\t\t\tmask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;\n"
     "\t\t\tif (moved) {\n", "mask block")
edit("\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);\n",
     "\t\tif (i == finger) {\n\t\t\tfmask = mask;\n\t\t}\n"
     "\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i + 1);\n", "index")
edit("\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerTouch, d);\n",
     "\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerTouch, d);\n"
     "\t\tchild->setFixedValue(kIOHIDEventFieldDigitizerPressure, d ? 0x10000 : 0);\n"
     "\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n", "touch/pressure")
edit("\tif (was != down) {\n\t\tIOLog(\"SWEET HID: touch %d %s at",
     "\tif (moved && down) {\n"
     "\t\tIOLog(\"SWEET HID: touch %d move at %d,%d mask=0x%x\\n\",\n"
     "\t\t    finger, x, y, (unsigned)fmask);\n\t}\n"
     "\tif (was != down) {\n\t\tIOLog(\"SWEET HID: touch %d %s at", "move log")
if MARKER not in new:
    die("marker missing")
def git(cwd, *a):
    return subprocess.run(["git", *a], cwd=cwd, capture_output=True)
tmp = tempfile.mkdtemp()
try:
    os.makedirs(os.path.join(tmp, os.path.dirname(REL)))
    p = os.path.join(tmp, REL)
    open(p, "wb").write(src.encode())
    git(tmp, "init", "-q")
    git(tmp, "-c", "user.name=x", "-c", "user.email=x@x", "add", ".")
    git(tmp, "-c", "user.name=x", "-c", "user.email=x@x", "commit", "-qm", "base")
    open(p, "wb").write(new.encode())
    patch = git(tmp, "diff", "-U1", "--", REL).stdout
    if not patch:
        die("empty diff")
    removed = sum(1 for l in patch.decode().split("\n") if l.startswith("-") and not l.startswith("---"))
    if removed != EXPECT_REMOVED:
        die("removed lines = %d, expected %d" % (removed, EXPECT_REMOVED))
    open(p, "wb").write(src.encode())
    r = subprocess.run(["git", "apply", "-"], cwd=tmp, input=patch, capture_output=True)
    if r.returncode or open(p, "rb").read() != new.encode():
        die("round-trip mismatch: " + r.stderr.decode())
finally:
    shutil.rmtree(tmp)
r = subprocess.run(["git", "apply", "--check", "-"], cwd=TREE, input=patch, capture_output=True)
if r.returncode:
    die("git apply --check vs tree: " + r.stderr.decode())
open(OUT, "wb").write(patch)
print("wrote %s (%d bytes, %d removed lines)" % (OUT, len(patch), removed))
