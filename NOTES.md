# iOS on Galaxy S21 (sweet) — Mesa/Panfrost GPU handoff

Updated 2026-11-25. Keep this file short: it is the active handoff for the next agent.

## Goal and proof bar

- Goal: injected `backboardd` CoreAnimation/EAGL renders the iOS UI through Mesa 26.2.4 Panfrost on Mali-G78.
- Proof requires **all** of:
  - GPU `glReadPixels()` PASS, not a CPU-painted colour square.
  - EAGL markers from the injected `backboardd`.
  - No dead injected `backboardd`, no respring spiral.
  - Correct colours, especially app icons and wallpaper.
  - Usable speed and a visibly correct UI.
- V22 was a real speed/stability win but not final: mean frame 422,309 us, zero `export failed`/`CREATE_BO failed`, stable UI. V23 improved speed again but colours and several UI details remain wrong. V18–V21 were not wins.

## Current hardware status (2026-11-25)

- Working: boot to iOS 16.7.16 lock/home screen and SpringBoard, 1080×2400 display, all 8 CPUs with `cpus=8`, touch/swipe-up unlock, power and volume buttons, file-backed HFS+ root in `userdata`.
- Charging works by **user report**. There is no separate kernel log proof recorded here; the older photo only showed a low-battery indicator. Treat it as current device behaviour, not a measured gate.
- GPU mostly works: real `glReadPixels()` PASS and injected `backboardd` EAGL UI in V22/V23/V26. V26 is fastest so far (median 198 ms) but not final: blue↔yellow/red↔blue icon swaps, missing Clock/Settings icons, expanded-folder black bar and focus/Wi-Fi icon backgrounds remain wrong.
- Not proven: 10-minute soak, USB networking inside iOS (the kernel CDC-ECM link is for bringing/logs), Wi-Fi, cellular, cameras and audio. Keep these separate from the GPU win.

## Hard rules

- Work only on `testing-mesa-port` in `/home/tom/s21xnu-linux-mesa`. Never touch `main`.
- Never reuse deleted commit `7cfe4f6cf72c8a6910c060a0fcb4825740e3256d`.
- Assistant must not boot, kexec, flash, repartition or overwrite the active image without explicit user approval.
- User runs the sniffer first, then boots only:
  - `cd /home/tom/Downloads && sudo ./sweet-usbnet-sniff.sh`
  - `cd /home/tom/Downloads && KERNEL_SRC=<kernel-file> EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh`
  - `KERNEL_SRC` must be the **kernel file**, not a directory.
- Preserve bootloaders, recovery, modem, persist, backups and Android. iOS root stays in the existing `userdata` image; no GPT changes. Confirm every destructive step.
- `SWEETMETAL=1` keeps Metal off so CoreAnimation uses GLES. `s21eagl=0` disables EAGL injection.
- Keep untracked `s20/` read-only. Do not commit it. Do not copy `ca_hooks.c`.
- User prefers one combined script/attempt instead of several separate boots.
- Do **not** reintroduce V18's `open("/dev/null")`/always-signalled sync-file workaround: it removed `export failed` but caused 335 `CREATE_BO failed` and instability.

## Current state

- Branch: `testing-mesa-port`. V22 source `f69330b` + build fixes; V23 source `519ada5` + `fc69974`; V24 source `62b6668`; V25 source/docs `116a1c9` through `ecb2433`; V26 source/docs `1523515` + build fixes `8310aaf`, `3fd36a5`, `b3f9891`, `c0d5080`, `dc63a31`, `9169be9`, `827ad3c`. V25 CI/image/swap/boot and V26 CI/image/swap/boot are complete and analysed below. V26 image SHA-256 `30128e5f...3ef9c`; run `45677583215` green. V27 source commit `2208185` makes `malinop` prefer `sweet_epoch=` from `kern.bootargs`; userland CI `45688962279` green and artifact verified. V27 image/push scripts are staged but no V27 image has been built or swapped. No assistant boot/kexec/flash/partition has happened. Any V27 active-image swap requires the user's typed confirmation.
- V21 source commit `c6cc351`; V21 CI green, artifacts verified, image built, pushed/swapped and **booted by the user**. V21 was not a win.
- V22 source commit `f69330b` (`gpu ios: V22 selective flush, surface import and swizzle fixes`), with build fixes `52010e9` and `2ddd129`.
  - xnu run `45596778651` on head `d0ba852`: **success**, completed `2026-11-25T10:49:34Z`. Kernel artifact `/home/tom/Downloads/kernel-v22/kernel.development.sweet`; `SHA256SUMS` PASS; Mach-O arm64; SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`. No XNU patch change; V24 will reuse this kernel.
  - linux-mesa-ios: failed runs `45596778741` (wrong IOSurface header) and `45598612838` (missing shim include path); fixed by `52010e9` and `2ddd129`. Run `45600557745` on head `2ddd129`: **success**, completed `2026-11-25T11:11:10Z`; host test passed in CI.
- V22 image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v22.img`: SHA-256 `8cf044022932213676245e5320533f6c74779a5edf6aae760f241e650646320b`, 17,179,869,184 B, `root:root 644`; pre/post `fsck.hfsplus` OK. User built, pushed and swapped it with typed `SWAP_V22`; device swap was rsync 17,179,869,184 B in 11m34s, then `dd` in 183 s. Independent recheck: `extents=3797`, `free_segments=10050`, tunables `2048`/`0`.
- V22 userland artifact `/home/tom/Downloads/ios-mesa-g78-v22/`; local `SHA256SUMS` PASS; all 8 files Mach-O arm64. Changed libs vs V21: `libgallium-26.2.4.dylib` `b68bc18f3e409fb2d4ec6482ce0f7f1d6bc2214200ba77c09ad1e9a002e147a2`, `libs21drm.dylib` `7e82ccff8062413c578dea4f665a9cf0319807eefc8c5dc783fe2b84eed26ad3`, `libs21eagl.dylib` `cc3104e8728ee7de1dcd4bd1468b5974432dc084e5434756eb6a202aa8ce7f8b`.
- V22 boot result: real speed/stability win, but not final. See below.
- V23 source fixes `519ada5` + `fc69974`: large format-0 surfaces passed to `-texImageIOSurface:` were forced BGRA; attached format-0 masks kept the opacity sampler; byte-order logging covered 128 surfaces with dimensions and caller.
- V23 linux-mesa-ios run `45612732388` on head `a96a603`: **success**, 12m39s. Kernel/xnu CI not run; V22 kernel reused. Artifacts verified: all 8 files Mach-O arm64; `SHA256SUMS` PASS; only `libs21eagl.dylib` differed from V22. V23 EAGL SHA-256 `a12ff8d1c8e43decf7478223db6ff0e6f921b5e861ca7b92a15f03cd1bbbf16f`; artifact `/home/tom/Downloads/ios-mesa-g78-v23/`.
- V23 image/push scripts `/home/tom/Downloads/install-s21eagl-image-v23.sh` and `/home/tom/Downloads/push-and-swap-v23.sh` were prepared, both `bash -n` PASS; the user installed/booted V23. Exact active V23 root SHA-256 was not recorded.
- V23 boot result: App Library ghosting fixed and speed improved, but blue/yellow and red/blue icon swaps remain, Clock/World Clock/Alarm/Stopwatch/Timer sub-icons are still missing, and expanding a folder gives a black bar over the top ~25% until the folder is closed. See below.
- V24 source work (commit `62b6668`): page-align EGLImage import sizes and normalise all 8-bit four-channel EAGL textures/readback to RGBA with explicit surface-order swizzling. No kernel change; no speculative clear/sync change for the folder bar. Full attempt approved; linux-mesa-ios CI run `45623900771` on head `4a5eefc` completed **success** in 16m18s. V24 artifacts/image verified; user ran the typed `SWAP_V24` and `ACTIVE_V24_READY` is recorded. V24 boot log analysis is complete below: stable/fast enough, but App Library ghosting returned, icon colours did not change, Clock sub-icons stayed missing, and display EGLImage imports failed with `EGL_BAD_PARAMETER`.
- V25 source/docs are committed/pushed as `116a1c9`: restore V23 deep/display/copy-out path, remove explicit dma-buf modifier attributes with extension logging, classify all format-0 `-texImageIOSurface:` pictures as BGRA, and upload BGRA client pictures/targets as native `GL_BGRA` while keeping opacity-first ABGR masks. Timezone default changed to `Europe/London`; linux-mesa-ios run `45640981522` on head `6d03744` completed **success**. Artifacts in `/home/tom/Downloads/ios-mesa-g78-v25/` are eight Mach-O arm64 files with local `SHA256SUMS` PASS; V25 EAGL `0013432b9d949df996f034248df9182947252eaa756dba36c3e5a3a6c06a202c`, gallium `b68bc18f...` and DRM `7e82ccff...` unchanged. The first V25 image `c6adcb67...26226f` panicked in `tzinit` because `/private/var/db/timezone/zoneinfo` was made a real directory. The corrected image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v25.img` SHA-256 `cbb8c3a9ec0905c3c854eff839eae75b6d2ae3a58b3a2598de9596ed86e56abf` leaves that path absent, merges zones into `/usr/share/zoneinfo.default`, and was swapped/booted by the user. V25 result is below.

## V21 boot result (observed, 2026-11-25)

- Log: `/home/tom/Downloads/udp-sniff-20261125-093638.txt` (49,257 lines / 3,978,436 bytes).
- User: “still slow and icons fucked apart from some idk. and red bit only shows on lockscreen wallpaper. … doesn't seem to be crashing springboard shit or reboot icon yet”.
- Gate/UI markers: EAGL injection pid 58 line `1037`; `S21GPU: clear PASS` line `1307`; 8/8 EAGL methods replaced lines `1492-1500`; `SWEET FB: swap` 16; `submit result` 2,041.
- Failures that are now **zero**: `CREATE_BO failed`, `panfrost_batch_init failed`, `pool exhausted`, `bo map failed`, MAP_BO rollback, `0x505`, `0x506`. So V21's BO/VA/crash fixes worked.
- Remaining: `export failed` 130, `0 drawn in place` 68.
- Frame summaries: 41; mean `1,109,026 us`, median `571,501 us`, min `370,518 us`, max `4,983,583 us`. Steady frames ~0.55 s.
- Copy-out: mean `312,143 us`, median `327,909 us`, 28.1% of frame time. IOSurface pre-time mean `172,690 us`. Copy-out is no longer dominant, but the path is still far too slow.
- Icon observation: 180×180, format `0x1908`/type `0x1401`, 720 bytes a row, often 31,092/32,400 pixels lit. Logged middle pixels after V21 conversion include `ff ff ff ff`, `ff cc c7 c7`, `ff 5e 42 fd`, `ff f7 9c 1b` — pink/orange results. Hypothesis (not yet proven): source bytes are BGRA and V21's blanket ABGR assumption swizzled them wrongly.
- Display targets observed as `'BGRA'`, 1125×2436, 4,608 bytes a row. These are the intended V22 EGLImage/dma-buf import candidates.

## V22 boot result (observed, 2026-11-25)

- Log: `/home/tom/Downloads/udp-sniff-20261125-115921.txt` (27,259 lines / 2,246,876 bytes).
- User: “OK YES FASTER BUT ICONS ALMOST THERE CUZ BLUE SHOWS AS YELLOW AND YELLOW SHOWS AS BLUE”.
- Follow-up: Settings and Clock app sub-icons (stopwatch/timer etc.) are not visible correctly; in the App Library the app window appears “ghosted into the bg”. No screenshot was provided. These symptoms are not yet tied to specific surface IDs; the log itself shows no backboardd crash or zero-lit icon surfaces.
- Gate/UI markers: EAGL injection pid 58 line `471`; `S21GPU: clear PASS` line `768`; 8/8 EAGL methods replaced lines `954-962`; `SWEET FB: swap` 16; `submit result` 993.
- Selective flush worked: 993/993 submits `mode=list`, zero `mode=full`. Zero `export failed`, `CREATE_BO failed`, `pool exhausted`, `bo map failed`, `panfrost_batch_init failed`, `0x505`, `0x506`. No backboardd/SpringBoard crash and no respring. The 10 `EXC_CORPSE_NOTIFY` entries are other daemons (widget check-in timeouts, `contextstored` non-fatal `EXC_RESOURCE`, etc.).
- EGLImage/PRIME sharing did not engage: zero `shared IOSurface` lines; wrapped surfaces used the CPU upload path. The speed gain came from the submit/fence path, not zero-copy.
- Frame summaries: 40 frames; mean `422,309 us` (V21 `1,109,026 us`), median `360,710 us` (V21 `571,501 us`), min `225,855 us`, max `1,031,407 us` (V21 max `4,983,583 us`). Steady frames 34-38 were `264-270 ms` (~3.7 fps).
- Copy-out: mean `244,995 us` / 58.0% of frame time (V21 `312,143 us` / 28.1%); IOSurface pre-time mean `127,949 us`. Copy-out is now the main remaining cost.
- Colour: first 24 unique wrapped surfaces logged; 18 BGRA and 6 ABGR (36/12 log lines because each message is duplicated). Large 180x180 icons were almost all BGRA; surface 15 hit the 985/985 opacity tie and was classified ABGR (`ff 2a 2e ff`). Small 28x65/28x95/28x28/6x85/6x6 masks were ABGR and should stay ABGR.
- V22 corrected most icon colours versus V21: the same source bytes that V21 logged as `ff 5e 42 fd` and `ff cc c7 c7` became `5e 42 fd ff` and `cc c7 c7 ff`. The remaining blue/yellow swap is consistent with the opacity heuristic tying on saturated icons. V23 forces format-0 surfaces with width or height >= 100 to BGRA and keeps the sampler for small masks.

## V23 boot result (observed, 2026-11-25)

- Log: `/home/tom/Downloads/udp-sniff-20261125-130800.txt` (59,894 lines / 4,904,917 bytes).
- User: “still icons w blue show as yellow, icons with red show as blue, ghosting issue w library bg is fixed, but the app icons look normal in the small 2x2 preview in the folders  but when i click on the 2x2 to expand the folder they are back to weird and has a black like bar on top of screen 25% until i click off that folder it goes, still dont see world clock alarm stopwatch timer icons”.
- Gate/stability: `S21GPU: clear PASS rgba=00ff00ff` 1; 8/8 EAGL methods ours 1; `SWEET FB: swap` 16; 2,483/2,483 submits `mode=list`, zero `mode=full`. Zero `export failed`, `CREATE_BO failed`, `pool exhausted`, `bo map failed`, `panfrost_batch_init failed`, `0x505`, `0x506`. No backboardd/SpringBoard crash or respring marker. Nine `EXC_CORPSE_NOTIFY` entries were present but not attributed to the injected processes.
- Speed: 46 frame summaries parsed; mean `383,307 us`, median `306,816 us`, min `22,861 us`, max `1,055,709 us` (V22 mean `422,309 us`, median `360,710 us`). Copy-out mean `242,177 us` / 63.2% of frame time; `shared IOSurface` count remained zero. The V23 gain came from the same V22 selective-flush path, not zero-copy.
- Colour/UI: the V23 large-client-picture BGRA forcing did **not** fix blue/yellow or red/blue icon swaps. App Library ghosting is fixed. Small 2x2 folder previews look normal, but expanded folders revert to swapped icons and show a black bar over the top ~25% until the folder is closed. World Clock/Alarm/Stopwatch/Timer sub-icons remain missing.
- Diagnosis: V23's byte-order guess was insufficient. V24 now targets two concrete causes: EGLImage imports were silently rejected for non-page-multiple sizes, and `GL_BGRA` upload/readback itself is unreliable on this port.

## V24 boot result (log analysed 2026-11-25)

- Log: `/home/tom/Downloads/udp-sniff-20261125-142842.txt` (29,681 lines / 2,439,871 bytes). User: “ghosting issue is back on library it shows my apps page as the bg again and colors still same dont see 4 icons on clock either so properly look into that issue and fix it aswell as performance wise.....” and asked for London as the default timezone.
- Gate/stability: `S21GPU: clear PASS` 1; 8/8 EAGL methods replaced; `SWEET FB: swap` 16; 1,153/1,153 submits `mode=list`. Zero `export failed`, `CREATE_BO failed`, `pool exhausted`, `bo map failed`, `panfrost_batch_init failed`, `0x505`, `0x506`, `respring`, `crash: backboardd`, `shared IOSurface` and `import_user failed`. This confirms the V22 selective-flush path still works.
- Import failure: `eglCreateImageKHR failed for IOSurface 2/3: EGL_BAD_PARAMETER (0x300c)`. The V24 size rounding was a dead end: the 1125×2436 display IOSurfaces allocate `11,239,424` bytes, already 4 KiB-aligned, and V23/V24 both had zero `shared IOSurface`. Leading cause is the explicit `EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT/HI_EXT` attributes: Mesa's `eglimage.c` rejects them with `EGL_BAD_PARAMETER` unless the display advertises `EGL_EXT_image_dma_buf_import_modifiers`. V25 removes those attributes and logs extension status; the exact display extension list is not in the V24 log.
- Colour: V23/V24 `icon: texture ... filled from surface ... the middle` byte logs are equivalent. V24's RGBA normalisation did not reach the failing icon bytes. V25 therefore restores V23's deep/display/copy-out path and targets the wrapped format-0 byte order directly.
- Clock-like wrapped surfaces in V24: ids 19 (`28x65`), 20 (`28x95`), 21 (`28x28`), 22 (`6x85`) and 23 (`6x6`), all classified `ABGR`. V25 classifies every format-0 `-texImageIOSurface:` picture as BGRA while keeping attached masks on the opacity sampler.
- Speed: V24 still shows large copy-outs (`215357`, `225019`, `230284`, `452009` us in the sampled frames). No zero-copy engagement, so V25 does not add speculative flush/sync changes; it restores the stable V23 path and fixes the import attributes that may eventually let EGLImage replace CPU copy-out.

## V25 corrected boot result (observed, 2026-11-25)

- Log: `/home/tom/Downloads/udp-sniff-20261125-172800.txt` (95,299 lines / 7,782,358 bytes). Image SHA-256 `cbb8c3a9...e56abf`; `tzinit` completed (lines 96-97, no panic), `exec timed` line 424. User: “ghosting still there speed still same ish colors still same, most icons gone in settings like brightness icon thingy back icon clock icons still nada time wrong so get the phone built in clock working ... shows london timezone only at jan 1 1970 2:16 am”.
- Gate/stability: `S21GPU: clear PASS rgba=00ff00ff` line 732; 8/8 EAGL methods ours line 908; `SWEET FB: swap` 16; 4,263/4,263 submits `mode=list`. Zero `mode=full`, `export failed`, `CREATE_BO failed`, `0x505`, `0x506`, `respring`, `crash: backboardd`. This is a stable boot, not a visible win.
- Speed: 45 frame summaries; mean `466,853 us`, median `454,535 us` (V23 mean `383,307`, median `306,816`; V24 mean `434,178`). Copy-out mean `206,438 us` / 44.2%. V25 made the frame median worse despite less relative copy-out.
- Colour/UI: 162 `wrapped surface` lines and 106 `native GL_BGRA` lines confirm the V25 colour path ran. Icon middle bytes reversed relative to V23/V24 (`c7 c7 cc ff` vs `cc c7 c7 ff`, `ff bf 00 ff` vs `00 bf ff ff`), but the user saw no colour win. Ghosting, Settings icons, Clock sub-icons and colours all remained wrong.
- EGLImage: extension log at lines 2026-2027 says dma-buf import modifiers absent; 16 `eglCreateImageKHR failed ... EGL_BAD_PARAMETER (0x300c)`; zero `shared IOSurface`. Later source inspection found the real cause: the iOS cross-build never defined `HAVE_LIBDRM`, so Mesa compiled out `EGL_LINUX_DMA_BUF_EXT` entirely. Fake fd/planes/pitch/modifiers were never reached.
- Clock: SpringBoard logs `1970-01-01 02:14:05` at line 3649 while uptime is `4445 s`; London correctly adds one historical hour to the 1970 base. `tzinit`/`timed` are working. Important correction: the hardware RTC exists and Linux uses it successfully (`/home/tom/s21/linux/drivers/firmware/t2s_acpm_rtc.c`, DTS RTC child under ACPM, `/home/tom/s21/boot66.log` shows `registered as rtc0` and `setting system clock to 2026-11-11T14:22:18 UTC`). The failure is in the XNU/iOS port: it has no PMIC RTC read driver (only an `IORTC` placeholder) and no code calls `settimeofday()`. `userland/malinop.c` already does it from `/usr/local/etc/sweet_epoch`, but that file was absent and no launchd job ran malinop.

## V26 boot result (observed, 2026-11-25)

- Image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v26.img`: SHA-256 `30128e5fb583eeb2dd7a9910a75398e3d1636ab21ba72accfd9fd42b0323ef9c`; `.sha256` matches. Log: `/home/tom/Downloads/udp-sniff-20261125-193549.txt` (51,868 lines / 4,242,853 bytes). User: ghosting fixed, date working, slightly faster, but time about 39 min behind; colours/icons unchanged; focus/Wi-Fi icons on white backgrounds; Settings brightness/back icons missing; Calls call icon and four Favorites icons gone; Clock sub-icons missing; black bar remains in Library/folders.
- Gate/stability: `S21GPU: clear PASS rgba=00ff00ff` 1; 8/8 EAGL methods ours 1; `SWEET FB: swap` 16; 2,215/2,215 submits `mode=list`, zero `mode=full`. Zero `export failed`, `CREATE_BO failed`, `pool exhausted`, `bo map failed`, `panfrost_batch_init failed`, `0x505`, `0x506`, `respring`, `crash: backboardd`. Nine `EXC_CORPSE_NOTIFY` entries were unrelated daemons.
- EGLImage import now engaged: modifiers advertised; `shared IOSurface` ids `[2, 3, 32, 65]` at 1125×2436 BGRA; zero `eglCreateImageKHR failed`, `createImageFromDmaBufs failed`, `import_user failed`. Icon surfaces (e.g. 180×180 ids 12–15/21–31) and small Clock-like masks (28×65/28×95/28×28/6×85/6×6) still have pixel format `0`, so they remain on the CPU upload path. V26 restored V23-style icon bytes (`cc c7 c7 ff`, `5e 42 fd ff`, `f7 9c 1b ff`); V25's reversed bytes were not visible as a win.
- Speed: 42 frame summaries; mean `220,580 us`, median `198,164 us`, min `34,540 us`, max `1,031,114 us`. This is better than V23 median `306,816`, V22 `360,710` and V25 `454,535`. The gain came from the stable selective-flush path and display EGLImage sharing; icon/colour defects remain.
- Clock: `exec malinop` at line 477 and jetsam idle at 7623, but no `clock set to` line in the log. SpringBoard first timestamp `2026-11-25 18:58:43.753` at line 3676 while the user reported about 19:38, consistent with `/usr/local/etc/sweet_epoch` being written at image-build time. Timezone selection itself is working; only the epoch seed is stale.
- Diagnosis: with V23/V26 byte order restored and V25's opposite bytes also failing visually, the colour/icon fault is probably after texture upload (sampling shader, framebuffer write, display EGLImage texture or scanout format), not the CPU byte order alone. V27 adds shader/census logging rather than another blind colour swap.

## V22 combined attempt

One userland build; no kernel source change. Targets speed and colour together.

1. **Selective cache flush** (`shim/s21_iokit.h`, `s21_iokit_darwin.c`, `s21_iokit_host.c`, `shim/s21_drm.c`)
   - Use existing kernel selector 11 `kSMSubmitList`: scalars `[slot, job_chain, timeout_ms, nread]`, structure input is `uint64_t[]` GPU VAs.
   - Duplicate each large BO VA into pre- and post-job halves; kernel always flushes BOs ≤16 pages.
   - Fall back to full-flush selector 3 `S21_SUBMIT` if selector 11 fails; log `mode=list` vs `mode=full`.
   - Signal Mesa's out syncobj from the synchronous shim submit. No fake sync-file export.
2. **PRIME/EGLImage sharing** (`gl_s21.c`, `shim/s21_drm.c`, `patch_mesa_ios.py`)
   - `s21_drm_import_user()` wraps IOSurface memory; fake PRIME fd is `0x50000000 + handle` and must never be passed to `dup()`/`close()`.
   - `s21_drm_prime_fd_size()` replaces upstream `lseek()` in `pan_kmod_bo_import()`.
   - Panfrost pipe screen advertises `DRM_PRIME_CAP_IMPORT`.
   - EGLImage import restricted to simple 4-byte `'BGRA'`, 0 planes, page-aligned allocation ≤15 MiB; bounded 16-entry cache retaining IOSurfaces; CPU fallback on every failure.
   - `hakios_gl_surface_written()` calls `glFinish()` before CoreAnimation publishes the surface.
3. **Colour handling** (`eagl/src/eagl.c`)
   - Content-based byte-order detection for format-0 wrapped surfaces instead of blanket ABGR.
   - BGRA-for-RGBA swizzle in CPU upload path; swizzle assignment shared by `-texImageIOSurface:` and `-attachImage:`.
4. **Fence/syncobj** (`patch_mesa_ios.py`)
   - `panfrost_fence_create()` transfers `ctx->syncobj` into a fresh syncobj.
   - `panfrost_fence_get_fd()` and `panfrost_fence_server_sync()` are no-ops under `S21_IOS`.

Expected outcome is uncertain: best case stable/faster with mostly correct icons; middle case faster but imperfect colours; worst case PRIME/EGLImage import fails or the syncobj path destabilises `backboardd`. CPU fallback should keep basic rendering alive.

## V22 local checks (all PASS)

- `git diff --check`: PASS.
- `python3 -m py_compile tools/linux-mesa-ios/patch_mesa_ios.py`: PASS.
- Fresh `cp -a /home/tom/s21/mesa-26.2.4 /tmp/opencode/mesa-v22`; `patch_mesa_ios.py` + `patch_mesa_g78.py`: PASS. Patched `pan_kmod.c`, `pan_screen.c`, `pan_fence.c`, `pan_context.c` as intended.
- `CC=cc MESA_DIR=/tmp/opencode/mesa-v22 tools/linux-mesa-ios/shim/build-host-test.sh` → `s21-drm-host-test: PASS`.
- Syntax-only compile of `gl_s21.c` with iOS/Mesa stubs: PASS.
- No XNU patch changes and no `s20/` files were committed.

## V24 source attempt (CI green, not deployed)

- Source commit `62b6668` (`eagl: fix V24 import size and RGBA byte order`) pushed on `testing-mesa-port`; no kernel or XNU patch change. V22 kernel `1607a49d...` is still the intended boot kernel. Approved linux-mesa-ios CI run `45623900771` on head `4a5eefc` completed **success** in 16m18s; no V24 boot result exists.
- CI artifact `ios-mesa-g78` downloaded to `/home/tom/Downloads/ios-mesa-g78-v24/`. Eight files are Mach-O arm64; local `SHA256SUMS` PASS. Only `libs21eagl.dylib` differs from V23: V23 `a12ff8d1c8e43decf7478223db6ff0e6f921b5e861ca7b92a15f03cd1bbbf16f` → V24 `7215eeb4df1a617fabc17b1b4f073fd08ca421dcc7125b62db99d61a953186be`. `libgallium-26.2.4.dylib`, `libs21drm.dylib`, both EGL libs, both GLESv2 libs and `s21-gpu-test` keep their V22/V23 hashes.
- Local V23 base image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v23.img` SHA-256 `d97a73a297db0c51ccff0c6592050cbc408fccc1445f54a22a6bc2a28e0b558f`. V24 image/push scripts prepared at `/home/tom/Downloads/install-s21eagl-image-v24.sh` and `/home/tom/Downloads/push-and-swap-v24.sh`, both `bash -n` PASS. User built `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v24.img`: SHA-256 `db4c532619bef3062b898e441a4112015709d9b8979db82696227dc108ca5f86`, 17,179,869,184 B, `root:root 644`; `.sha256` verification PASS and post-`fsck.hfsplus` says volume OK.
- User ran `/home/tom/Downloads/push-and-swap-v24.sh` with typed `SWAP_V24`. Reported: active V23 verified before push; rsync 17,179,869,184 B at 23.28 MB/s in 11m43s; stage hash `db4c5326...ca5f86`; F2FS GC ran because `free_segments=9995`, finished at `10055`; `dd` wrote the image in ~185.75 s. Device reported `ACTIVE_V24_READY`, active hash `db4c532619bef3062b898e441a4112015709d9b8979db82696227dc108ca5f86`, `extents=3307`, `free_segments=10069`, `gc_pin_file_thresh=2048`, `gc_urgent=0`. This is user-reported terminal output, not an assistant SSH check.
- Fix 1 — EGLImage import (`tools/linux-mesa-ios/eagl/src/gl_s21.c`): `importable()` previously required `IOSurfaceGetAllocSize()` to be a 4 KiB multiple. The 1125 x 2436 display surface has row 4608, `row * height = 11,225,088` bytes (`0xab4800`, not page-aligned); rounded size is `11,227,136` (`0xab5000`). V24 rounds up to 4 KiB and logs the first 16 refusals through `import_refused()` with surface id, dimensions, row, needed and alloc sizes. This directly targets V22/V23's zero `shared IOSurface`.
- Fix 2 — byte order (`tools/linux-mesa-ios/eagl/src/eagl.c`): the V23 user report plus logged source bytes fit a red/blue swap: blue/cyan `00 bf ff` appearing yellow `ff bf 00`, and orange `ff 7a 00` appearing blue `00 7a ff`. V24 normalises all 8-bit four-channel uploads to `GL_RGBA`, reads back with `GL_RGBA`, and converts explicitly to the IOSurface's byte order (BGRA normally, ABGR for wrapped opacity-first masks). Deep `b3a8` conversion now uses RGBA internally. Attached BGRA masks are included; no speculative clear/sync change was added for the expanded-folder black bar.
- Proof markers for the next boot: `S21EAGL: shared IOSurface` and/or `S21EAGL: IOSurface ... not importable` should now appear; icon red/blue and blue/yellow swaps should be gone; expanded-folder icons and black bar, Clock sub-icons and speed/stability must be rechecked.
- V24 local checks (all PASS): `git diff --check`; `clang -fsyntax-only -std=gnu11 -w` on both edited EAGL sources with a freshly generated `eagl_gen.h`, Mesa 26.2.4 headers, the shim/include paths and temporary iOS stubs. Host shim test was not rerun because no shim file changed. No `s20/` file was committed.

## V25 prepared changes (local checks PASS)

- `gl_s21.c`: extension-status log once, and no explicit `EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT/HI_EXT` attributes. Mesa's `eglimage.c` rejects explicit modifiers with `EGL_BAD_PARAMETER` when `EGL_EXT_image_dma_buf_import_modifiers` is absent; removing them uses the implicit linear modifier.
- `eagl.c`: V24 deep texture/upload/copy-out block reverted to V23's proven blue-first path; all format-0 surfaces passed to `-texImageIOSurface:` (including 28×65/28×95/28×28/6×85/6×6 Clock-like surfaces) are classified BGRA; attached format-0 masks still use the opacity sampler. BGRA client pictures and BGRA targets upload as native `GL_BGRA` with no CPU red/blue swap; opacity-first ABGR masks keep the swizzle.
- Timezone: `s21-root-fixes.py` defaults to `Europe/London`; the V25 image script keeps `private/var/db/timezone/zoneinfo` absent (iOS/tzinit creates it as a symlink at boot), merges the complete host `/usr/share/zoneinfo` database into `/usr/share/zoneinfo.default`, and points `localtime` at the chosen default. Any installed zone can be selected with `sudo env TZ_DEFAULT=Area/City /home/tom/Downloads/install-s21eagl-image-v25.sh`; London remains the default.
- Local checks (all PASS): `git diff --check`, `python3 -m py_compile tools/s21-root-fixes.py`, `bash -n` on both V25 scripts, and `clang -fsyntax-only -std=gnu11 -w -DS21_IOS` on both edited EAGL sources with a freshly generated `eagl_gen.h`, Mesa 26.2.4 headers, shim/include paths and temporary iOS stubs. Host shim test was not rerun because no shim file changed. No `s20/` file was touched. After CI download, the image script was tightened to assert exact V25 EAGL hash `0013432b...`.
- First V25 image build (user, 2026-11-25) passed all eight artifact `SHA256SUMS` checks, then stopped with `STOP: Europe/London zoneinfo is missing in the base image`. Wrong early diagnosis: V24's `private/var/db/timezone/zoneinfo` is absent in the image, not a Linux-invisible symlink. The shipped `/usr/share/zoneinfo.default` already has 620 entries including `Europe/London`, `America/New_York`, `Asia/Seoul`, `Australia/Sydney` and `UTC`.
- Observed V25 boot failure: first corrected-build image SHA-256 `c6adcb...26226f` made `/private/var/db/timezone/zoneinfo` a real directory with 1,865 host zone files. `tzinit[17]` then exited 17 before EAGL/backboardd. Extracted V24 image metadata proves the base has no `private/var/db/timezone/zoneinfo`, `usr/share/zoneinfo` is a symlink to `/var/db/timezone/zoneinfo`, and `usr/share/zoneinfo.default` is a real directory with 620 files. `s20/Jesvi-iOS-main/userland/build.sh` rewrites the var skeleton so `/private/var/db/timezone/zoneinfo` is a symlink to `/usr/share/zoneinfo.default`. Working hypothesis, not proven from source: tzinit expects to create/use that symlink and a real directory at the path makes it exit 17. The image script now leaves that path absent and merges host zones only into `/usr/share/zoneinfo.default`. No source or CI redo is needed.
- The first failed build left `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v25.img` as an interrupted V25 work image: hash `63c17d59be5fe5ff1c735f7a2046585e55855bbf3789cadb5fff63a96a848b08`, no `.sha256` file. The first built/booted image later had hash `c6adcb67...26226f` with a `.sha256` file. The corrected image script removes only those known V25 states (or a base copy / verified V22 gallium+DRM plus V24/V25 EAGL partial without a final SHA) before rebuilding; it refuses unexplained existing images. The push/swap script now accepts the known-bad V25 active hash as well as V24, and removes a stale full-size staging file before rsync. `bash -n` PASS on both corrected scripts.

## V26 build complete (CI green; first image attempt stopped at script bug)

- `tools/linux-mesa-ios/eagl/src/eagl.c`: restore V23's wrapped-surface classification (large format-0 surfaces BGRA, small masks and attached surfaces use the opacity sampler) and V23's CPU BGRA-for-RGBA upload path. Keep the improved dimension/caller logging. This removes V25's `native GL_BGRA` gamble, which made the frame median worse without visual improvement.
- `tools/linux-mesa-ios/build-ios-mesa.sh`: define `-DHAVE_LIBDRM` in the cross-file `c_args` and `cpp_args`. Mesa's `meson.build` normally disables libdrm on iOS because there is no KMS/DRM sysfs; without it, `EGL_LINUX_DMA_BUF_EXT` handling and both dma-buf EGL extensions are compiled out. The S21 build already supplies fake libdrm headers and `libs21drm.dylib` shims, so this should let `eglCreateImageKHR` reach Panfrost. V25's implicit linear modifier is valid; no `gl_s21.c` change was needed for this root cause.
- `tools/linux-mesa-ios/include/xf86drm.h` (commits `8310aaf`, `3fd36a5`, `dc63a31`): fixes CI failures `45668154779`, `45670519474` and `45674981067`. Global `-DHAVE_LIBDRM` made Mesa compile previously disabled loader/DRI/EGL code, which needs `DRM_NODE_PRIMARY/CONTROL/RENDER/MAX`, `DRM_BUS_PCI/USB/PLATFORM/HOST1X/FAUX`, `DRM_PRIME_CAP_IMPORT/EXPORT`, `drmAuthMagic`, `drmCreateContext`, `drmDestroyContext`, `drmDevicesEqual` and `drm_context_t`. Values/signatures match Mesa's own no-libdrm shim and real libdrm.
- `tools/linux-mesa-ios/shim/s21_drm.c` (commits `b3f9891`, `c0d5080`, `dc63a31`): fixes CI failures `45672602689` and `45674981067` plus the earlier helper-stub problem. Meson omits `pipe_loader_drm.c` because `dep_libdrm.found()` is false, but cross-file `-DHAVE_LIBDRM` still compiles its callers. Exports ABI-compatible failure stubs for the DRM helpers, `pipe_loader_drm_probe`, `pipe_loader_drm_probe_fd`, `pipe_loader_drm_get_driconf_by_name`, `pipe_loader_get_compatible_render_capable_device_fd`/`..._fds`, `drmAuthMagic`, `drmCreateContext`, `drmDestroyContext` and pointer-identity `drmDevicesEqual`. These paths must report "no DRM device" on iOS and never enter the real KMS pipeline.
- `tools/s21-root-fixes.py`: install `/System/Library/LaunchDaemons/000.s21.clockrestore.plist`, a `RunAtLoad` root job for existing `/usr/bin/malinop`, in both the cached launchd.plist and the standalone plist. The `000` prefix places it before backboardd/timed in the sorted cached job list. The image script writes the host Unix time to `/usr/local/etc/sweet_epoch`; malinop calls `settimeofday()` early and logs `malinop: clock set to ...` to `/dev/console`. This is a wall-clock workaround, not an XNU RTC driver.
- Scripts created: `/home/tom/Downloads/install-s21eagl-image-v26.sh` and `/home/tom/Downloads/push-and-swap-v26.sh`. The image script currently has placeholder `*_V26=0000...`; it must be updated with all verified CI hashes. Because `c0d5080`/`dc63a31` change `libs21drm.dylib`, it now requires `DRM_V26` and explicitly rejects the unchanged V22 shim hash. It uses corrected V25 SHA-256 `cbb8c3a9...e56abf` as the base, keeps all zones, leaves `private/var/db/timezone/zoneinfo` absent, defaults to London, and accepts `TZ_DEFAULT=Area/City`.
- Local checks (all PASS): `git diff --check`, `bash -n` on both V26 scripts and `build-ios-mesa.sh`, `python3 -m py_compile tools/s21-root-fixes.py`, fresh Mesa 26.2.4 patch scripts, a dry run of root-fixes against the V25 cached launchd.plist, `clang -fsyntax-only` on both edited EAGL sources, a `clang -fsyntax-only` fake-header check, `CC=cc MESA_DIR=/tmp/opencode/mesa-v26 tools/linux-mesa-ios/shim/build-host-test.sh`, a comprehensive scan of DRM call sites in EGL/loader/DRI/pipe-loader/Panfrost sources (28 call names: zero missing declarations and zero declared-but-unimplemented symbols), and an `nm -g` export check for the new DRM and pipe-loader stubs. No `s20/` file was touched.
- linux-mesa-ios run `45677583215` on `dc63a31`: **success**, completed `2026-11-25T18:49:27Z`, 8m41s. Artifacts downloaded to `/home/tom/Downloads/ios-mesa-g78-v26/`; locally generated `SHA256SUMS` PASS and all eight files are Mach-O arm64. SHA-256: `libEGL.1.dylib` `72ae3d6e5a174897b11487439310f71c5ddbcffc63f84e3c3091530fc44357ed`, `libEGL.dylib` `273bf66741266f47af5274a4268d18e57464a0ae936c07b441accafec2600b98`, `libGLESv2.2.dylib` `8abca4e119e0681c6a1c9bb78b423d2b52618d107420ae0ebbf9bf2a4157c3c0`, `libGLESv2.dylib` `42294d28442171ee9eb64f128d4a3e2d6c7fefd5a59dc1b7b7e58080a472d6b6`, `libgallium-26.2.4.dylib` `ee914f4008aec912d654564e6332bc2d07b805c99bcf0185ca7f16b12edb9b49`, `libs21drm.dylib` `6f30c42fcf2ecf96ac21381748cfaa0fc0ce03adb4761728f169062aecc6f2e4`, `libs21eagl.dylib` `dbf6efa94bb8e405a99907d0e126412cb58b8681e39e8d10d5e794e5b67d6c24`, `s21-gpu-test` `a00d00ff4584e7b637d2c13619244895c3debae8fded33ffb9063ef7c6f3b6ea`. Only GLESv2 is unchanged from V22; EGL, Gallium, DRM shim and EAGL all changed.
- `/home/tom/Downloads/install-s21eagl-image-v26.sh` is now pinned to all seven dylib hashes above, has `bash -n` PASS, and explicitly rejects an unchanged V22 `libs21drm.dylib`. First user run (2026-11-25, after 18:49 CI) passed artifact checks, installed all V26 dylibs and the early clock job, then stopped with `STOP: clock restore job is missing in System/Library/LaunchDaemons/000.s21.clockrestore.plist`. Root cause was a script-only verification bug: it opened the standalone plist twice instead of reading the cached job from `System/Library/xpc/launchd.plist`. Fixed the verifier and extended interrupted-image recovery to recognise a partial image with V26 dylibs installed. Local `bash -n` and synthetic cached+standalone plist verification PASS. User should rerun the same install command; it will remove the known partial image and rebuild from the verified V25 base.

## Next move

1. V27 is one combined diagnostic/clock attempt. Source commit `2208185` makes `/usr/bin/malinop` prefer `sweet_epoch=` from `sysctlbyname("kern.bootargs")` with `/usr/local/etc/sweet_epoch` only as fallback. Userland CI `45688962279` on that commit is **success**; `/home/tom/Downloads/ios-tools-v27/malinop` is Mach-O arm64 and pinned as SHA-256 `6c2f05456e71de9d684e5acfeef265444fb2b108ace37bc7f38a25b41c3fdca5`.
2. User runs `sudo /home/tom/Downloads/install-s21eagl-image-v27.sh`. It starts from verified V26 image `30128e5f...3ef9c`, checks all V26 Mesa hashes, replaces only `malinop`, preserves the clockrestore plist, full timezone database and London default, and leaves `private/var/db/timezone/zoneinfo` absent. No Mesa artifact or kernel changes are in V27.
3. After `V27_IMAGE_READY` and fsck/hash checks, user puts the device in Android and runs `/home/tom/Downloads/push-and-swap-v27.sh` with typed `SWAP_V27`. It accepts active V26 only; no on-device rollback is kept. Assistant must not boot/kexec/flash/partition.
4. User starts `cd /home/tom/Downloads && sudo ./sweet-usbnet-sniff.sh` before the next boot, then boots only with `KERNEL_SRC=/home/tom/Downloads/kernel-v22/kernel.development.sweet EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh`. `KERNEL_SRC` must be the kernel file. The boot script now automatically appends `sweet_epoch=$(date +%s)`, `glshaders=1` and `glcensus=1`; use `DIAG_ARGS=0` for a quieter boot if needed. Do not add a second `sweet_epoch=` to `EXTRA`.
5. V27 log gates: `malinop: clock set to ... from kern.bootargs sweet_epoch=` and SpringBoard time close to host time; all V26 GPU/EAGL/import/stability gates still hold; shader text for program 5 and the small Clock-like mask programs, plus `glcensus` function names, are present for diagnosis. Do not change colour handling again until those shader/draw logs identify where the swap or missing sub-icons happens. Keep V26/V25/V24 host images as rollback references.

## Active device state

- User rebuilt V26 as SHA-256 `30128e5fb583eeb2dd7a9910a75398e3d1636ab21ba72accfd9fd42b0323ef9c`, typed `SWAP_V26`, and booted it at 19:35. It reached the fastest stable EAGL UI so far, but the wall clock was 39 minutes behind and the colour/sub-icon/black-bar defects remained. Current Android/iOS state is not confirmed here; user controls the device. Do not issue SSH/kexec/flash/partition commands until the user says otherwise.
- Host rollback images remain: V26 `30128e5f...3ef9c`, corrected V25 `cbb8c3a9...e56abf`, V24 `db4c5326...ca5f86`, V23 `d97a73a2...558f`. No on-device rollback was kept.
- The V22 kernel file `/home/tom/Downloads/kernel-v22/kernel.development.sweet`, SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`, was used for V22/V23/V24/V25/V26 and is the intended V27 boot kernel. No kernel has been flashed.
- Android, bootloaders, recovery, modem, persist, backups, `userdata` and the partition table were untouched by the V22–V26 image swaps. No assistant boot/kexec/flash/partition change has happened.

## Short version history

- V5: first real GPU `clear PASS rgba=00ff00ff`; proof gate only, not final UI.
- V14–V17: EAGL injection, thread-local context, client-pixel patch; V17 best early UI but slow with export errors.
- V18: fake sync-file export removed `export failed` but caused 335 `CREATE_BO failed` and instability. Not a win.
- V19: colour fix; normal colours except icons, but BO pool exhaustion and respring.
- V20: kernel page-table reclaim + MAP_BO rollback; no observed reclaim path, 183 `CREATE_BO failed`, respring, slow.
- V21: BO/VA limits, Mesa NULL-deref fix, single-readback copy-out; zero `CREATE_BO failed`, stable UI, still slow, icons/red wallpaper wrong.
- V22: selective flush + EGLImage surface sharing + content swizzle + real syncobj transfer. Booted: much faster and stable, zero `export failed`/`CREATE_BO failed`, icons nearly correct but blue/yellow swapped. Not a final win.
- V23: userland fix built, deployed and booted; App Library ghosting fixed and mean frame improved to 383 ms, but blue/yellow and red/blue icon swaps remained, Clock sub-icons stayed missing, expanded folders showed swapped icons plus a top black bar. EGLImage sharing still zero.
- V24: source commit `62b6668`; page-align EGLImage import sizes and normalise EAGL 8-bit four-channel uploads/readback to RGBA with explicit IOSurface-order swizzling. Local checks and CI `45623900771` PASS; artifacts/image verified; user completed typed swap and booted. Log shows stable selective flush but `EGL_BAD_PARAMETER` imports, unchanged icon bytes, returned App Library ghosting, missing Clock sub-icons and large copy-outs. Not a win.
- V25: source/docs committed, CI green, artifacts verified; restore V23 deep/copy-out path, implicit dma-buf modifiers, native BGRA upload for BGRA clients/targets, all format-0 texImage pictures BGRA, and London timezone default. First image panicked in `tzinit`; corrected image `cbb8c3a9...e56abf` booted stably but showed no visual/speed win, still had ghosting/missing Settings/Clock icons/wrong colours, and had a 1970 wall clock. Root causes found afterwards: Mesa was built without `HAVE_LIBDRM` so EGLImage imports were compiled out, and no launchd job called `settimeofday()`.
- V26: restore V23 colour/upload path, force `HAVE_LIBDRM` for dma-buf EGLImage import, and restore wall clock early through `/usr/local/etc/sweet_epoch` + malinop. Four build/header/shim fixes after CI failures; run `45677583215` green. Built, swapped and booted: best speed so far (median 198 ms), display EGLImage imports work, stable, but clock 39 min behind and colour/sub-icons/black bar unchanged. Not final.
- V27 (prepared, not built/booted): rebuild malinop to read `sweet_epoch=` from bootargs and add shader/census boot-arg diagnostics, with no Mesa/kernel/image-path change. Source `2208185`, userland CI `45688962279` green, malinop SHA-256 `6c2f0545...3fdca5`.

## Log triage checklist

```sh
grep -n 'pool exhausted\|bo map failed\|CREATE_BO failed\|export failed\|panfrost_batch_init failed' LOG
grep -n 'EXC_CORPSE_NOTIFY\|crash: backboardd\|CRASH backboardd\|respring' LOG
grep -n 'S21EAGL-kernel\|EAGLContext\|hakios-gl: frame\|SWEET FB: swap\|submit result' LOG
grep -n 'S21DRM: submit .* mode=\|shared IOSurface\|not importable\|import_user failed\|wrapped surface .* byte order\|large client picture' LOG
grep -n 'SWEET SURF: pixels\|0 drawn in place\|S21GPU: clear PASS' LOG
grep -n '0x505\|0x506\|glReadPixels' LOG
```

`SWEET SURF: pixels ... -> 0` means `copyin()` error 0 (success), not zero pixels lit.

## Key files and artifacts

- Active handoff: `/home/tom/s21xnu-linux-mesa/NOTES.md`. Second copy kept in sync: `/home/tom/s21xnu-main/NOTES.md`.
- V22 source commits: `f69330b` + build fixes `52010e9`, `2ddd129`; notes include the completed push/swap and boot result.
- V23 source fixes: `519ada5`, `fc69974`; CI `45612732388` success; artifacts `/home/tom/Downloads/ios-mesa-g78-v23/`. V23 was installed and booted by the user; active root hash was not recorded. Boot log: `/home/tom/Downloads/udp-sniff-20261125-130800.txt`.
- V24 source commit: `62b6668`; CI `45623900771` success on head `4a5eefc`. Edit files: `tools/linux-mesa-ios/eagl/src/eagl.c` and `tools/linux-mesa-ios/eagl/src/gl_s21.c`. Verified artifacts: `/home/tom/Downloads/ios-mesa-g78-v24/` (`libs21eagl.dylib` `7215eeb4df1a617fabc17b1b4f073fd08ca421dcc7125b62db99d61a953186be`). Verified image: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v24.img` SHA-256 `db4c532619bef3062b898e441a4112015709d9b8979db82696227dc108ca5f86`; scripts: `/home/tom/Downloads/install-s21eagl-image-v24.sh`, `/home/tom/Downloads/push-and-swap-v24.sh` (`bash -n` PASS).
- V25 commit `116a1c9` edits: `tools/linux-mesa-ios/eagl/src/eagl.c`, `tools/linux-mesa-ios/eagl/src/gl_s21.c`, `tools/s21-root-fixes.py`, plus README/roadmap/handoff. CI run `45640981522`, artifacts `/home/tom/Downloads/ios-mesa-g78-v25/`. First image `c6adcb67...26226f` panicked in `tzinit`; corrected image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v25.img` SHA-256 `cbb8c3a9ec0905c3c854eff839eae75b6d2ae3a58b3a2598de9596ed86e56abf` booted at 17:28. Boot log: `/home/tom/Downloads/udp-sniff-20261125-172800.txt`. Corrected scripts `/home/tom/Downloads/install-s21eagl-image-v25.sh` and `/home/tom/Downloads/push-and-swap-v25.sh` are executable and `bash -n` PASS.
- V26 built/booted image: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v26.img` SHA-256 `30128e5fb583eeb2dd7a9910a75398e3d1636ab21ba72accfd9fd42b0323ef9c`; scripts `/home/tom/Downloads/install-s21eagl-image-v26.sh` and `/home/tom/Downloads/push-and-swap-v26.sh`. Verified artifacts: `/home/tom/Downloads/ios-mesa-g78-v26/`. Green run `45677583215` on `dc63a31`; failed runs `45668154779`, `45670519474`, `45672602689`, `45674981067`; cancelled `45672292806`. Boot log `/home/tom/Downloads/udp-sniff-20261125-193549.txt`.
- V27 prepared (image not built): `/home/tom/Downloads/install-s21eagl-image-v27.sh` and `/home/tom/Downloads/push-and-swap-v27.sh`, both executable and `bash -n` PASS. Base is verified V26; only `/usr/bin/malinop` is replaced with SHA-256 `6c2f05456e71de9d684e5acfeef265444fb2b108ace37bc7f38a25b41c3fdca5` from `/home/tom/Downloads/ios-tools-v27/`. Source commit `2208185`; userland CI `45688962279` green. `/home/tom/Downloads/boot-hojun-gpu.sh` now appends `sweet_epoch=$(date +%s)` plus `glshaders=1 glcensus=1` (override with `DIAG_ARGS=0`).
- V22 CI: xnu `45596778651` success; linux-mesa-ios `45600557745` success. Failed attempts: `45596778741`, `45598612838`.
- V22 boot log: `/home/tom/Downloads/udp-sniff-20261125-115921.txt`.
- V22 artifacts: kernel `/home/tom/Downloads/kernel-v22/kernel.development.sweet` SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`; userland `/home/tom/Downloads/ios-mesa-g78-v22/`.
- V21 source commit: `c6cc351`; CI xnu `45577164674`, linux-mesa-ios `45577164974`.
- V21 log: `/home/tom/Downloads/udp-sniff-20261125-093638.txt`.
- V21 kernel artifact: `/home/tom/Downloads/kernel-v21/kernel.development.sweet`.
- V21 userland artifact: `/home/tom/Downloads/ios-mesa-g78-v21/`.
- V21 image/push scripts: `/home/tom/Downloads/install-s21eagl-image-v21.sh`, `/home/tom/Downloads/push-and-swap-v21.sh`.
- V22 host image: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v22.img`; scripts: `/home/tom/Downloads/install-s21eagl-image-v22.sh`, `/home/tom/Downloads/push-and-swap-v22.sh`.
- V23 host image/scripts: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v23.img` SHA-256 `d97a73a297db0c51ccff0c6592050cbc408fccc1445f54a22a6bc2a28e0b558f`, `/home/tom/Downloads/install-s21eagl-image-v23.sh`, `/home/tom/Downloads/push-and-swap-v23.sh`; installed by the user, on-device active hash not read back.
- Boot script template: `/home/tom/Downloads/boot-hojun-gpu.sh`.
- V22 edit files: `tools/linux-mesa-ios/eagl/src/eagl.c`, `tools/linux-mesa-ios/eagl/src/gl_s21.c`, `tools/linux-mesa-ios/patch_mesa_ios.py`, `tools/linux-mesa-ios/shim/{s21_drm.c,s21_drm.h,s21_iokit.h,s21_iokit_darwin.c,s21_iokit_host.c,host_test.c}`.
- Kernel selector 11 ABI reference: `xnu/patches/0112-s21-mali-iokit.sh` (unchanged).
