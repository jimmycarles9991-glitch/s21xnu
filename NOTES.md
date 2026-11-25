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
- Do not claim a V22 win until a boot log proves these. V18–V21 were not wins.

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

- Branch: `testing-mesa-port`; V22 source commit `f69330b` (`gpu ios: V22 selective flush, surface import and swizzle fixes`).
- V21 source commit `c6cc351`; V21 CI green, artifacts verified, image built, pushed/swapped and **booted by the user**.
- V21 result: stability improved, but still slow; icon colours still wrong; red lockscreen wallpaper remains. Not a win.
- V22 source is complete, pushed and locally checked.
  - xnu run `45596778651` on head `d0ba852`: **success**, completed `2026-11-25T10:49:34Z`. Kernel artifact downloaded to `/home/tom/Downloads/kernel-v22/`; `SHA256SUMS` PASS; Mach-O arm64; SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`. Same size as V21 (`17,472,176` B) and `xnu/patches/0112-s21-mali-iokit.sh` is byte-identical to V21; binary hash differs only because of rebuild. Selector 11 is present in the V21/V22 source; V21 boot used it successfully via the shim.
  - First linux-mesa-ios run `45596778741` on head `d0ba852`: **failed**; `gl_s21.c` used `<IOSurface/IOSurface.h>`, which the iOS SDK exposes as `<IOSurface/IOSurfaceRef.h>` (same header `eagl.c` already uses).
  - Fix commit `52010e9`; linux-mesa-ios rerun `45598612838` also **failed**: `gl_s21.c` could not find the fake `<xf86drm.h>` because the EAGL compile lacked `-I$HERE/include`.
  - Fix commit `2ddd129` adds that include path; linux-mesa-ios `45600557745` on head `2ddd129`: **success**, completed `2026-11-25T11:11:10Z`. Host test passed inside CI.
- V22 host image built by the user; push/swap executed by the user with typed `SWAP_V22`. User booted V22 on 2026-11-25; log `/home/tom/Downloads/udp-sniff-20261125-115921.txt`. Speed/stability improved and icons are nearly correct, but blue/yellow icons are swapped. Not a final win.
- V22 image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v22.img`: SHA-256 `8cf044022932213676245e5320533f6c74779a5edf6aae760f241e650646320b`, 17,179,869,184 B, `root:root 644`; pre/post `fsck.hfsplus` OK.
- Device swap: rsync 17,179,869,184 B at 23.59 MB/s in 11m34s; stage hash verified; F2FS GC 9982 → 10051 free segments; sequential `dd` in 183 s at 93.9 MB/s; printed `ACTIVE_V22_READY`.
- Independent read-only recheck: active SHA-256 `8cf044022932213676245e5320533f6c74779a5edf6aae760f241e650646320b`, `root:root 644`, `extents=3797`, `free_segments=10050`, tunables `2048`/`0`.
- Scripts used: `/home/tom/Downloads/install-s21eagl-image-v22.sh` and `/home/tom/Downloads/push-and-swap-v22.sh`; both `bash -n` PASS. No assistant boot/kexec/flash/partition change.
- Userland artifact: `/home/tom/Downloads/ios-mesa-g78-v22/`; local `SHA256SUMS` PASS; all 8 files Mach-O arm64.
- V22 changed libs vs V21:
  - `libgallium-26.2.4.dylib` `b68bc18f3e409fb2d4ec6482ce0f7f1d6bc2214200ba77c09ad1e9a002e147a2`
  - `libs21drm.dylib` `7e82ccff8062413c578dea4f665a9cf0319807eefc8c5dc783fe2b84eed26ad3`
  - `libs21eagl.dylib` `cc3104e8728ee7de1dcd4bd1468b5974432dc084e5434756eb6a202aa8ce7f8b`
- Kernel patch is unchanged for V22: the V21 kernel already implements selector 11 (`kSMSubmitList`). A V22 boot can reuse `/home/tom/Downloads/kernel-v21/kernel.development.sweet`; prefer the V22 kernel for traceability. V22 userland is already built and installed in the active image.
- V23 source fix `519ada5` (`eagl: V23 read large format-0 client pictures as BGRA`) is committed and pushed. It forces format-0 surfaces with width or height >= 100 to BGRA, while keeping the opacity sampler for small tinted masks. No V23 CI, artifacts, image, push/swap or boot yet.

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
- Gate/UI markers: EAGL injection pid 58 line `471`; `S21GPU: clear PASS` line `768`; 8/8 EAGL methods replaced lines `954-962`; `SWEET FB: swap` 16; `submit result` 993.
- Selective flush worked: 993/993 submits `mode=list`, zero `mode=full`. Zero `export failed`, `CREATE_BO failed`, `pool exhausted`, `bo map failed`, `panfrost_batch_init failed`, `0x505`, `0x506`. No backboardd/SpringBoard crash and no respring. The 10 `EXC_CORPSE_NOTIFY` entries are other daemons (widget check-in timeouts, `contextstored` non-fatal `EXC_RESOURCE`, etc.).
- EGLImage/PRIME sharing did not engage: zero `shared IOSurface` lines; wrapped surfaces used the CPU upload path. The speed gain came from the submit/fence path, not zero-copy.
- Frame summaries: 40 frames; mean `422,309 us` (V21 `1,109,026 us`), median `360,710 us` (V21 `571,501 us`), min `225,855 us`, max `1,031,407 us` (V21 max `4,983,583 us`). Steady frames 34-38 were `264-270 ms` (~3.7 fps).
- Copy-out: mean `244,995 us` / 58.0% of frame time (V21 `312,143 us` / 28.1%); IOSurface pre-time mean `127,949 us`. Copy-out is now the main remaining cost.
- Colour: first 24 unique wrapped surfaces logged; 18 BGRA and 6 ABGR (36/12 log lines because each message is duplicated). Large 180x180 icons were almost all BGRA; surface 15 hit the 985/985 opacity tie and was classified ABGR (`ff 2a 2e ff`). Small 28x65/28x95/28x28/6x85/6x6 masks were ABGR and should stay ABGR.
- V22 corrected most icon colours versus V21: the same source bytes that V21 logged as `ff 5e 42 fd` and `ff cc c7 c7` became `5e 42 fd ff` and `cc c7 c7 ff`. The remaining blue/yellow swap is consistent with the opacity heuristic tying on saturated icons. V23 forces format-0 surfaces with width or height >= 100 to BGRA and keeps the sampler for small masks.

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

## Next move

1. V23 source fix `519ada5` is committed and pushed. V22 kernel/userland/image remains active and booted; no V23 CI, artifacts, image, push/swap or boot yet.
2. Ask explicit approval for V23 CI, artifacts, image, push/swap and boot. The change is userland-only; the V22 kernel can be reused.
3. If approved: run linux-mesa-ios CI only (xnu unchanged), verify userland artifacts, build V23 image from active V22, then prepare a typed-confirmation push/swap script. User runs sniffer first, then boots with the V22 kernel.
4. Analyse the V23 log for `BGRA (large client picture)`, icon middle bytes, blue/yellow icons, and confirm V22's speed/stability markers have not regressed.

## Active device state

- V22 was booted by the user at 2026-11-25 11:59; active root remains V22 SHA-256 `8cf044022932213676245e5320533f6c74779a5edf6aae760f241e650646320b`. The boot log shows iOS `backboardd`/SpringBoard running. Current SSH/Android state was not rechecked after boot; assume unavailable until the user boots back to EndeavourOS.
- V22 kernel file `/home/tom/Downloads/kernel-v22/kernel.development.sweet`, SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`, was used for the boot. V21 kernel SHA-256 `0bc9d5c4c73d146efaa362c363c48cdcdfae4270b244b7d0ee53406a005c8fac` also has selector 11; V22 kernel is preferred for traceability.
- No kernel has been flashed. The active image was overwritten only after the user typed `SWAP_V22`; Android, bootloaders, recovery, modem, persist and backups were untouched. V23 is source-only so far and is not on the device.
- No assistant boot/kexec/flash/partition change has happened.

## Short version history

- V5: first real GPU `clear PASS rgba=00ff00ff`; proof gate only, not final UI.
- V14–V17: EAGL injection, thread-local context, client-pixel patch; V17 best early UI but slow with export errors.
- V18: fake sync-file export removed `export failed` but caused 335 `CREATE_BO failed` and instability. Not a win.
- V19: colour fix; normal colours except icons, but BO pool exhaustion and respring.
- V20: kernel page-table reclaim + MAP_BO rollback; no observed reclaim path, 183 `CREATE_BO failed`, respring, slow.
- V21: BO/VA limits, Mesa NULL-deref fix, single-readback copy-out; zero `CREATE_BO failed`, stable UI, still slow, icons/red wallpaper wrong.
- V22: selective flush + EGLImage surface sharing + content swizzle + real syncobj transfer. Booted: much faster and stable, zero `export failed`/`CREATE_BO failed`, icons nearly correct but blue/yellow swapped. Not a final win.
- V23: source fix only; force large format-0 client pictures to BGRA so saturated blue/yellow icons are not misread as ABGR. Committed/pushed, not built or booted.

## Log triage checklist

```sh
grep -n 'pool exhausted\|bo map failed\|CREATE_BO failed\|export failed\|panfrost_batch_init failed' LOG
grep -n 'EXC_CORPSE_NOTIFY\|crash: backboardd\|CRASH backboardd\|respring' LOG
grep -n 'S21EAGL-kernel\|EAGLContext\|hakios-gl: frame\|SWEET FB: swap\|submit result' LOG
grep -n 'S21DRM: submit .* mode=\|shared IOSurface\|wrapped surface .* byte order\|large client picture' LOG
grep -n 'SWEET SURF: pixels\|0 drawn in place\|S21GPU: clear PASS' LOG
grep -n '0x505\|0x506\|glReadPixels' LOG
```

`SWEET SURF: pixels ... -> 0` means `copyin()` error 0 (success), not zero pixels lit.

## Key files and artifacts

- Active handoff: `/home/tom/s21xnu-linux-mesa/NOTES.md`. Second copy kept in sync: `/home/tom/s21xnu-main/NOTES.md`.
- V22 source commits: `f69330b` + build fixes `52010e9`, `2ddd129`; notes include the completed push/swap and boot result.
- V23 source fix: `519ada5`; no V23 CI/artifacts/image/boot yet.
- V22 CI: xnu `45596778651` success; linux-mesa-ios `45600557745` success. Failed attempts: `45596778741`, `45598612838`.
- V22 boot log: `/home/tom/Downloads/udp-sniff-20261125-115921.txt`.
- V22 artifacts: kernel `/home/tom/Downloads/kernel-v22/kernel.development.sweet` SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`; userland `/home/tom/Downloads/ios-mesa-g78-v22/`.
- V21 source commit: `c6cc351`; CI xnu `45577164674`, linux-mesa-ios `45577164974`.
- V21 log: `/home/tom/Downloads/udp-sniff-20261125-093638.txt`.
- V21 kernel artifact: `/home/tom/Downloads/kernel-v21/kernel.development.sweet`.
- V21 userland artifact: `/home/tom/Downloads/ios-mesa-g78-v21/`.
- V21 image/push scripts: `/home/tom/Downloads/install-s21eagl-image-v21.sh`, `/home/tom/Downloads/push-and-swap-v21.sh`.
- V22 host image: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v22.img`; scripts: `/home/tom/Downloads/install-s21eagl-image-v22.sh`, `/home/tom/Downloads/push-and-swap-v22.sh`.
- Boot script template: `/home/tom/Downloads/boot-hojun-gpu.sh`.
- V22 edit files: `tools/linux-mesa-ios/eagl/src/eagl.c`, `tools/linux-mesa-ios/eagl/src/gl_s21.c`, `tools/linux-mesa-ios/patch_mesa_ios.py`, `tools/linux-mesa-ios/shim/{s21_drm.c,s21_drm.h,s21_iokit.h,s21_iokit_darwin.c,s21_iokit_host.c,host_test.c}`.
- Kernel selector 11 ABI reference: `xnu/patches/0112-s21-mali-iokit.sh` (unchanged).
