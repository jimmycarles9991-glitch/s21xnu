# Prebuilt Panfrost PAN_ARCH=9 shaders for the S21 iOS cross-build

Mesa 26.2.5 normally generates `src/panfrost/libpan/libpan_v9.{cpp,h}` and
`libpan_shaders_v9.{c,h}` at build time with three host tools: `mesa_clc`,
`vtn_bindgen2` and `panfrost_compile`. Those tools require LLVM plus
LLVMSPIRVLib, which are not available for an iOS arm64 cross-build on the
current CI image.

These four checked-in files were generated once on a Linux x86_64 host from
the unmodified Mesa 26.2.5 `src/panfrost/libpan/*.cl` sources, using Mesa's
own `mesa_clc` (LLVM 22.1.8 + SPIRV-LLVM-Translator 22.1.5),
`vtn_bindgen2` and `panfrost_compile`. They are target-independent SPIR-V/NIR
blobs; only `PAN_ARCH=9` is included because the S21 GPU is Mali-G78
(`GPU_ID 0x92020010`, arch 9.2).

`0005-s21-prebuilt-panfrost-shaders-and-gl-clear.patch` adds the
`-Ds21-prebuilt-shaders=true` Meson option. With that option the cross-build
skips the CLC tools entirely, builds only the v9 Panfrost driver, and links
`s21-gl-clear` (a pipe-level clear/readback test) with `s21_sweet_drm.c`.

To regenerate: build the native tools from a patched Mesa 26.2.5 tree, then
copy `src/panfrost/libpan/libpan_v9.cpp`, `libpan_v9.h`,
`libpan_shaders_v9.c` and `libpan_shaders_v9.h` here.
