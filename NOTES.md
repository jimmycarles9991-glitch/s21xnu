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

## Current state: V20 source ready, both CI runs pending

- Branch: `testing-mesa-port`; clean except untracked `s20/`. Check `git log -1` for the current notes-only head.
- V20 source commit: `dda812d` (`gpu ios: V20 pool reclaim and MAP_BO rollback`).
- Notes-only commits after source: `6b90a11`, `9fe118f`, `880aea5` (compacted handoff from 1044 to 161 lines).
- No V20 image, push, swap or boot has happened. No V20 artifacts downloaded yet.

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

| Run | Workflow | Head | Status at last check |
| --- | --- | --- | --- |
| `45485926430` | xnu | `dda812d` | `in_progress`, updated `2026-11-24T20:56:19Z` |
| `45486048108` | linux-mesa-ios | `6b90a11` (same source as `dda812d`) | `in_progress`, updated `2026-11-24T20:57:04Z` |

Check with:

```sh
gh api repos/jimmycarles9991-glitch/s21xnu/actions/runs/45485926430 --jq '[.status,.conclusion,.updated_at] | @tsv'
gh api repos/jimmycarles9991-glitch/s21xnu/actions/runs/45486048108 --jq '[.status,.conclusion,.updated_at] | @tsv'
```

### Next move

1. Wait for both runs to complete green.
2. Download and verify both artifacts (`sha256sum -c SHA256SUMS`, Mach-O arm64 file check).
3. Ask explicit approval before one combined V20 deployment.
4. Prepare one script that stages the new image from V19 and replaces only `libs21drm.dylib`; keep the new kernel as a separate file for the user's boot command.
5. User returns device to Arch/Android SSH if needed, runs sniffer, then boots with the new kernel file.
6. Analyse the new UDP log for `pool exhausted`, `bo map failed`, `CREATE_BO failed`, `export failed`, respring, frame timings, EAGL markers, icons and wallpaper.

Expected good V20 signs: `pool exhausted` and `bo map failed` may appear, but `CREATE_BO failed (err=5)` should collapse, no `EXC_CORPSE_NOTIFY`/respring, EAGL markers still present, UI still renders.

## Active device state (V19 root + V17 kernel)

- Active root image: `/mnt/android-data/linux/iosroot.img` on device.
- SHA-256: `4feaa1d933baaeb7af4f1e8124eaa985113f2c45b42390fd0ab697ed6bcd28ee`.
- Size: `17,179,869,184` B; mode/owner `644 root:root`; `extents=3428` (≤8192 boot cap).
- Host copy: `/home/tom/ios-sweet/build/iosroot-hojun-816f870-eagl-v19.img`, same SHA-256.
- Active booted kernel: `/home/tom/Downloads/kernel-v17/kernel.development.sweet`.
- Kernel SHA-256: `a98e03830ed20ef183f78ac163e987768d626b89d33a56a1f83fd8de582a872b`.
- Device V18 root `bb4447bdc6413b3464cfa62df5208844b15b82bf7999fd41a220364db4f606db` was deleted during the V19 swap. No ready V18 rollback image found under `/home/tom`.
- Last known device state: V19 was booted by the user. SSH/Android is unavailable while in iOS; return to Arch/Android before another push.
- No assistant boot/kexec/flash/partition change has happened.

Current V19 user-only boot command (will change for V20):

```sh
cd /home/tom/Downloads && KERNEL_SRC=/home/tom/Downloads/kernel-v17/kernel.development.sweet EXTRA="launchd_unsecure_cache=1" SWEETMETAL=1 GPUCOMP=1 ./boot-hojun-gpu.sh
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
- V20: kernel page-table reclaim + shim rollback, pending CI. Targets stability, not smoothness.

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
- V19 changed libs vs V18:
  - `libs21drm.dylib` `f3ba031a774340cdff051cc7b55753b8ef4901844fe177b280a5846b5620afac`
  - `libs21eagl.dylib` `cda1822897047b630612c6b16e285e617a69d1aa234633d87be82303f8c12e0a`
- V19 install/swap scripts: `/home/tom/Downloads/install-s21eagl-image-v19.sh`, `/home/tom/Downloads/push-and-swap-v19.sh`.
- Boot script template: `/home/tom/Downloads/boot-hojun-gpu.sh`.
- V19 boot log: `/home/tom/Downloads/udp-sniff-20261124-204125.txt`.

## Unresolved after V20 stability

- App icons still wrong despite all 16 icon copies succeeding and icon textures reporting 31,092/32,400 pixels lit. Likely swizzle/plane order, not missing pixels.
- Wallpaper red-bright-at-top is undiagnosed.
- UI smoothness: current path is ~0.8 fps average with ~113 ms CPU copy-out and heavy GL-call/icon-upload bursts. Need GPU blit/zero-copy for real speed; target 10–20 fps first, not 60.
- V20 expected outcome: respring gone or much later, `CREATE_BO` failures sharply reduced, stability first; colours/smoothness need separate attempts.
