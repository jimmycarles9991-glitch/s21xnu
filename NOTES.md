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
- V22 source is complete, pushed and locally checked. V22 CI is **running**:
  - xnu run `45596778651`
  - linux-mesa-ios run `45596778741`
  - head `d0ba852` on `testing-mesa-port`.
- **No V22 artifacts, image, push, swap or boot has happened yet.** No V22 deployment is approved. Ask before any device write.
- Kernel patch is unchanged for V22: the V21 kernel already implements selector 11 (`kSMSubmitList`). A V22 boot can reuse `/home/tom/Downloads/kernel-v21/kernel.development.sweet`; userland must be rebuilt from V22 source.

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

1. Push `testing-mesa-port`: done at head `d0ba852`.
2. Wait for xnu `45596778651` and linux-mesa-ios `45596778741`; download artifacts and verify `SHA256SUMS` + Mach-O arm64.
3. Ask explicit user approval before any V22 image build, push, swap, boot or active-image overwrite.
4. If approved and device is back in Arch/Android: user runs sniffer first, then boots once with the V22 kernel/userland using the user-only command above.
5. Analyse V22 log for `mode=list`, `mode=full`, `shared IOSurface`, `wrapped surface ... byte order`, `export failed`, `CREATE_BO failed`, respring, EAGL markers, frame/copy-out times, icons, wallpaper and visibly correct UI.

## Active device state

- Active root image on device remains the V21 image; host copy `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v21.img`, SHA-256 `0c322bf05756a21444f8ffe59e92082454c11a3c743b6a947f4a0e14021856cc`.
- V21 kernel file: `/home/tom/Downloads/kernel-v21/kernel.development.sweet`, SHA-256 `0bc9d5c4c73d146efaa362c363c48cdcdfae4270b244b7d0ee53406a005c8fac`. It contains selector 11.
- No kernel has been flashed. No V22 image or artifact has touched the device.
- After the V21 boot, SSH/Android may be unavailable until the user returns to Arch/Android.
- No assistant boot/kexec/flash/partition change has happened.

## Short version history

- V5: first real GPU `clear PASS rgba=00ff00ff`; proof gate only, not final UI.
- V14–V17: EAGL injection, thread-local context, client-pixel patch; V17 best early UI but slow with export errors.
- V18: fake sync-file export removed `export failed` but caused 335 `CREATE_BO failed` and instability. Not a win.
- V19: colour fix; normal colours except icons, but BO pool exhaustion and respring.
- V20: kernel page-table reclaim + MAP_BO rollback; no observed reclaim path, 183 `CREATE_BO failed`, respring, slow.
- V21: BO/VA limits, Mesa NULL-deref fix, single-readback copy-out; zero `CREATE_BO failed`, stable UI, still slow, icons/red wallpaper wrong.
- V22: selective flush + EGLImage surface sharing + content swizzle + real syncobj transfer. Local checks green; deployment pending.

## Log triage checklist

```sh
grep -n 'pool exhausted\|bo map failed\|CREATE_BO failed\|export failed\|panfrost_batch_init failed' LOG
grep -n 'EXC_CORPSE_NOTIFY\|crash: backboardd\|CRASH backboardd\|respring' LOG
grep -n 'S21EAGL-kernel\|EAGLContext\|hakios-gl: frame\|SWEET FB: swap\|submit result' LOG
grep -n 'S21DRM: submit .* mode=\|shared IOSurface\|wrapped surface .* byte order' LOG
grep -n 'SWEET SURF: pixels\|0 drawn in place\|S21GPU: clear PASS' LOG
grep -n '0x505\|0x506\|glReadPixels' LOG
```

`SWEET SURF: pixels ... -> 0` means `copyin()` error 0 (success), not zero pixels lit.

## Key files and artifacts

- Active handoff: `/home/tom/s21xnu-linux-mesa/NOTES.md`.
- V22 source commit: `f69330b` on `testing-mesa-port`.
- V21 source commit: `c6cc351`; CI xnu `45577164674`, linux-mesa-ios `45577164974`.
- V21 log: `/home/tom/Downloads/udp-sniff-20261125-093638.txt`.
- V21 kernel artifact: `/home/tom/Downloads/kernel-v21/kernel.development.sweet`.
- V21 userland artifact: `/home/tom/Downloads/ios-mesa-g78-v21/`.
- V21 image/push scripts: `/home/tom/Downloads/install-s21eagl-image-v21.sh`, `/home/tom/Downloads/push-and-swap-v21.sh`.
- Boot script template: `/home/tom/Downloads/boot-hojun-gpu.sh`.
- V22 edit files: `tools/linux-mesa-ios/eagl/src/eagl.c`, `tools/linux-mesa-ios/eagl/src/gl_s21.c`, `tools/linux-mesa-ios/patch_mesa_ios.py`, `tools/linux-mesa-ios/shim/{s21_drm.c,s21_drm.h,s21_iokit.h,s21_iokit_darwin.c,s21_iokit_host.c,host_test.c}`.
- Kernel selector 11 ABI reference: `xnu/patches/0112-s21-mali-iokit.sh` (unchanged).
