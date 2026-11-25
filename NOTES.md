# iOS on Galaxy S21 (sweet) — Mesa/Panfrost GPU handoff

Fresh concise handoff, 2026-11-24. The old ~450 KB chronology was removed from the working tree; it is still recoverable in the git repo as `git show 9fe118f:NOTES.md`. Keep this file short: it is the active handoff for the next agent.

## Goal and proof bar

- Goal: injected `backboardd` CoreAnimation/EAGL renders the iOS UI through Mesa 26.2.4 Panfrost on Mali-G78.
- Proof requires all of:
  - GPU `glReadPixels()` PASS, not a CPU-painted colour square.
  - EAGL markers from the injected `backboardd`.
  - No crashed/dead injected `backboardd` and no respring spiral.
  - Correct colours, especially app icons and wallpaper.
  - Usable speed and a visibly correct UI.
- V20 targets the failure spiral/respring and BO-pool exhaustion. It is not the zero-copy/smoothness fix.
- Smooth UI needs a later zero-copy/GPU blit into the IOSurface; current path still copies pixels out on CPU.

## Hard rules

- Work only on `testing-mesa-port` in `/home/tom/s21xnu-linux-mesa`. Never touch `main`.
- Never reuse deleted commit `7cfe4f6cf72c8a6910c060a0fcb4825740e3256d`.
- Assistant must not boot, kexec, flash, repartition or overwrite the active image without explicit user approval.
- User runs the sniffer first, then boots only:
  - `cd /home/tom/Downloads && sudo ./sweet-usbnet-sniff.sh`
  - `cd /home/tom/Downloads && KERNEL_SRC=<kernel-file> EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh`
  - `KERNEL_SRC` must be the kernel file, not a directory.
- Preserve bootloaders, recovery, modem, persist, backups and the Android install. iOS root stays in the existing `userdata` image; no GPT changes. Confirm every destructive step.
- `SWEETMETAL=1` keeps Metal off so CoreAnimation uses GLES. `s21eagl=0` disables EAGL injection.
- Keep untracked `s20/` as a read-only reference. Do not commit it. Do not copy `ca_hooks.c`.
- User prefers one combined script/attempt instead of several separate boots.

## Current state: V21 real fixes ready, CI pending

- Branch: `testing-mesa-port`; head is the V21 source commit, with a notes-only follow-up expected. Keep `s20/` untracked.
- V20 source commit: `dda812d`; deployed and booted, no win (slow, respring, mixed pinky icons).
- V20 log: `/home/tom/Downloads/udp-sniff-20261124-215027.txt` (69,846 lines).
- V21 is a fix attempt, not a diagnostic run: raise BO limits, stop the observed NULL deref, reduce readback cost, and fix VA/accounting exhaustion. No V21 device image, push, swap or boot is approved yet.

### V20 changes

- `xnu/patches/0111-s21-mali-core.sh`
  - Logs `pool exhausted: used=%u of %u free=%u`.
  - Reclaims empty L3/L2/L1 page tables in `sweet_mali_ctx_unmap()` as soon as their last PTE is gone.
- `xnu/patches/0112-s21-mali-iokit.sh`
  - Logs `bo map failed h=... mapped=...` so kernel-side failure is visible.
- `tools/linux-mesa-ios/shim/s21_drm.c`
  - On failed `MAP_BO`, rolls back all kernel BO chunks created for that call and frees the VA slot.
- `tools/linux-mesa-ios/shim/s21_iokit_host.c`, `s21_iokit.h`, `host_test.c`
  - Adds `s21_host_fail_map()` injection and a host regression test for the rollback.
- `libs21eagl.dylib` has no V20 source change; only `libs21drm.dylib` is expected to differ from V19. V20 needs both a new kernel and the rebuilt userland artifact.

### V20 local checks already done

- `git diff --check`: PASS.
- `bash -n` on both changed XNU patch scripts: PASS.
- `CC=cc MESA_DIR=/tmp/opencode/mesa-26.2.4 tools/linux-mesa-ios/shim/build-host-test.sh` → `s21-drm-host-test: PASS`.

### V20 CI (correct repo: `jimmycarles9991-glitch/s21xnu`)

| Run | Workflow | Head | Result |
| --- | --- | --- | --- |
| `45485926430` | xnu | `dda812d` | **success**, completed `2026-11-24T21:11:35Z` |
| `45486048108` | linux-mesa-ios | `6b90a11` (same source as `dda812d`) | **success**, completed `2026-11-24T21:08:01Z` |

### Verified artifacts

- Kernel artifact downloaded to `/home/tom/Downloads/kernel-v20/`; `sha256sum -c SHA256SUMS` PASS.
- Kernel `/home/tom/Downloads/kernel-v20/kernel.development.sweet`:
  - SHA-256 `8aed3bbc61ffdf095f24e4ba3bc7bc31358472a6479172aa6d8bf8ebc53382fa`
  - Mach-O 64-bit arm64.
  - Strings contain `pool exhausted: used=%u of %u free=%u` and `bo map failed h=%llu va=%llx pages=%llu mapped=%u ctx=%llu pa=%llx`.
- Userland artifact downloaded to `/home/tom/Downloads/ios-mesa-g78-v20/`; local `SHA256SUMS` created and all 8 files verify PASS; all are Mach-O arm64.
- V20 changed libs vs V19:
  - `libs21drm.dylib` `a4fc64c27945a2da849ee99a588e3201c2716c08fbb61f39fa455b9984936c9a`
  - `libs21eagl.dylib` `31c6876b923c3b335ea3221010a1e43f0e6ae5b68a54e8d1d39b563c3e4da81c`
- `libs21eagl.dylib` has no V20 source change (`git diff a895199..dda812d -- tools/linux-mesa-ios/eagl` is empty), but its CI hash differs from V19 `cda18228…c12e0a`. Treat that as rebuild nondeterminism; **do not install it**. The V20 image script keeps V19 `libs21eagl.dylib`.

### V20 deployment (approved and completed 2026-11-24)

- User approved full deployment. Host image build was run by the user because this shell has no interactive sudo password; it printed `V20_IMAGE_READY`.
- Host V20 image `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v20.img`: SHA-256 `bd34f2f44e5cb9f574a162764ec80ff93d925f40a854011c6c3043525d61e48f`, 17,179,869,184 B, `root:root 644`; pre/post `fsck.hfsplus` both say `The volume iOSRoot appears to be OK`.
- The image script replaced only `libs21drm.dylib` and asserted that V19 `libs21eagl.dylib` remained in place.
- Approved push/swap `/home/tom/Downloads/push-and-swap-v20.sh` ran with typed `SWAP_V20` (log `/tmp/opencode/v20-swap.log`). SSH preflight verified Arch Linux, active V19, 31G free.
- rsync to `/mnt/android-data/linux/iosroot-v20.stage.img`: 17,179,869,184 B at 23.55 MB/s in 11m35s; stage hash verified.
- Device swap removed active V19 (host copy retained), ran F2FS GC, then wrote sequentially with `dd bs=64M conv=fsync` in 190.609 s at 90.1 MB/s. Final active root verified: SHA-256 `bd34f2f4…15e48f`, `root:root 644`, `extents=3272`, `free_segments=10564`, tunables `2048`/`0`, 31G free. Printed `ACTIVE_V20_READY`.
- No boot/kexec/flash/partition change by assistant. Device is still in Arch Linux.

### V20 boot result (user report + log analysis)

- User: “still jst as slow, and spring rebooted still. also some icons render fine and some are pinky like and shit.”
- GPU gate PASS: clear `706`, textured quad `773`, blended quad `803`. EAGL injection: pid 58 line `424`, pid 385 line `28160`, pid 465 line `58681`; 8/8 EAGL methods replaced.
- V20’s specific failure paths did **not** fire: `pool exhausted` 0, `bo map failed` 0, `S21DRM: map failed … rolling back` 0. Do not claim the V19 page-table/MAP_BO root cause was observed in V20.
- Persistent failures: `CREATE_BO failed (err=5)` 183, `export failed` 98, `panfrost_batch_init failed` 55, `0x505` 30, `0x506` 0, `0 drawn in place` 130.
- `err=5` remains generic. `s21_iokit_scalar()` maps every IOKit failure to `-EIO`, and the shim returns `-EIO` for a failed create or bad output. At the first failure, reconstructed shim state was 248 live slots, 247 MiB/512 MiB total, 8 free slots and a 142 MiB largest VA hole, so the obvious shim `C.total`/slot/VA early returns do not explain it. The kernel `kSMCreateBO` failure paths log nothing; kernel slot/total/MD failures are still indistinguishable. Imports also consume kernel `fBo`/`sm_total` without a `bo create` log, so log reconstruction alone cannot settle it.
- Crash is downstream of BO failure. Symbolicated:
  - `libgallium-26.2.4.dylib+0x6f0868` = `_panfrost_ptr_map+0x300`; it dereferences a NULL returned by `panfrost_resource_create_with_modifier()` in `pan_resource_modifier_convert()` (`pan_resource.c:1837-1841`).
  - Caller chain: `libs21eagl.dylib+0x5defc` `_target_to_surface+0x638` ← `+0x5d498` `_frame_end+0x4e8` ← `+0x2a668` `_my_glFinishObjectAPPLE+0xe8`.
  - So failed BO creation during copy-out leads directly to the NULL deref/respring; it is not an independent EAGL crash.
- Respring timeline: backboardd pid 58 SIGSEGV line `27607`, SpringBoard SIGKILL/respawn `28202`; pid 385 SIGSEGV `58381`, SpringBoard SIGKILL `58717`. pid 465 launched `58681` and was still submitting at the end of the log (no later crash marker).
- Speed (88 full `|backboardd| hakios-gl: frame` summaries, frames 1–40 across three launches): mean 1.52 s/frame, median 1.09 s, range 0.17–5.29 s. Copy-out mean 339 ms (39% of frame time); IOSurface pre-time mean 242 ms, max 2.08 s. No zero-copy (`0 drawn in place` 130). Per-launch means: 1.37 s, 1.62 s, 1.73 s.
- Presents happened: 16 `SWEET FB: swap`, 2795 `submit result`; colour/icons remain unresolved.

### V21 changes (real fixes)

- `xnu/patches/0112-s21-mali-iokit.sh`
  - `SM_MAX_BO` 256 → 512: V20’s per-client `fBo` table filled with live BOs.
  - `SM_MAX_TOTAL` 512 MiB → 768 MiB: global `sm_total` includes `CREATE_BO` and `IMPORT_USER`; 768 MiB still leaves page-table headroom in the 512-page GPU pool.
- `tools/linux-mesa-ios/shim/s21_drm.c`
  - Matches 512 BO slots / 768 MiB total, 128 chunks.
  - `C.total` is now checked under `C.lock`; concurrent GL threads can no longer pass a stale unlocked value.
  - `s21_drm_import_user()` also charges `C.total`, so imported IOSurfaces share the same budget as created BOs.
  - VA allocator is best-fit instead of first-fit: small BOs no longer split the large hole needed for the next tiler heap.
- `tools/linux-mesa-ios/patch_mesa_ios.py`
  - Patches `pan_resource.c`: if `pan_alloc_staging()` returns NULL, return a failed transfer instead of dereferencing NULL. This removes the symbolicated `_panfrost_ptr_map+0x300` crash/respring.
- `tools/linux-mesa-ios/eagl/src/eagl.c`
  - Inverted copy-out uses one full `glReadPixels()` into a bounce buffer plus a CPU row flip; the old per-row readback remains only as a malloc-failure fallback. Targets the measured 339 ms copy-out.
- `tools/linux-mesa-ios/shim/s21_iokit_host.c`, `host_test.c`
  - Host model now has 512 BO slots; tests cover 300 live BOs and best-fit VA reuse.

### V21 CI dispatched

| Run | Workflow | Head | Status |
| --- | --- | --- | --- |
| `45577164674` | xnu | `bdef6e2` | dispatched `2026-11-25` |
| `45577164974` | linux-mesa-ios | `bdef6e2` | dispatched `2026-11-25` |

### V21 local checks already done

- `git diff --check`: PASS.
- `bash -n` on `0112-s21-mali-iokit.sh`: PASS.
- `python3 -m py_compile patch_mesa_ios.py`: PASS.
- `python3 patch_mesa_ios.py /tmp/opencode/mesa-v21` + `patch_mesa_g78.py`: PASS.
- `CC=cc MESA_DIR=/tmp/opencode/mesa-v21 tools/linux-mesa-ios/shim/build-host-test.sh` → `s21-drm-host-test: PASS`.

### Next move

1. Commit and push V21 source, then dispatch xnu and linux-mesa-ios CI.
2. Download and verify V21 kernel + userland artifacts when green.
3. Prepare one combined image script replacing `libgallium-26.2.4.dylib`, `libs21drm.dylib` and `libs21eagl.dylib`; keep the V21 kernel separate for the boot command.
4. Ask explicitly before any V21 image build/push/swap. User runs sniffer first, then boots once with the V21 kernel file.
5. Analyse V21 log for `CREATE_BO failed`, respring, EAGL markers, frame timings, icons and wallpaper. Then do zero-copy/GPU blit for smoothness if needed.

## Active device state (V20 root and kernel booted by user)

- Active root image: `/mnt/android-data/linux/iosroot.img` on device.
- SHA-256: `bd34f2f44e5cb9f574a162764ec80ff93d925f40a854011c6c3043525d61e48f`.
- Size: `17,179,869,184` B; mode/owner `644 root:root`; `extents=3272` (≤8192 boot cap).
- Host copy: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v20.img`, same SHA-256.
- Kernel file for the next boot: `/home/tom/Downloads/kernel-v20/kernel.development.sweet`, SHA-256 `8aed3bbc61ffdf095f24e4ba3bc7bc31358472a6479172aa6d8bf8ebc53382fa`.
- Last booted kernel is the V20 file above (`8aed3bbc…82fa`), supplied to the user's temporary `fastboot boot`; no kernel has been flashed.
- Device V19 root was deleted during the V20 swap; host V19 and V18 images are retained under `/home/tom/ios-sweet/build`. There is no ready on-device rollback image.
- Device was booted into iOS by the user. SSH/Android will be unavailable until the user returns to Arch/Android.
- No assistant boot/kexec/flash/partition change has happened.

Last user-only boot command (sniffer first) was:

```sh
cd /home/tom/Downloads && sudo ./sweet-usbnet-sniff.sh
cd /home/tom/Downloads && KERNEL_SRC=/home/tom/Downloads/kernel-v20/kernel.development.sweet EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh
```

## V19 boot result and root cause

- Log: `/home/tom/Downloads/udp-sniff-20261124-204125.txt` (37,977 lines).
- User: “colors normal!!!! apart from app icons, and still sloowwww and still resprining”; later “wen rendered wallpaper had some red bright at top or smth fyi”.
- GPU gate PASS at lines 833/899/930/932/934.
- Injected `backboardd`: pid 58 at line 555, pid 384 at 28776; 8/8 EAGL methods replaced; ES3 context created.
- 118 `hakios-gl: frame`, 16 `SWEET FB: swap`, 1440 `submit result`; no explicit `crash: backboardd`/`CRASH backboardd`.
- pid 58 gets `EXC_CORPSE_NOTIFY` at line 28771 after mass `bo free`; no crash cause logged.
- Failure chain: 588 `CREATE_BO failed (err=5)`, 80 `export failed`, 114 `panfrost_batch_init failed`, 56 `0x505`, 36 `0x506`, 90 `0 drawn in place`.
- `err=5` is the shim's generic `-EIO`; real kernel cause was hidden.
- Root cause: GPU page-table pool exhaustion makes `MAP_BO` return `kIOReturnNoMemory`; the shim left the already-created kernel BO in a slot on failure, leaking handles until the 256-slot cap. Distinct L3 leaves reach 128 at first failure and 163 by end of the 512-page pool.
- Speed: 59 frame summaries average ~1.23 s/frame (~0.8 fps). Calm frames ~0.17–0.34 s (3–6 fps). Copy-out ~113 ms. Worst frames 3–6 s correlate with 1400–1500 GL calls and 2.4–2.8 s IOSurface upload time.
- V19 overall colours are a real win; icon colours and wallpaper red-bright-top are separate unresolved issues.

## Short version history

- V5: first real GPU `clear PASS rgba=00ff00ff` and full-screen green paint. Good gate proof, not final UI proof.
- V7/V9/V10: textured/blended quad readback failed; GPU VA faults investigated.
- V11: added BO/submit/page-table diagnostics; required both new userland and kernel.
- V14/V15: EAGL injection and thread-local context fixes; `IOSurface` wrap-refuse crash fixed.
- V16: first two injected `backboardd` instances still died after presenting frames; BO/GL OOM churn.
- V17: combined attempt worked best so far: injection, EAGL, frames and presents; icon pixels fixed by patch 0147; 95 `export failed` remained.
- V18: syncobj export/import fix removed `export failed` but introduced 335 `CREATE_BO failed` plus restarts. Not a win.
- V19: userland-only colour fix; normal colours except icons, but BO pool exhaustion and respring. Deployed now.
- V20: kernel page-table reclaim + shim MAP_BO rollback. Deployed and booted; no observed reclaim/map-failure path, 183 `CREATE_BO failed`, respring and slowness remain. Not a win.

## Log triage checklist

For any new boot log, count/grep:

```sh
grep -n 'pool exhausted\|bo map failed\|CREATE_BO failed\|export failed\|panfrost_batch_init failed' LOG
grep -n 'EXC_CORPSE_NOTIFY\|crash: backboardd\|CRASH backboardd' LOG
grep -n 'S21EAGL-kernel\|EAGLContext\|hakios-gl: frame\|SWEET FB: swap\|submit result' LOG
grep -n 'SWEET SURF: pixels\|0 drawn in place\|S21GPU: clear PASS' LOG
grep -n '0x505\|0x506\|glReadPixels' LOG
```

Good boot: GPU PASS, EAGL markers, presents, no dead injected `backboardd`, no respring, colours correct. `SWEET SURF: pixels ... -> 0` means `copyin()` error 0 (success), not zero pixels lit.

## Key files and artifacts

- Active handoff: `/home/tom/s21xnu-linux-mesa/NOTES.md`.
- Roadmap: `/home/tom/s21xnu-linux-mesa/ROADMAP-linux-mesa-port.md`.
- V20 kernel patches: `xnu/patches/0111-s21-mali-core.sh`, `xnu/patches/0112-s21-mali-iokit.sh`.
- V20 userland rollback: `tools/linux-mesa-ios/shim/s21_drm.c`, `tools/linux-mesa-ios/shim/host_test.c`.
- EAGL/colour paths: `tools/linux-mesa-ios/eagl/src/eagl.c`, `tools/linux-mesa-ios/eagl/src/gl_s21.c`.
- IOSurface client-pixel patch: `xnu/patches/0147-s21-iosurface-format-and-client-pixels.patch`.
- V19 artifact: `/home/tom/Downloads/ios-mesa-g78-v19/`; `sha256sum -c SHA256SUMS` PASS; all 8 files Mach-O arm64.
- V20 artifact: `/home/tom/Downloads/ios-mesa-g78-v20/`; local `SHA256SUMS` PASS; all 8 files Mach-O arm64.
- V20 kernel: `/home/tom/Downloads/kernel-v20/kernel.development.sweet`, SHA-256 `8aed3bbc61ffdf095f24e4ba3bc7bc31358472a6479172aa6d8bf8ebc53382fa`.
- V19 changed libs vs V18:
  - `libs21drm.dylib` `f3ba031a774340cdff051cc7b55753b8ef4901844fe177b280a5846b5620afac`
  - `libs21eagl.dylib` `cda1822897047b630612c6b16e285e617a69d1aa234633d87be82303f8c12e0a`
- V20 changed libs vs V19:
  - `libs21drm.dylib` `a4fc64c27945a2da849ee99a588e3201c2716c08fbb61f39fa455b9984936c9a`
  - `libs21eagl.dylib` `31c6876b923c3b335ea3221010a1e43f0e6ae5b68a54e8d1d39b563c3e4da81c` (rebuild-only; do not install)
- V19 install/swap scripts: `/home/tom/Downloads/install-s21eagl-image-v19.sh`, `/home/tom/Downloads/push-and-swap-v19.sh`.
- V20 install/swap scripts (executed): `/home/tom/Downloads/install-s21eagl-image-v20.sh`, `/home/tom/Downloads/push-and-swap-v20.sh`; both `bash -n` PASS; swap log `/tmp/opencode/v20-swap.log`.
- Boot script template: `/home/tom/Downloads/boot-hojun-gpu.sh`.
- V19 boot log: `/home/tom/Downloads/udp-sniff-20261124-204125.txt`.
- V20 boot log: `/home/tom/Downloads/udp-sniff-20261124-215027.txt`.

## Unresolved / to verify after V21

- Verify whether the raised BO/VA limits remove `CREATE_BO failed (err=5)` and `export failed`; do not assume until the V21 log is analysed.
- Verify the Mesa NULL-deref fix removes the `_panfrost_ptr_map+0x300` SIGSEGV and the SpringBoard respring spiral.
- Verify the single-readback copy-out reduces the 339 ms mean; real smoothness still needs GPU blit/zero-copy, target 10–20 fps first.
- App icons still wrong; mixed fine/pinky icons after V20. Likely swizzle/plane order or alpha handling, not missing pixels.
- Wallpaper red-bright-at-top is undiagnosed.
