# S21+ iOS Port: Current Handoff

Updated 2026-10-31. This note captures the S21+ setup-skip investigation and the current early-kernel panic diagnostic. The main project history is in `NOTES.md`.

## Goal and device

- Port iOS 16 to the Samsung Galaxy S21+ SM-G996B (Exynos 2100).
- Current IPSW: iPhone 10,3 / 10,6 iOS 16.7.16, build 20H392.
- Device currently runs Arch Linux and has been reachable at `root@192.168.1.66` over SSH. Do not put the supplied password in project files.
- Bring-up uses a temporary kexec image. Keep Android partitions, bootloaders, recovery, modem, persist, and backups intact. The only intended iOS storage is the file image inside existing `userdata`; do not change the GPT.

## Setup Assistant investigation

- Audited `ios-galaxy-s8-main.zip` and `ios-sweet-main.zip` (S8 and S20+ comparison ports). Their `tools/sweet-root-tweaks.py` setup changes are byte-identical. The recipe sets `SetupDone`, `SetupFinishedAllSteps`, `SetupVersion=11`, disables the mini-Buddy/cloud setup path, and seeds the mobileactivationd data ark. The ADT's `/chosen` security properties select the offline Hactivation path.
- Neither ZIP contained `Setup.app` deletion, SpringBoard injection, or an auto-unlock implementation. The useful setup-skip recipe is the plist/data-ark plus ADT state.
- Added `tools/s21-setup-skip.py` and wired it into `sweet-sb-fix.sh` for clean roots. Read-only checks showed tw16 already contained those setup flags and data ark.
- The original trimmed tw16 image booted to Hello, but swipe-up did not open the phone. A complete-root comparison was built because the trimmed image lacked `/Applications/Setup.app`; this was a plausible cause, not a proven one.

## Full-root image state

- Built `iosroot-full-20H392.img`, a 16 GiB case-sensitive HFS+ image containing the OS root plus SystemOS and AppOS cryptexes, with the setup-skip state applied.
- Local SHA-256: `bfb992a684ec8d6552a779c36c4a631f1944d19c0c631981bb7634f8cdbd0434`.
- User authorized writing and booting it. It was streamed into `/mnt/android-data/linux/iosroot.img` on the phone after extending that existing file to 16 GiB. A completed remote hash matched the local hash at write time.
- The later return-to-Arch check found the remote file still has size 17,179,869,184 bytes and 33,587,440 blocks; `/mnt/android-data` had 3.6 GiB free (97% used). A second full hash check did not complete. The full-root image never mounted during the failed iOS boot, and no partition other than the image file was changed.

## Kernel build and panic

- UFS extent map for the 16 GiB sparse image contains 7,280 extents / 4,194,304 blocks. Kernel patch 0028 was enlarged to 8,192 entries and marks the kernel with `SWEET UFS: extent cap 8192`.
- User asked to switch Actions from macOS 14 to macOS 15. Workflow commit `384f038` did so; run `40948729419` succeeded using Xcode 16.0. Artifact kernel SHA-256: `8370ff66dcf9ad06da1616f595a351f5f03e89989589e65a4317cbd6921eac98`.
- `boot0104.sh` generated the extent map and ADT, copied the temporary shim/payload to the phone, and invoked kexec. The phone displayed `SWEET EARLY PANIC: Synchronous Exception taken while SP1 selected`; the full HFS root did not mount and Hello was not reached.
- Photo values: PC `0xfffffe0007480b24`, FAR `0x220`, ESR `0x96000006`, x0=0.
- Disassembly of the exact kernel shows PC is `ldr x0, [x0, #0x220]` at `_current_percpu_base+0xc`. The preceding call is `_current_thread`, which reads `TPIDR_EL1`; `machine_set_current_thread` writes that register. This establishes a null current-thread pointer at the fault.
- Compared with the Oct 28 kernel, `_current_percpu_base` and `_machine_set_current_thread` have identical instruction sequences. This does not support an Xcode 16 code-generation change as the cause. The exact caller/startup ordering that leaves `TPIDR_EL1` zero is still unknown.

## Diagnostic patch and latest build

- Added `xnu/patches/0106-sweet-early-fault-frame.patch`. It logs the saved frame pointer, previous frame pointer, and caller LR from `sleh_synchronous_sp1` before the normal panic path, so the call site can be mapped offline.
- Earlier CI runs must not be used for a phone boot:
  - `40952558826`: successful build, but 0106 was skipped because its panic-call syntax did not match XNU 8796.
  - `40954669761`: cancelled after the patch was reported malformed.
  - `40955455764`: the `sleh.c` hunk applied, but the helper insertion hunk missed `sweet_early.c` by one line; cancelled.
- Corrected the helper insertion to line 11 in commit `51946b2`, but the actual post-0012 `sweet_early.c` has the `kern/misc_protos.h` include on line 12. Run **40956410872** ended before acquiring a runner due to macOS ARM capacity constraints. Run **41147646499** succeeded on the Intel runner, but its kernel SHA-256 is `f30be6960dea9bed314b520582fac078db8d8357c8b1849371335677a3a26dd0`; the artifact contains the UFS extent-cap marker and does not contain `_sweet_early_trace_frame`. Its log confirms patch 0106 was skipped atomically because the helper hunk failed at `sweet_early.c:11`. The patch now uses exact context from the post-0012 source and passes `git apply --check` against the buildtree. CI requires the helper call in patched source and the compiled symbol. The artifact must not be booted for this diagnostic.
- The source phrase `SWEET EARLY FRAME` is lowered into immediate character writes by this compiler, so it is not a reliable `strings` marker. Verify `_sweet_early_trace_frame` and inspect its disassembly before boot instead.
- Do not boot any artifact until logs confirm both 0106 hunks applied and the kernel contains `_sweet_early_trace_frame`. Then verify the artifact checksum, prepare a temporary kexec payload, and use the printed caller LR with `ipsw macho a2s` to identify the startup path.

## Next actions

1. Push the corrected patch and CI guard to trigger a fresh build; verify both source and symbol checks pass.
2. If it builds, download the artifact, verify its `SHA256SUMS`, trace marker, and function symbol/disassembly. Do not use the failed artifacts above.
3. With the phone in Arch, run the temporary kexec diagnostic kernel (no userdata rewrite or partition write), capture the early frame/caller line, and map the LR in the exact kernel.
4. Inspect that caller's startup path and only then decide whether `machine_set_current_thread` ordering or another path needs a code change.

## Relevant repository history

- `384f038`: switch kernel CI to macOS 15.
- `d12233d`: record initial Xcode 16 early panic.
- `1378bf2`: add frame diagnostic (first hunk attempt; later corrected).
- `84eefb6`, `f5ff536`, `51946b2`: iterative corrections to the diagnostic patch.
- `1914675`: record latest patch offset and queued run.
