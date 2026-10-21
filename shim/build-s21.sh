#!/bin/bash
# Stage-1 build: arm64 Image for `kexec -l` (no boot.img). Needs aarch64-linux-gnu-{gcc,binutils}.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-aarch64-linux-gnu-gcc}; OBJCOPY=${OBJCOPY:-aarch64-linux-gnu-objcopy}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do $CC $CFLAGS -c $f -o out/${f%.*}.o; done
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
  out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
size=$(printf '%d' 0x$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}'))
truncate -s "$size" out/Image
ls -l out/Image
