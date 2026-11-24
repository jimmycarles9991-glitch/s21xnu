# EAGL bridge for the S21 Mesa port

## Third-party code (MIT)

- `src/eagl.c`, `src/eagl_slots.txt`, `src/hakios.gl.h`, `include/IOKit/IOKitLib.h`
  and `mkeagl.py` are HakiOS's user-space OpenGL ES bridge for CoreAnimation's
  render server, from `oosexgang/hackios-xnu-kernel` (`userland/`), MIT,
  Copyright (c) 2025-2026 the HakiOS authors.  Full licence text in
  `LICENSE.hakios`.  They were copied from the Galaxy S7 port
  `egemengunel/ios-a5xelte` (`gpu/hakios-gl`), which uses the same iOS build
  (`20H392`, iOS 16.7.16) as the S21 root image.

The slot offsets in `src/eagl_slots.txt` were derived from 20F66/22H352 but the
S7 port verified all 457 entry points against its 20H392 dyld cache.  Unlike
iOS 18, iOS 16's render server falls back to `CA::OGL::GLESContext` by itself
when Metal cannot make a context, so no QuartzCore vtable hooks are needed.
`ca_hooks.c` from the S7 tree is deliberately **not** copied: it contains
iPhone10,3-specific addresses and only affects blur functions, not the first
working frame.

## New code (MIT)

- `src/gl_s21.c`: the `hakios.gl.h` backend over our existing surfaceless
  EGL/GLES3 Mesa 26.2.4 Panfrost stack; no IOSurface memory sharing (CPU copy
  path) on the first bring-up.
- S21-only change to the copied `src/eagl.c`: `say()` also sends each line to
  `s21_iokit_log()` so the HakiOS bridge messages reach the UDP serial console
  when backboardd's stderr is redirected.
- The build integration in `../build-ios-mesa.sh` produces
  `libs21eagl.dylib` and signs it for iOS.
