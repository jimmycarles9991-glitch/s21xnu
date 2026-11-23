#!/usr/bin/env python3
"""Add the Samsung Exynos 2100 Mali-G78 to Mesa 26.2.4's Panfrost model table.

Fresh port of the user's own Linux-side /home/tom/s21/mesa-26.2.4/patch_mesa_g78.py.
GPU_ID 0x92020010 decodes to PAN_PROD_ID(9, 2, 2), which upstream Mesa 26.2.4
(and 26.2.5) do not list. The tilebuffer sizes and rates are copied from G57 and
are explicitly unverified; they are what the working Linux port uses.

Usage: patch_mesa_g78.py MESA_DIR
"""
import argparse
import pathlib
import shutil
import sys

REL = pathlib.Path("src/panfrost/model/pan_model.c")
ANCHOR = '   VALHALL_MODEL(PAN_PROD_ID(10, 8, 7), 0, "G610",'
ENTRY = """   /* Samsung Exynos 2100 Mali-G78 (MC14), GPU_PROD_ID 0x9202.
    * Tilebuffer sizes and rates copied from G57: UNVERIFIED for G78. */
   VALHALL_MODEL(PAN_PROD_ID(9, 2, 2), 0, "G78", "G77", MODEL_ANISO(ALL), MODEL_TB_SIZES(16384, 8192),
                                              MODEL_RATES_X(2, 4, 8,  32,  32, 8)),
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mesa_dir", type=pathlib.Path)
    args = ap.parse_args()

    path = args.mesa_dir / REL
    backup = path.with_name(path.name + ".pre-g78")
    if not path.is_file():
        print(f"ERROR: {path} not found", file=sys.stderr)
        return 1
    text = path.read_text()
    if '"G78"' in text:
        print("G78 already present; nothing to do")
        return 0
    if backup.exists():
        print(f"ERROR: {backup} exists; refusing to overwrite a backup", file=sys.stderr)
        return 1
    if text.count(ANCHOR) != 1:
        print(f"ERROR: anchor not unique in {path}", file=sys.stderr)
        return 1

    shutil.copy2(path, backup)
    path.write_text(text.replace(ANCHOR, ENTRY + ANCHOR))
    print(f"patched {path}; backup {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
