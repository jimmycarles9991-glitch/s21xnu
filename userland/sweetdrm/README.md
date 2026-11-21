# s21_sweet_drm — libdrm/Panfrost over S21's `SweetMali`

First userland slice of the Mesa/Panfrost GPU path for this S21 fork. It is
deliberately independent of the hand-written `SweetMetal` bundle: Mesa's
`panfrost` driver calls the ordinary libdrm API, and this shim translates those
calls to the **actual selectors in `xnu/patches/0112-s21-mali-iokit.sh`**.

Status: host test passes; an iOS cross-build of Mesa + this shim is staged in
`userland.yml` and awaiting CI. It has not been booted on the phone. The
CPU-working port is untouched.

## Files

- `s21_sweet_drm.c/.h`: the shim. Defines the `drm*` symbols and the
  `darwin_drm_bo_map/unmap` hooks from Mesa patch 0004.
- `fake_iokit.h/.c`, `host_test.c`, `host-test.sh`: deterministic Linux host
  test using an in-memory model of the S21 selectors.
- `s21_gl_clear_test.c`: pipe-level clear/readback test linked into Mesa by
  patch 0005; no EGL/GLX is needed on iOS.
- `../patches/mesa-26.2.5/`: Mesa patches 0001-0004 (MIT, PixOS authorship)
  and 0005 (S21 prebuilt v9 shaders + `s21-gl-clear` target).
- `../prebuilt/panfrost-shaders/v9/`: checked-in PAN_ARCH=9 shader blobs so the
  iOS cross-build does not need LLVM/`mesa_clc`.

## Why this is not the PixOS shim

PixOS's `sweet_drm_mali.c` assumes selectors `query/boNew/boClose/submit/wait/
importSurface`, where the kernel allocates the GPU VA and returns a seqno.
S21's client must choose the GPU VA and call `MAP_BO` itself, and `SUBMIT` is
synchronous with a status word instead of a seqno. S21 also caps a single BO
at 15 MiB, while Mesa asks for a 128 MiB tiler heap: this shim splits that
allocation into nine kernel BOs mapped at adjacent VAs under one DRM handle.

## Host test

```
# one-time: pristine Mesa 26.2.5 with patches 0002-0004 applied
git checkout -b s21-shim-test mesa-26.2.5
for p in userland/patches/mesa-26.2.5/000[234]*.patch; do git -C mesa apply "$OLDPWD/$p"; done
userland/sweetdrm/host-test.sh "$PWD/mesa"
```

The test checks GET_INFO/GET_PARAM, BO create+GPU-map, CPU map, submit, BO
wait/close, 128 MiB heap splitting, user-memory import via selector 8, and
syncobj basics against the fake IOKit.

## What works / what is still a stub

Implemented: version 1.2, the params Mesa needs for a G78 (values are from the
measured Linux `panparams` dump on this phone, not from the S21 user client),
BO create/map/close, heap splitting, synchronous submit, BO wait, mmap hooks,
`register_import`/`drmPrimeFDToHandle`, and binary syncobjs.

Not implemented yet: real IOSurface id imports (S21 has no `importSurface`,
the caller must supply a mapped user VA), async fences, `QUERY_BO_INFO`,
`SET_LABEL_BO`, JM contexts, perfcnt, and `drmSyncobjQuery2`. The next stage
is to build and run `s21-gl-clear` from the `ios-tools` artifact before
touching EAGL or the kernel.
