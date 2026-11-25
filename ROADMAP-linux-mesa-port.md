# Roadmap: fresh Linux-Mesa-based GPU UI port (S21 / Mali-G78)

Status: **active** (2026-11-25). This supersedes `ROADMAP-jesvi-gl-route.md` as the
chosen route. It starts from the Linux port's *known-working* Mesa 26.2.4 stack on
this exact phone, not from the deleted Mesa branch or from Jesvi's G77/Mesa 25.0.7
tree. Everything under `tools/linux-mesa-ios/` is written fresh in this repository.

## Current status (2026-11-25)

The original goal — backboardd rendering the iOS UI through Mesa Panfrost on the
Mali-G78 — has been demonstrated. The current work is visual correctness and speed,
not first light. V22, V23 and V24 booted with stable EAGL injection and real
`glReadPixels()` PASS gates; V23 improved the mean frame to `383,307 us`, but icon
colours remain swapped (blue↔yellow, red↔blue), Clock sub-icons are missing, and an
expanded folder shows a black bar over the top ~25%. EGLImage/IOSurface sharing has
never engaged (`shared IOSurface` count zero). V24 source (`62b6668`) fixed the
4 KiB import-size rejection and normalised EAGL textures/readback to RGBA. CI
`45623900771` is green and only `libs21eagl.dylib` changed from V23; the V24
image passed checks, the user completed the typed swap and booted. Log analysis
showed `EGL_BAD_PARAMETER` display imports, unchanged icon bytes, returned App
Library ghosting, missing Clock sub-icons and no speed win. V25 source/docs
are committed; approved CI run `45640981522` is green and its eight Mach-O arm64
artifacts are verified. No V25 image, swap or boot has happened.

| Area | Current state | Evidence / caveat |
|---|---|---|
| Boot, display, SMP, touch, buttons | Working | iOS 16.7.16 reaches lock/home screen; all 8 cores up with `cpus=8`; touch and power/volume keys reach iOS. |
| Charging / battery | User-reported working (2026-11-25) | Treat as current device behaviour, but no separate kernel log proof is recorded here; the old photo only proved a low-battery indicator was visible. |
| GPU proof | **Passed** | `S21GPU: clear PASS rgba=00ff00ff`, textured quad, blended quad and clear+quad readback PASS. |
| backboardd/EAGL UI | **Working but not final** | 8/8 EAGL methods replaced, no respring/backboardd crash in V22/V23, visible iOS UI. Colours and some icons/UI details still wrong. |
| Storage | Working for boot | File-backed HFS+ root in `userdata`; XNU UFS remains read-only, with RAM-overlay/rootrw used for recent boots. Preserve partitions and backups. |
| Open GPU work | V25 (CI green, not deployed) | Restore V23 deep/copy-out path, implicit dma-buf modifiers, native BGRA upload for BGRA clients/targets, BGRA class for all format-0 texImage pictures, London timezone default; verify icon colours, Clock sub-icons, ghosting, import modifiers and speed. |
| Not yet proven | 10-minute soak, USB networking inside iOS, Wi-Fi, cellular, cameras, audio | Keep these separate from the GPU win. |

## 0. Goal and decisions

**Goal:** backboardd renders iOS UI through Mesa Panfrost on the Mali-G78, so the
screen shows the Apple logo, SpringBoard and apps rather than only a progress bar.

| Decision | Value | Evidence |
|---|---|---|
| Mesa version | **26.2.4** (upstream tag) | Linux port renders GLES 3.1 with `Mali-G78 MC14 (Panfrost)` / `OpenGL ES 3.1 Mesa 26.2.4`; `/home/tom/s21/S21_Exynos2100.md` §11.21+ |
| G78 model row | downstream `PAN_PROD_ID(9,2,2)` | `/home/tom/s21/mesa-26.2.4/patch_mesa_g78.py`; upstream 26.2.4/26.2.5 have no G78 row |
| GPU UAPI target | XNU `SweetMali` IOKit selectors in `xnu/patches/0112-s21-mali-iokit.sh` | selectors 0 GET_INFO, 1 CREATE_BO, 2 MAP_BO, 3 SUBMIT, 4 DESTROY_BO, 5 LOG, 6 MAPFB, 8 IMPORT_USER, 12/13 range flush |
| iOS build in our image | **16.7.16 (20H392)** | `README.md`, image `iosroot-20H392-full.img` in `NOTES.md` |
| Reuse policy | Linux port scripts/docs are the user's own work and may be adapted with credit; no code from the deleted Mesa tree; no Linux-kernel GPL code copied into iOS userland; Jesvi code stays a read-only reference for now | user decision 2026-11-23; Jesvi `LICENSE` is MIT for userland/tools, APSL for `xnu/files/` |
| Why not the old plan | Jesvi's `inject.c` has dyld-cache addresses for iOS 16.5 (20F66); our image is 20H392. Its Mesa 25.0.7 also lacks a G78 model row. | `ROADMAP-jesvi-gl-route.md` Phase 0; direct tarball checks recorded in `LINUX_S21_GPU.md` |

## 1. Target architecture

```
backboardd (CoreAnimation, GLES renderer)
   -> fresh EAGL/GL glue (20H392 symbols must be discovered, not guessed)
   -> libpanfrost_s21.dylib: Mesa 26.2.4 Panfrost + fresh libdrm shim
   -> IOConnectCall* -> SweetMali user client (0112)
   -> Mali-G78 job manager -> framebuffer/IOSurface present
```

The two new pieces are (a) the Mesa 26.2.4 iOS cross-build and (b) the drm shim.
The kernel GPU driver and display handoff already exist in this tree.

## 2. Phases

### Phase 0 — evidence and decisions (mostly done)
- [x] License decision above.
- [x] iOS build identified as 20H392; 20F66 addresses rejected.
- [x] GPU identity/params recorded below from the Linux port's register dump.
- [x] Coherency requirement recorded: Linux only rendered reliably after
      `dcache_clean_inval_poc()` on write-combined BO mappings. XNU's selector 3
      flushes all mapped BOs before/after submit; selector 13 flushes explicit
      ranges. The fresh shim must use 3 or 13/12, never assume a coherent GPU.
- [x] Repeatable test loop confirmed with the user: `boot-hojun-gpu.sh` +
      `sweet-usbnet-sniff.sh` + one codesigned `s21-gpu-test` launched from the
      cached launchd job, no root-image rebuild per test.

### Phase 1 — fresh Mesa 26.2.4 iOS build (CI green)
- [x] `tools/linux-mesa-ios/patch_mesa_g78.py`: fresh, idempotent G78 model-row patch.
- [x] Darwin/fake-libdrm glue written fresh in `patch_mesa_ios.py`, applied twice
      to pristine 26.2.4 with no failures: DRI enable on Darwin, direct
      `panfrost_drm_screen_create()`, fake DRM fd loader, surfaceless probe,
      cached BO maps.
- [x] `.github/workflows/linux-mesa-ios.yml` and `build-ios-mesa.sh` write the
      pinned tarball, check SHA-256, cross-build `libs21drm.dylib` + EGL/GLES +
      `libgallium-26.2.4.dylib` for arm64 iOS, codesign, and upload `ios-mesa-g78`.
- [x] Host-side patch-apply check and fake-IOKit test pass before any device boot:
      `patch_mesa_ios.py` + `patch_mesa_g78.py` idempotent; `s21-drm-host-test: PASS`.
- [x] macOS CI run **`45191168840`** at implementation head `12c44d8f` completed
      **success**. Artifact `ios-mesa-g78` id `14827231974` contains arm64
      `libs21drm.dylib`, EGL/GLES dylibs, `libgallium-26.2.4.dylib` and
      `s21-gpu-test`, with `@rpath` dependencies that keep one shared shim.
      The temporary `push` trigger was restored to `workflow_dispatch` only.
      Failed iterations and fixes: `45176526852`/`45178636303` (`drmSyncobjFDToHandle`,
      include paths), `45180829476`/`45182449137` (driconf macros),
      `45184666904`/`45184836741` (host LLVM/SPIRV-Tools removed from target),
      `45186829240` (arm64 `cpp_link_args`), `45189306716` (renderonly stubs).

### Phase 2 — fresh drm shim over SweetMali (done, device-verified by V5 clear PASS)
- [x] `GET_PARAM` mapping from the measured table below (including raw
      `L2_FEATURES 0x07120206`, workgroup/barrier `0x200`).
- [x] BO create/map/destroy with a client-side VA allocator; split the 128 MiB
      tiler heap into <=15 MiB kernel BOs (kernel limit in 0112).
- [x] Submit through selector 3 (with selector 12/13 range-flush support), syncobj
      emulation, `IMPORT_USER` for IOSurface-backed memory.
- [x] Fake-IOKit host test covering params, heap splitting, submit and flush calls.
- [x] Shared `libs21drm.dylib` linked by both EGL and Panfrost so BO/syncobj state
      is not duplicated across images.

### Phase 3 — one-process GLES proof (complete on device)
- [x] `tools/linux-mesa-ios/test/s21_gpu_test.c`: surfaceless GLES3 clear/readback
      with `S21GPU: ... PASS/FAIL` output; the signed arm64 binary is in the
      `ios-mesa-g78` artifact and links only the shipped `@rpath` dylibs.
- [x] Image staged on device (2026-11-23, user approved): `/data/linux/iosroot.img`
      SHA-256 `90a9a1223cf2036c1dfbe6972cda41f96e84e5ecc086e9a897121b7e9043de0d`,
      root:root 644, matches the host image byte-for-byte. `com.s21.gputest` is in
      the cached launchd bootstrap; `com.s21.glclear` was removed.
- [x] Clear+readback through Mesa Panfrost on the G78, with a full-screen PASS
      colour on the panel: user observed full-screen green on 2026-11-23 and log
      `/home/tom/Downloads/udp-sniff-20261123-210124.txt` shows every stage plus
      `S21GPU: clear PASS rgba=00ff00ff`. Selector 6 reported 1080x2400, stride 4320.
      A post-PASS paint-loop SIGSEGV was caused by a missing `& 0xffff` when decoding
      the packed height; fixed in source and rebuilt as V6 (CI `45265919487`), but
      V6 was intentionally skipped so V7 could include the fix plus new coverage.
- [x] Textured and blended quads through Mesa Panfrost on the G78. V9 Jetsam
      override fixed the `InactiveHardMemoryLimit=6` kill; V11 page-table walks
      exposed a shim/kernel 4 KiB vs 16 KiB BO VA overlap; V12 rounds BO size and
      VA span to 16 KiB. V12 boot 2026-11-24 11:27, log
      `/home/tom/Downloads/udp-sniff-20261124-112727.txt`: `textured quad PASS
      rgba=ff0000ff`, `blended quad PASS rgba=8080ffbf`, `clear+quad PASS
      rgba=8080ffbf`, all 9 submits `ret=0 out=0`, zero timeouts and zero
      `walk L0` lines. The user-observed full-screen green is the CPU-painted
      flag; the gate proof is exact `glReadPixels()` PASS.
- [x] Present into an IOSurface-backed layer using the existing framebuffer path. V14+
       injected `libs21eagl.dylib` into `backboardd`; V22/V23/V24 logs show 8/8
       EAGL methods replaced, visible SpringBoard UI and exact GPU readback PASS.
       The CPU copy-out path still presents most buffers; V25 removes the explicit
       dma-buf modifier attributes that caused V24's `EGL_BAD_PARAMETER` imports.

### Phase 4 — backboardd integration
- [x] Use the same-build (20H392 / iOS 16.7.16) HakiOS EAGL bridge from
      `egemengunel/ios-a5xelte`: its 457-entry `EAGLMacroContext` slot table was
      verified against that build's dyld cache, so no 20F66 address is reused.
      iOS 16 CoreAnimation falls back to `CA::OGL::GLESContext` when Metal gives
      it no context, so no QuartzCore vtable hook is needed.
- [x] Built V14 with `libs21eagl.dylib` and patched the cached launchd job's
      `DYLD_INSERT_LIBRARIES=/usr/bin/libs21eagl.dylib`; the V14 boot showed
      real UI progress but **zero** `S21EAGL:`/`hakios-gl:` lines, so the plist
      env was not honoured by the platform binary.
- [x] Boot a new kernel with `0145-s21-eagl-backboardd-env.patch`: the exec-time
      hook appends the same env to `/usr/libexec/backboardd`, logs
      `S21EAGL-kernel:`, and marks the injected PID in the existing AMFI dyld
      policy. Verified in the V22/V23 boots with `SWEETMETAL=1` and 8/8 EAGL
      methods replaced.
- [x] Verify logo, progress bar, SpringBoard and lock screen. The visible UI is
      real Mesa/EAGL output; Apple-logo/progress-bar presentation is not the
      final proof and CPU-painted test colours must not be confused with it.
- [ ] Fix the remaining EAGL defects: red/blue and blue/yellow icon swaps,
      missing Clock/World Clock/Alarm/Stopwatch/Timer sub-icons, expanded-folder
      icons and top black bar, and App/Library presentation details.

### Phase 5 — stabilise and optimise
- [x] Selective cache flush and real syncobj transfer (V22): all V22/V23 submits
      used `mode=list`; zero `export failed`/`CREATE_BO failed` and no respring.
- [ ] Enable EGLImage/IOSurface sharing and cut the CPU copy-out: V22/V23 mean
      copy-out was ~242 ms (~58–63% of frame time) with zero shared surfaces.
      V24 rounded sizes and normalised byte order but its display imports failed
      with `EGL_BAD_PARAMETER`; V25 uses implicit linear modifiers and logs the
      display extension status.
- [ ] Async submit, GPU clocks/thermal, renderer boot-arg and a 10-minute soak.
- [ ] Final visual acceptance: correct colours, icons, folder/black-bar behaviour
      and usable speed in one boot log and on-screen observation.

## 3. Measured GPU parameters (from `/home/tom/s21/S21_Exynos2100.md` §11.14/11.21)

| Parameter | Value | Notes |
|---|---|---|
| GPU_ID | `0x92020010` | arch 9.2, product major 2 -> `PAN_PROD_ID(9,2,2)` |
| SHADER_PRESENT | `0x00ee0cee` | 14 cores, non-contiguous; use the mask |
| TILER_PRESENT / L2_PRESENT | `0x1` / `0x1` | |
| AS_PRESENT / JS_PRESENT | `0xff` / `0x7` | 8 address spaces, 3 job slots |
| L2_FEATURES | `0x07120206` | 64 B lines, 128-bit bus, 2^18 B/slice |
| MEM_FEATURES | `0x301` | 4 L2 slices |
| MMU_FEATURES | `0x2830` | 48-bit VA, 40-bit PA |
| THREAD_FEATURES | `0x48000` | |
| MAX_THREADS | `0x400` | |
| MAX_WORKGROUP_SIZE | `0x200` | not 0x400 |
| MAX_BARRIER_SIZE | `0x200` | not 0x400 |
| TILER_FEATURES | `0x809` | 512-byte bins, 8 levels |
| TEXTURE_FEATURES0-3 | `0xf7fe007e`, `0xc3fff7ff`, `0xbfe1ff9f`, `0x000010c6` | measured |

Linux bring-up facts that must not be forgotten: G3D MMIO `0x18500000` size
`0x5000`; GIC SPI 339/340/338 (job/mmu/gpu); bootloader leaves PMU/CMU powered;
early TZ PD-restore SMC `0x82000410` was used; 400 MHz stub clock and dummy
regulator were enough to render; no vendor DVFS/OPP needed for first light.

## 4. Test checklist (every phase)

1. Kernel log shows Mali up and no `OFF:`/fault lines after a frame.
2. [x] One solid clear with exact readback, then textured and blended quads
   (V12, 2026-11-24).
3. [x] Full-screen colour result visible on the panel (no shell needed).
4. [x] Apple logo during the migration screen, then SpringBoard and lock/home
   screen via Mesa/EAGL (V14–V24 boots).
5. [ ] Correct icon colours, folder/Clock sub-icons and visible UI details in one
   boot (V25 is the next attempt; CI artifacts are verified, deployment pending).
6. [ ] 10-minute soak without GPU hang or panic.

## 5. Risks

| Risk | Mitigation |
|---|---|
| Mesa 26.2.4 iOS cross-build needs Darwin patches we must write fresh | start with host patch-apply checks, then CI |
| Non-coherent memory | explicit selector 3/12/13 flushes; readback test |
| 256 BO / 15 MiB per-BO / 512 MiB total kernel limits | split heap, log pool use |
| 20H392 symbols unknown for backboardd injection | Phase 3 stays single-process until symbols are discovered |
| G78 row values are G57-derived guesses | Linux already renders with them; revisit perf tuning only after first light |
