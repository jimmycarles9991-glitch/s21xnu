#!/usr/bin/env python3
import os, subprocess, sys, tempfile, shutil
H = os.path.expanduser("~/ios-sweet/xnu/patches/")
TREE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/xnu-scratch-001220e")
P102 = sys.argv[2] if len(sys.argv) > 2 else H + "0102-sweet-hid-move-range-touch.patch"
P103 = sys.argv[3] if len(sys.argv) > 3 else H + "0103-sweet-hid-clean-first-sample.patch"
OUT = sys.argv[4] if len(sys.argv) > 4 else "0104-sweet-hid-field-toggles.patch"
REL = "iokit/Kernel/arm/hid/SweetHID.cpp"
MARKER = "sweet-hid field toggles"
EXPECT_REMOVED = 7
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
    for name, pp in (("0102", P102), ("0103", P103)):
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
    edit("#define SWEET_TOUCH_EDGE_BOTTOM_PX 100\n",
         "#define SWEET_TOUCH_EDGE_BOTTOM_PX 100\n"
         "/* " + MARKER + ": boot-arg sweethidf, set bit = feature off */\n"
         "static uint32_t sweet_hid_f;\n", "decl")
    edit("\tIOLog(\"SWEET HID: touch registered\\n\");\n",
         "\tPE_parse_boot_argn(\"sweethidf\", &sweet_hid_f, sizeof(sweet_hid_f));\n"
         "\tIOLog(\"SWEET HID: touch registered " + MARKER + " f=0x%x\\n\", (unsigned)sweet_hid_f);\n", "registered")
    edit("\tcollection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n"
         "\tcollection->setIntegerValue(kIOHIDEventFieldIsBuiltIn, 1);\n",
         "\tif (!(sweet_hid_f & 8)) {\n"
         "\t\tcollection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n"
         "\t\tcollection->setIntegerValue(kIOHIDEventFieldIsBuiltIn, 1);\n\t}\n", "collection")
    edit("kIOHIDEventFieldDigitizerIndex, i + 1);",
         "kIOHIDEventFieldDigitizerIndex, i + ((sweet_hid_f & 4) ? 0 : 1));", "index")
    edit("\t\tchild->setFixedValue(kIOHIDEventFieldDigitizerPressure, d ? 0x10000 : 0);\n",
         "\t\tif (!(sweet_hid_f & 1)) {\n"
         "\t\t\tchild->setFixedValue(kIOHIDEventFieldDigitizerPressure, d ? 0x10000 : 0);\n\t\t}\n", "pressure")
    edit("\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n",
         "\t\tif (!(sweet_hid_f & 2)) {\n"
         "\t\t\tchild->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);\n\t\t}\n", "child DI")
    edit("kIOHIDEventFieldDigitizerButtonMask, any ? 1 : 0);",
         "kIOHIDEventFieldDigitizerButtonMask, (any && !(sweet_hid_f & 16)) ? 1 : 0);", "buttonmask")
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
