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
- GPU mostly works: real `glReadPixels()` PASS and injected `backboardd` EAGL UI in V22/V23. Not final: blue↔yellow/red↔blue icon swaps, missing Clock sub-icons, expanded-folder black bar and ~0.3–0.4 s frames.
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

- Branch: `testing-mesa-port`. V22 source `f69330b` + build fixes; V23 source `519ada5` + `fc69974`; notes through `4a5eefc`; docs status pass `459d992`. V24 source is committed/pushed as `62b6668` and described below. The full V24 CI/image/push/swap/boot attempt is approved; the final active-image swap still requires the user's typed confirmation.
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
- V24 source work (commit `62b6668`): page-align EGLImage import sizes and normalise all 8-bit four-channel EAGL textures/readback to RGBA with explicit surface-order swizzling. No kernel change; no speculative clear/sync change for the folder bar. Full attempt approved; linux-mesa-ios CI run `45623900771` on head `4a5eefc` completed **success** in 16m18s. V24 artifacts verified; local image build is waiting on sudo, and no push/swap/boot has happened.

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
- Local V23 base image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v23.img` SHA-256 `d97a73a297db0c51ccff0c6592050cbc408fccc1445f54a22a6bc2a28e0b558f`. V24 image/push scripts prepared at `/home/tom/Downloads/install-s21eagl-image-v24.sh` and `/home/tom/Downloads/push-and-swap-v24.sh`, both `bash -n` PASS. Image build was attempted but blocked because non-interactive `sudo` cannot supply a password; no image was created yet.
- Fix 1 — EGLImage import (`tools/linux-mesa-ios/eagl/src/gl_s21.c`): `importable()` previously required `IOSurfaceGetAllocSize()` to be a 4 KiB multiple. The 1125 x 2436 display surface has row 4608, `row * height = 11,225,088` bytes (`0xab4800`, not page-aligned); rounded size is `11,227,136` (`0xab5000`). V24 rounds up to 4 KiB and logs the first 16 refusals through `import_refused()` with surface id, dimensions, row, needed and alloc sizes. This directly targets V22/V23's zero `shared IOSurface`.
- Fix 2 — byte order (`tools/linux-mesa-ios/eagl/src/eagl.c`): the V23 user report plus logged source bytes fit a red/blue swap: blue/cyan `00 bf ff` appearing yellow `ff bf 00`, and orange `ff 7a 00` appearing blue `00 7a ff`. V24 normalises all 8-bit four-channel uploads to `GL_RGBA`, reads back with `GL_RGBA`, and converts explicitly to the IOSurface's byte order (BGRA normally, ABGR for wrapped opacity-first masks). Deep `b3a8` conversion now uses RGBA internally. Attached BGRA masks are included; no speculative clear/sync change was added for the expanded-folder black bar.
- Proof markers for the next boot: `S21EAGL: shared IOSurface` and/or `S21EAGL: IOSurface ... not importable` should now appear; icon red/blue and blue/yellow swaps should be gone; expanded-folder icons and black bar, Clock sub-icons and speed/stability must be rechecked.
- V24 local checks (all PASS): `git diff --check`; `clang -fsyntax-only -std=gnu11 -w` on both edited EAGL sources with a freshly generated `eagl_gen.h`, Mesa 26.2.4 headers, the shim/include paths and temporary iOS stubs. Host shim test was not rerun because no shim file changed. No `s20/` file was committed.

## Next move

1. V24 source `62b6668` is pushed and CI `45623900771` is green. The full image/push/swap/boot attempt is approved; do not ask for another general approval, but still require the user's typed confirmation before overwriting the active image.
2. Image build is the current blocker: run `sudo /home/tom/Downloads/install-s21eagl-image-v24.sh` in a terminal that can answer the sudo password prompt. It must print `V24_IMAGE_READY` and write `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v24.img.sha256`.
3. Wait until the device is back in EndeavourOS/Android with SSH. The prepared `/home/tom/Downloads/push-and-swap-v24.sh` asserts the active hash is the verified V23 `d97a73a2...b558f` before it asks for typed `SWAP_V24`; if it reports a different active hash, stop and record that hash instead of forcing a swap.
4. User runs the sniffer first, then boots once with the unchanged V22 kernel:
   - `cd /home/tom/Downloads && sudo ./sweet-usbnet-sniff.sh`
   - `cd /home/tom/Downloads && KERNEL_SRC=/home/tom/Downloads/kernel-v22/kernel.development.sweet EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh`
5. Analyse the new log for `shared IOSurface`, `not importable`, icon red/blue and blue/yellow swaps, Clock/World Clock/Alarm/Stopwatch/Timer sub-icons, expanded-folder icons and black bar, and frame/copy-out speed. Confirm `mode=list`, zero `export failed`/`CREATE_BO failed`/respring and 8/8 EAGL markers have not regressed.

## Active device state

- V23 was booted by the user at 2026-11-25 13:08; the active root is expected to be the V23 userland image. The local V23 host image hash is `d97a73a297db0c51ccff0c6592050cbc408fccc1445f54a22a6bc2a28e0b558f`, but the on-device active hash has still not been read back. The last older verified root hash remains V22 `8cf044022932213676245e5320533f6c74779a5edf6aae760f241e650646320b`.
- The V22 kernel file `/home/tom/Downloads/kernel-v22/kernel.development.sweet`, SHA-256 `1607a49d53ea12c26f5a7b8b28444f39a07774424fb8690beb648fed9c0da8cd`, was used for V22 and V23 and is the intended V24 boot kernel. No kernel has been flashed.
- Android/SSH check at the 2026-11-25 docs update timed out; assume the device is still in iOS or unreachable. Android, bootloaders, recovery, modem, persist and backups were untouched by the V22/V23 swaps.
- V24 has source plus a green CI attempt and prepared scripts, but no image, push/swap or boot yet. Non-interactive sudo blocked the local image build. No assistant boot/kexec/flash/partition change has happened.

## Short version history

- V5: first real GPU `clear PASS rgba=00ff00ff`; proof gate only, not final UI.
- V14–V17: EAGL injection, thread-local context, client-pixel patch; V17 best early UI but slow with export errors.
- V18: fake sync-file export removed `export failed` but caused 335 `CREATE_BO failed` and instability. Not a win.
- V19: colour fix; normal colours except icons, but BO pool exhaustion and respring.
- V20: kernel page-table reclaim + MAP_BO rollback; no observed reclaim path, 183 `CREATE_BO failed`, respring, slow.
- V21: BO/VA limits, Mesa NULL-deref fix, single-readback copy-out; zero `CREATE_BO failed`, stable UI, still slow, icons/red wallpaper wrong.
- V22: selective flush + EGLImage surface sharing + content swizzle + real syncobj transfer. Booted: much faster and stable, zero `export failed`/`CREATE_BO failed`, icons nearly correct but blue/yellow swapped. Not a final win.
- V23: userland fix built, deployed and booted; App Library ghosting fixed and mean frame improved to 383 ms, but blue/yellow and red/blue icon swaps remained, Clock sub-icons stayed missing, expanded folders showed swapped icons plus a top black bar. EGLImage sharing still zero.
- V24: source commit `62b6668`; page-align EGLImage import sizes and normalise EAGL 8-bit four-channel uploads/readback to RGBA with explicit IOSurface-order swizzling. Local checks and CI `45623900771` PASS; artifacts verified, image build waiting on sudo, not deployed or booted.

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
- V24 source commit: `62b6668`; CI `45623900771` success on head `4a5eefc`. Edit files: `tools/linux-mesa-ios/eagl/src/eagl.c` and `tools/linux-mesa-ios/eagl/src/gl_s21.c`. Verified artifacts: `/home/tom/Downloads/ios-mesa-g78-v24/` (`libs21eagl.dylib` `7215eeb4df1a617fabc17b1b4f073fd08ca421dcc7125b62db99d61a953186be`). Planned image: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v24.img`; scripts: `/home/tom/Downloads/install-s21eagl-image-v24.sh`, `/home/tom/Downloads/push-and-swap-v24.sh` (`bash -n` PASS).
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
