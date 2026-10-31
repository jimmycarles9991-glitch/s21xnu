# S21 (Exynos 2100) iOS/XNU port: handoff prompt and status

Paste everything below the line into a new session, together with the latest boot photo.
Last updated: Sat 31 Oct 2026 (SMP + touch working, see the LAST section). Previously: Tue 27 Oct 2026, late morning, after the MobileIcons/PrivateFrameworks root repairs (tw6, tw7) and the first Apple-logo boot. **The LAST section of this file ('Latest session update, 27 Oct, root repairs and crash symbolication') is the newest state and supersedes the older 'Next actions' and 'Current handoff state' lists above it.** Older milestones below are kept as recorded.

---

## PROMPT

You are continuing the **ios-sweet S21 (Exynos 2100) port**: the dipa-studio/ios-sweet XNU (xnu-8796, iOS 16) port, retargeted from the Redmi Note 10 Pro (Qualcomm) to a Samsung S21+. Focus strictly on iOS/XNU. Do not explore UEFI, Windows or T2S unless asked.

### How I work (follow this)
- I give you one multiblock copy-paste command; no long explanations. Keep answers short, exact commands, terse.
- **Never put `set -e` in a block I paste into the shell**, because it kills Konsole. Write a script file with `cat > ~/x.sh <<'EOF'` and run it with `bash ~/x.sh`. Heredocs sometimes get flattened on paste; prefer single-line `{ ...; } > ~/x.out 2>&1; cat ~/x.out` blocks for read-only work.
- Do not re-run things that already succeeded (shim diagnostics, QEMU tests, kernel build, first-light boot, ramdisk/PID 1 boot, HFS+ root boot, init v2 tests, UFS link bring-up, NOP OUT, fDeviceInit, TUR, GPT parse, partition data read). Also done (24 Oct): HS-G4 power-mode change (`ufshs=2` boot, 0083 v2) and the file-backed HFS+ root at HS-G4 (`ufshs=1`).
- Also done 25 Oct (do not redo): trimmed iOS root written and verified (`SAME_EXTENTS`, SHA-256 match); `0084` and `0085` generated, reviewed, pushed and built; first boot of the iOS root (AMFI panic, then fixed by 0085); symbolication and loader disassembly (`~/sym.py`, `~/ioc.py`).
- Verify before acting. If you have not read a file, say so and give me a command to print it. When I paste a command without its output, say the output did not arrive. Do not state a theory as fact; mark it as a guess. Check a build artifact really is the new build (grep a new string) before using it.
- **kexec is never run on the PC.** Build on the PC, `scp` to the phone, then `ssh` to the phone and kexec there.
- Never write to `sda` (the phone's internal UFS disk) without my explicit approval of the exact partition. XNU itself has no UFS write path (there is no UFS WRITE command; since 0084 a RAM-only overlay can accept writes with `ufsov=1`, but they never reach the disk). The only write allowed so far is from Linux into the single file `iosroot.img` inside the f2fs `userdata` filesystem (see Milestone 5b).
- **I still need Android** (do not suggest wiping `super` or other Android partitions). I said shrinking `userdata` is fine and no backup is needed, but there is no `resize.f2fs` on the phone, so the pinned-file route was chosen instead.
- Do not use `gh` (not installed) or LLVM tools (`llvm-nm` is not installed; host `nm` cannot read Mach-O).
- "Apple logo then lockscreen" is not a realistic near-term goal (iBoot draws the logo; lockscreen needs launchd + SpringBoard/backboardd + display/GPU IOKit services that do not exist on the S21). Realistic goal: real launchd running with verbose logs.

### Environment
- PC: `tom@tom-jupiter` (Arch). **Working repo: `~/ios-sweet`, branch `s21-stage2`.** Intentional uncommitted local edits at the time of writing: `tools/mkpayload.py` (ramdisk bytes zero-padded to `rd_size`) and `tools/mkadt.py` (the working-tree file shows `UFS, UFS_SIZE = 0x13100000, 0x2000`; the older note said `0x200`, check `git diff tools/mkadt.py`). The ufsmap commit (below) does `git add tools/mkadt.py`, so it carries that uncommitted edit.
- **Remotes:** `origin` = `github.com/dipa-studio/ios-sweet` (stale, NOT used for builds). **`s21xnu` = `github.com/tiktop101/s21xnu`, branch `main`: the live repo that runs CI.** Push with `git push s21xnu s21-stage2:main` (username `tiktop101` plus token). Never force-push.
- Untracked in the repo: `adt*.bin`, `hfs-root.img`, `payload*.bin`, the IPSW, `xnu-kernel.zip` etc.
- **XNU patches:** `~/ios-sweet/xnu/patches/` (numbered up to `0085`; `0084` = RAM write overlay, `0085` = AppleMobileFileIntegrity IOKit service). Main file: `0028-sweet-ufs-root-device.patch` (creates `bsd/dev/sweet_ufs.c`). **`0083-sweet-ufs-hs-g4.patch`** (HS-G4, Milestone 5c; `0029` was already taken) is generated, not hand-edited: `python3 ~/mk0083.py` extracts the post-0028 `sweet_ufs.c` with `git apply --include`, edits it at asserted anchors, lets `git diff` write the patch, then runs `git apply --check` and a byte-compare. Do any further `sweet_ufs.c` change the same way; hand-recounting hunk headers in a 2434-line new-file patch is error-prone.
- Shim: `/home/tom/s21/New Folder/ios-sweets21+/s21-shim.Image` (sha256 `cf310264650d7b1700423640502644ad752edde635a5b24a6aa59ecb79485b3d`; quote the path). A copy sits in `~/ios-sweet`.
- Phone: `root@192.168.1.66` (password prompt on every ssh/scp), Linux 7.3.0-rc5 with kexec. Ping may fail while ssh works. No `devmem`, `busybox`, `f2fs_io`, `resize.f2fs`. Present: `python3`, `filefrag`, `fallocate`.
- clang on the PC has the aarch64 backend (`/usr/bin/clang`). PC also has `apfs-fuse` (`/usr/local/bin`), `7z`; no `dmg2img`, no `apfsutil`.
- **Linux UFS driver source:** `/home/tom/s21/linux/drivers/ufs/host/ufs-e2100-drv.c`; core `/home/tom/s21/linux/drivers/ufs/core/ufshcd.c`; `ufs-cal-if.c`, `ufs-cal-2100.h`, `ufs-vs-regs.h`, `ufs-vs-mmio.h` under `host/`; `include/ufs/ufs.h`, `include/ufs/unipro.h`. DT: `/home/tom/s21/exynos2100-t2s-firstlight-v32.dts`.

### CI, and how patches are applied (verified from the build script)
- Workflow `.github/workflows/xnu.yml` runs on `workflow_dispatch` and on pushes to `main` touching `xnu/patches/**`. It fetches blacktop's `darwin-xnu-build` at the pinned `DXB_SHA` (`663906528d1c...`), copies `xnu/patches/*.patch` and `*.sh` into `dxb/patches/`, runs `bash build.sh` (`macos_version=13.5 kernel_config=DEVELOPMENT machine_config=SWEET`, xnu-8796.141.3).
- **`build.sh` `patches()`: `*.sh` first, then every `*.patch`, each guarded by `git apply --check`. A failing patch is SKIPPED with a warning ("Skipping patch ...") and the build carries on; it never turns CI red.** So a malformed edit to 0028 would silently drop the whole UFS driver; check that the new marker string is in the built kernel.
- Against the current 0028, patches **0030 (writes), 0034 (dma above 4g), 0047 (persistent console log), 0054 (process sampler), 0060 (keys/touch), 0069 (faster), 0072 (usb probe), 0077 (usb live log), 0082 (ufs diag)** failed their `sweet_ufs.c` hunks and are expected to be skipped (as of the last time this was checked; the Build log "Skipping patch" lines have NOT been looked at in any recent run). 0067 (console mute) is untested. Files touched by the skipped ones: 0047: `serial_console.c`; 0054: `bsd/conf/files`, `sweet_sample.c`, `exception.c`, `IOUserClient.cpp`, `SweetFramebuffer.cpp`; 0060: `bsd/conf/files`, `sweet_keys.c`, `sweet_touch.c`; 0072: `bsd/conf/files`, `sweet_usb.c`; 0077: `sweet_usb.c`; 0030/0034/0069: only `sweet_ufs.c`. So the built kernel has read-only UFS and no persistent log, USB, keys or touch. **0083 did apply** in the build behind IMG_0311 (its `hsg4` lines are on screen); the generator's own output and the commit hash were not seen.
- **Getting the kernel (do it exactly this way):** download `xnu-kernel.zip` from the Actions run for the commit into `~/ios-sweet`; extract to `/tmp/newk`; the kernel is **`/tmp/newk/kernel.development.sweet`** (a `find -name 'kernel*sweet*'` glob matched `kernel.development.sweet.link/...linkarguments` once and wasted a step). The zip file's mtime is the download time; the mtimes inside are the CI build time (a zip whose files predate the push is the previous run's artifact). Always `grep -a -c '<new string>' /tmp/newk/kernel.development.sweet` and only then `cp` it over `xnu/out/kernel.development.sweet`. It is not copied automatically.
- **0084 and 0085 in CI (25 Oct):** the kernel built at 14:44:08 UTC contains `ufsov: RAM overlay` (1 hit), `hsg4 pmc: v2` (1), `ufsmap gap` (1) and `SWEET AMFIKIT` (2), so both new patches applied. `0085` touches `iokit/conf/files.arm64` and `iokit/KernelConfigTables.cpp`, where it depends on context from 0038 and 0040; if CI ever skips either of those, 0085 is silently skipped too, so always grep the marker.
- XNU source for the exact tag: `git fetch --depth 1 https://github.com/apple-oss-distributions/xnu xnu-8796.141.3`.

### Hardware facts
Verified by the shim on the phone:
- PSCI 1.1 over **HVC**. GICv3: GICD `0x10200000`, GICR `0x10240000`. Virtual timer INTID 27, 26 MHz. MMU supports 16 KiB pages.
- Framebuffer `0xfa000000`, 1080x2400, stride 4320 (XNU shows it as 1125x2436 presented).
- DRAM window for kernel + payload: `0x8c0000000` to `0x940000000` (2 GiB).
- Secondary CPUs never came up in the PSCI probe, so `cpus=1`.

From the phone's Linux:
- **RAM** (`/proc/iomem`): blocks between `0x80002000` and `0xffdfdfff`, and above 4 GiB `0x8a0000000-0x8afffffff`, `0x8bb600000-0x99bbfffff` (about 3.5 GiB, not yet checked against the shim's memory map), plus small blocks. A 16 MiB wifi DMA pool sits at `0x92400000`. The 1 MiB no-map `ufs-dma@a8000000` is NOT used by the Linux UFS driver for ordinary DMA.
- **UFS host** `exynos2100-ufshc` at `0x13100000` (`std` 13100000-131001ff, `hci` 13101100-131012ff, `pma` 13104000, `cport` 13108000, `unipro` 13180000, `ufsp` 132a0000). No iommu in the UFS path (the SysMMU at `0x18d50000` is audio).
- **Storage:** `sda` = Samsung KLUDG4UHDC-B0E1, 119.1 GB, GPT with 34 partitions (all GPT names, in order): 1 efs, 2 sec_efs, 3 steady, 4 param, 5 up_param, 6 keystorage, 7 bota, 8 harx, 9 uh, 10 uhcfg, 11 dtbo, 12 blcmd, 13 boot, 14 vendor_boot, 15 recovery, 16 radio, 17 cp_debug, 18 nad_refer, 19 persistent, 20 misc (1 MB), 21 keyrefuge, 22 dqmdbg, 23 vbmeta (64 KB), 24 vbmeta_system, 25 metadata, 26 fmp_selftest, 27 reserved, 28 super (10.6 GB, Android), 29 prism (1.1 GB), 30 optics, 31 cache (600 MB), 32 omr, 33 spu, 34 userdata (106.2 GB). (`cpefs` is on `sdd1`, another LUN.) Block size seen by GPT/XNU: 4 KiB device blocks, 512-byte logical sectors for the BSD layer.
- **`userdata` (`sda34`)** is f2fs mounted at `/mnt/android-data` (rw, noatime, lazytime, discard, checkpoint_merge, active_logs=6, ...). Linux root is `/mnt/android-data/linux/rootfs.img` (ext4, loop) inside it, along with the kexec launcher and Android data. **Overwriting `userdata` would destroy all of that.** Start sector from sysfs `27082752` (512 B units, so 4 KiB block `3385344` = `0x33A800`), size `222773248` sectors. Usage after creating iosroot.img: 107G total, 91G used, 16G free.
- Linux cmdline: `maxcpus=1 console=tty0 ignore_loglevel loglevel=8 panic=0 oops=panic clk_ignore_unused pd_ignore_unused ...`

### Linux UFS driver facts (from `ufs-e2100-drv.c`, DT v32, dmesg, `ufshcd.c`)
- Probe-time platform setup: PMU (`0x15860000`) `0x728` mask 1 val 1; sysreg (`0x11020000`) `0x700` mask `0x300000` written 0 (non-coherent); PMU `0x3e10` mask 8 val `1<<3`. **SMC `0xC2001810(0,0,3)` = FMP_SECURITY, SMC `0xC2001850(0,0,0)` = SMU_INIT**, both return 0.
- Order: host sw reset, config_host, device reset, HCE=1, clock-stop enable plus gates, CAL pre_link, DME_LINKSTARTUP, connected lanes (UniPro `0x3184`/`0x3204`), CAL post_link. UniPro mclk 166 MHz. DT `ufs,pmd-attr-gear` / `ufs,pmd-attr-lane` give `dt_gear`/`dt_lane` (defaults 1); target is G4 L2.
- Quirks: `UFSHCD_QUIRK_PRDT_BYTE_GRAN | UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR | UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR`. PRDT entries 128 bytes, max segment 4096. Under BYTE_GRAN the UTRD response/PRDT offsets and lengths are in bytes, response length 0x200.
- **UPIU query opcodes (verified in `ufs.h`):** NOP 0, READ_DESC 1, WRITE_DESC 2, READ_ATTR 3, WRITE_ATTR 4, **READ_FLAG 5, SET_FLAG 6**, CLEAR_FLAG 7, TOGGLE_FLAG 8.
- **Power-mode sequence (read from `ufshcd.c` `ufshcd_change_power_mode` / `ufshcd_dme_change_power_mode`) for the HS-G4 step:** `PRE_CHANGE` notify, then DME_SET `PA_RXGEAR`, `PA_ACTIVERXDATALANES`, `PA_RXTERMINATION` (true for FAST/FASTAUTO), `PA_TXGEAR`, `PA_ACTIVETXDATALANES`, `PA_TXTERMINATION`, `PA_HSSERIES` (when a FAST mode is used), then (unless `UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING`) `PA_PWRMODEUSERDATA0..5` and `DME_LocalFC0ProtectionTimeOutVal`/`LocalTC0ReplayTimeOutVal`/`LocalAFC0ReqTimeOutVal` to their defaults (`DL_FC0ProtectionTimeOutVal_Default` is 8191), then a UIC power-mode change with `pwr_rx << 4 | pwr_tx` (`PA_PWRMODE`), then `POST_CHANGE` notify. MIB IDs (verified in `unipro.h`): `PA_ACTIVETXDATALANES 0x1560`, `PA_TXGEAR 0x1568`, `PA_TXTERMINATION 0x1569`, `PA_HSSERIES 0x156A`, `PA_PWRMODE 0x1571`, `PA_ACTIVERXDATALANES 0x1580`, `PA_RXGEAR 0x1583`, `PA_RXTERMINATION 0x1584`, `PA_PWRMODEUSERDATA0 0x15B0` (1..5 follow), `DME_LocalFC0ProtectionTimeOutVal 0xD041`. The e2100 vendor `pwr_change_notify`: PRE_CHANGE clamps gear/lane to `dt_gear`/`dt_lane`, FAST_MODE both ways, HS rate B (source marks this an ASSUMPTION), fills `act_pmd`, calls `ufs_cal_pre_pmc(p)`; POST_CHANGE sets `active_tx_lane`/`active_rx_lane` and calls `ufs_cal_post_pmc(p)`. `ufs_cal_pre_pmc` also masks `PA_ERROR_IND_RECEIVED` in `UNIP_DL_ERROR_IRQ_MASK` and picks `calib_of_pwm`/`calib_of_hs_rate_a/b` tables; `ufs_cal_post_pmc` picks `post_calib_of_*`. Linux sources: `ufs-cal-if.c` lines ~657 and ~689.
- **Checked 24 Oct (was open):** 0028 already has the pmc tables (`calib_of_pwm`, `calib_of_hs_rate_a/b`, `post_calib_of_*`, plus `_card` variants), `ufs_cal_config_uic`, `struct uic_pwr_mode`, `PMD_*`, `ufs_cal_pre_link/post_link` and `su_uic()`. It did NOT have `ufs_cal_pre_pmc/post_pmc`; 0083 ports them as `su_cal_pre_pmc/su_cal_post_pmc` (only `SLOW_MODE` is special-cased, not `SLOWAUTO_MODE`; irrelevant for FAST). Verified in `ufshci.h`: `UIC_CMD_DME_GET 0x01`, `DME_SET 0x02`, `UIC_ARG_MIB_SEL(attr, sel) = attr<<16 | sel`, `UIC_ARG_ATTR_TYPE(t) = t<<16`, power-mode IRQ is IS bit 4 (`UIC_POWER_MODE 0x10`), `UPMCRS` is HCS bits 10:8 with `PWR_OK 0`, `PWR_LOCAL 1`, `PWR_REMOTE 2`, `PWR_BUSY 3`, `PWR_ERROR_CAP 4`, `PWR_FATAL_ERROR 5`. `e2100_pwr_change_notify` (drv.c line 208) was read and matches the summary above.

### Commands that work
```bash
python3 tools/mkadt.py adt.bin --highmem          # OUT FIRST, then flags. The reverse order writes a file named "--highmem".
python3 tools/mkpayload.py KERNEL ADT OUT [--cmdline "..."] [--ramdisk IMAGE]
scp s21-shim.Image payload.bin root@192.168.1.66:/root/
ssh -t root@192.168.1.66 'cd /root; kexec -u </dev/null 2>/dev/null; kexec -l s21-shim.Image --initrd=payload.bin </dev/null; echo jumping; sleep 2; kexec -e </dev/null'
```
- ssh dropping after "jumping" is normal. No `--mem-min` needed.
- UFS test cmdline: `-v debug=0xc serial=3 cpus=1 keepsyms=1 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0 sweetlog=0 rd=ufsroot0 ufsroot=<GPT name>`. Keep `sweetlog=0` on every UFS test. Add `ufshs=1` (try HS-G4, keep booting) or `ufshs=2` (try, re-read block 0 of the selected partition, log it, then return `ENXIO` on purpose so SULOG reaches the `IOFindBSDRoot` panic). Without `ufshs` the proven PWM G1 path runs untouched.
- `bash ~/hsboot.sh [N]` (default `ufshs=2`): unzips `~/ios-sweet/xnu-kernel.zip` to `/tmp/newk`, stops unless the kernel contains both `hsg4 pmc: v2` and `ufsmap gap`, copies it to `xnu/out/kernel.development.sweet`, builds the ADT (no `UFSMAP`) and `payload-hs.bin` with `ufsroot=vbmeta`, then scp + kexec. Compare the printed kernel mtime with the push time (a stale zip is the previous run's artifact).
- `bash ~/hsroot.sh`: file-backed root at HS-G4. Stops unless `xnu/out/kernel.development.sweet` contains `hsg4 pmc: v2`; over ssh runs `filefrag -v` on `iosroot.img` into `/tmp/ff3.txt`, `mkufsmap.py` -> `ufsmap3.bin`, `cmp` against `ufsmap.bin` (prints `PIN HELD`, or `WARNING` if extents differ, or says there was nothing to compare); builds the ADT with `UFSMAP=ufsmap3.bin` and `payload-hsroot.bin` with `... sweetlog=0 rd=ufsroot0 ufsroot=userdata ufshs=1`; scp + kexec. The `iosroot.img` path in it, `/mnt/android-data/linux/iosroot.img`, is taken from the 5b notes.
- `python3 ~/fix0083.py` patched `~/mk0083.py` (non-fatal soft sets, marker `hsg4 pmc: v2`) and `~/hsboot.sh`; re-run `python3 ~/mk0083.py` afterwards to regenerate the patch. Do not run `fix0083.py` twice (its anchors are single-use).
- iOS-root helpers (PC unless noted): `python3 ~/iossize.py` (read-only size report from the `~/ipsw` apfs-fuse mounts, remounts them if missing, output `~/iossize.out`); `~/trim.filter` (rsync exclude list); `python3 ~/mkderive.py` (derives `tools/mkiosroot-trim.sh` from `tools/mkiosroot.sh` with asserted anchors); `bash tools/mkiosroot-trim.sh OS.dmg SYSTEMOS.dmg OUT.img [SIZE=4G]` (sudo, builds the image; log `~/mkiosroot-trim.out`); `bash ~/wroot.sh` (the write to the phone, see Milestone 6b; log `~/wroot.out`). Probe outputs: `~/iosroot_probe.out`, `~/iostrim.out`, `~/trimchk.out`.
- **Symbolicate a panic address** (no LLVM): parse the kernel Mach-O `LC_SYMTAB` in Python (`ncmds` at header offset 16, load commands from 32, cmd `0x2`, nlist entries 16 bytes, keep `n_type & 0x0e == 0x0e`), sort and bisect. Only valid if `xnu/out/kernel.development.sweet` is the exact kernel that panicked (compare the Kernel UUID).
- **`bash ~/ovboot.sh`** (written 25 Oct): unzips `~/ios-sweet/xnu-kernel.zip` to `/tmp/newk`, prints the zip and kernel mtimes, stops unless the kernel contains all of `ufsov: RAM overlay`, `hsg4 pmc: v2`, `ufsmap gap`, `SWEET AMFIKIT`, copies it to `xnu/out/kernel.development.sweet`, then **regenerates `~/ovroot.sh` from `~/hsroot.sh`** with `sed` (inserts ` ufsov=1 rootrw=1` after `ufshs=1`, asserts exactly one `ufshs=1`) and runs it (several phone password prompts). The cmdline it boots: `-v debug=0xc serial=3 cpus=1 keepsyms=1 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0 sweetlog=0 rd=ufsroot0 ufsroot=userdata ufshs=1 ufsov=1 rootrw=1 unentitled_ios_sim_launch=1` (`unentitled_ios_sim_launch=1` was already in `hsroot.sh`; its purpose has not been checked). **To boot a variant** (for example `ufsovmb=512`, or without `ufsov`), do not go through `ovboot.sh` (it overwrites `ovroot.sh`): copy `~/ovroot.sh` to `~/ovroot2.sh`, edit the cmdline in the copy and run `bash ~/ovroot2.sh`. The `PIN HELD`/`WARNING` line from the extent check scrolls past in that output; look for it.
- **Panic symbolication, working version:** `python3 ~/sym.py` (parses the kernel Mach-O, prints the kernel UUID next to the panic's UUID, the `__TEXT` vmaddr and one symbol per address; edit the address list inside the script). The first version matched `__TEXT_EXEC` instead of `__TEXT`, gave a skewed slide and produced nonsense symbols; the fixed version's `__TEXT vmaddr` equals the panic's `Kernel text base`. Always compare the two UUIDs.
- **Reading a userland binary on the PC (no LLVM):** `pip install capstone --break-system-packages`; mount the image read-only (`sudo mount -t hfsplus -o loop,ro,force ~/ios-sweet/iosroot-trim.img /mnt/iosro`, copy the file out, `sudo umount /mnt/iosro`). `~/ioc.py` parses `LC_DYLD_CHAINED_FIXUPS` (format 1) to name the stub imports and prints call sites plus the code around chosen calls. In small binaries string references are `adr`, not `adrp`+`add`. **Never name a helper script `dis.py`**: it shadows Python's stdlib `dis` module and capstone fails with a circular import (it is `~/disl.py` now).
- **Patch generators** (`~/mk0084.py`, `~/mk0085.py`): same pattern as `mk0083.py`. Lessons: (1) apply anchor edits before inserting text that itself contains an anchor line (0084's helpers contained the `blocks` line twice, which made the anchor match 3 times; `~/fix0084.py` reordered it, single use); (2) for files that no earlier patch creates (the iokit tables), the generator fetches the real `xnu-8796.141.3` tag, applies the earlier patches that touch those files with `git apply --include`, then edits; (3) `git diff -U1` keeps context small; (4) the generator checks removed-line count, the marker, a round trip, and that added code mentions no UFS write opcodes.
- Ramdisk build for a Mach-O root (`~/s21boot3.sh`) and the `hfs-root.img` recipe (`~/s21hfs.sh`, `~/s21hfs2.sh`): pad the Mach-O to a multiple of 0x4000; `mkpayload.py ... --ramdisk ...` with cmdline `-v debug=0xc serial=3 cpus=1 keepsyms=1 rd=md0 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0`; `hfs-root.img` is 64 MiB, `mkfs.hfsplus -v SWEETROOT` (no journal), `/sbin/launchd` = `sweet-init.macho`.

### Milestones 1 to 4 (done)
1. First light, no ramdisk: XNU boots, SWEET stand-ins registered, `SWEET FB: AppleCLCD 1080x2400`, `cpu=0 came alive`, `SWEET HFS` registered.
2. Ramdisk root, `sweet-init` as PID 1 (zero-padding the Mach-O to 16 KiB fixed a `vm_map.c:1676` assert).
3. HFS+ ramdisk root, `/sbin/launchd` loaded from HFS.
4. Init v2 passes remount rw, file/dir ops, fork+exec+wait4, sysctl, mmap, /dev/random from the HFS root.

### Milestone 5 (UFS read path): DONE as of 21:54
`0028-sweet-ufs-root-device.patch` mirrors `ufs-e2100-drv.c`: PMU/sysreg writes, both SMCs, host sw reset, config_host, device reset, HCE=1, clock-stop/gates, CAL pre-link, DME_LINKSTARTUP with retries, lanes from hardware, CAL post-link, NOP OUT, set + poll `fDeviceInit`, TEST UNIT READY with retries, GPT parse (LBA 1, 128 entries x 128 bytes), name lookup via `ufsroot=` (default `userdata`), and a read-only BSD block/char device `ufsroot0`/`rufsroot0` (`bdevsw`/`cdevsw`, `d_strategy` supports reads only, writes return EROFS). READ(10) opcode `0x28`; DMA via a contiguous bounce buffer; polled operation (`IE=0`). **0028 itself leaves the link in PWM G1 (slow); the power-mode change lives in 0083 (Milestone 5c) and is off unless `ufshs=` is passed.** `su_uic(cmd, a1, a2, a3, *val)` is the UIC helper: polls UCRDY (HCS bit 3), writes args to `0x94/0x98/0x9c`, command to `0x90`, polls IS bit 10 (UCCS), result in `0x98 & 0xff`, arg3 result in `0x9c`. It is used with cmd `0x16` (DME_LINKSTARTUP), `0x03` (DME_PEER_GET) and `0x04` (DME_PEER_SET) in the patch; DME_GET `0x01` and DME_SET `0x02` are verified in `ufshci.h` and used by 0083. SULOG lines (`SWEET UFS: ...`) go to a 1100-byte buffer that is appended **only** to the `IOFindBSDRoot` panic string.

Bring-up history (compressed): link startup failed with the old hand-typed 0028; PRDT byte-granularity bug (UTRD offsets must be bytes under `PRDT_BYTE_GRAN`) fixed in `bfdda24`; query opcodes were `READ_FLAG 0x06`/`SET_FLAG 0x07` and are fixed to `0x05`/`0x06` in `12f2230` (confirmed against `ufs.h`; that commit is `s21xnu/main`).

Proof boots (photos, kernel UUIDs differ per build):
- `5A49245F-...` (commit `187aa88`, prints `u7 TUR err=%d hex=0x%x OK/FAIL`): `u2 hce 0x1 hcs 0x8`, `u3 try 0 ... hcs 0xf`, `u4 link up tx 2 rx 2 post 0`, `u6 nop -> 0`, `u6 fDeviceInit done after 2 ms`, then `req 0x01 ocs 0x7 IS 0x1` and `u7 TUR err=0 hex=0x0 OK`.
- `C4D3AE8E-...` (GPT messages moved from `printf` to `SULOG`, added entry count): `no GPT part "zzzz" in 128 entries`. So the GPT read/parse is correct.
- `2229D6AF-...` (commit `4b64be2`, temporary diag): `ufsroot=vbmeta` printed `part0 41564230 00000001 00000000 00000000`, which matches `xxd` of `/dev/sda23` on the phone (`AVB0`, version 1, zeros). **Partition data reads are correct end to end.** (`misc`/sda20 is all zeros on the phone, useless as a test.) `ufsroot0 unavailable: 6` there was only the diag's forced `ENXIO`.
- After reverting the diag (the revert commit hash was not recorded; `4b64be2` is what was reverted): `"vbmeta" on LUN 0: first block 0x1cd90, 0 MiB, read-only`, `BSD root: ufsroot0, major 1, minor 0`, then HFS rejects it with `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature`, `hfs_mount ... error=22`, `cannot mount root, errno = 19`, `mount(2..4) failed`, and the boot continues to later `SWEET HID/FB/CPU` lines instead of panicking at the root lookup. So the block-device layer works; only a real HFS+ filesystem is missing.
- `req 0x01 ocs 0x7 IS 0x1` before `u7` is printed when a request completes with nonzero OCS (UPIU type 0x01 = SCSI command, so the first TUR). My guess (unverified): a unit-attention-style failure that the retry loop clears (OCS 7 meaning fatal error is from memory of the UFS spec).

### Milestone 5b (root on a pinned file inside userdata): DONE as of 22:33
**Result (photo):** `SWEET UFS: ufsmap 1564 extents, 1048576 blocks`; `"userdata" on LUN 0: first block 0x33a800, 4096 MiB, read-only`; `BSD root: ufsroot0, major 1, minor 0`; `hfs: mounted SWEETROOT on device b(1, 0)`; `SWEET: root volume is not authenticated (error 25); continuing unsealed`; `init_program: attempting to load /usr/appleinternal/sbin/launchd.development` then `/sbin/launchd`; `sweet-init` ran as PID 1 from the file-backed UFS root (top of the screen shows `SWEET userspace: init running, PID 1`, `read /sbin/launchd magic OK`, and write/mkdir tests failing, which is expected because XNU has no write path; the exact errno digits were too small to read). So the extent map, the pin (the script would have stopped on moved extents; its output was not seen) and the HFS+ mount from UFS all work. The remaining text below is the design/history of this step.
**Why:** a usable iOS root is far bigger than any safe partition. `cache` 600 MB is too small; `super` (10.6 GB) is Android's and I need Android; `userdata` holds the Linux rootfs; the 2 GiB DRAM window rules out a ramdisk. Shrinking is not possible without `resize.f2fs` (and would need an offline fs). So: a pinned 4 GiB file inside the f2fs filesystem, exposed to XNU read-only through an extent table.

**Done on the phone (output seen):** `/mnt/android-data/linux/iosroot.img` created, `F2FS_IOC_SET_PIN_FILE` ioctl run via python (ioctl number `0x4004f50d` from memory, GET readback `0` which is probably the GC-failure count rather than the flag; **pin not proven**), then `fallocate -l 4G`, `sync`. `filefrag -v`: **1564 extents, 1048576 blocks of 4096 (4 GiB), none unwritten, not contiguous** (first extents are 512 blocks = 2 MiB each, some merged ones up to 10752 blocks, last extent logical 1048064 -> physical 2075136, length 512). Physical offsets are in 4 KiB blocks relative to the start of the `userdata` partition, which matches XNU's 4 KiB device blocks plus `su.first_block`. The PC copy of that listing is `/tmp/ff.txt` (volatile; regenerate with `ssh root@192.168.1.66 filefrag -v /mnt/android-data/linux/iosroot.img > /tmp/ff.txt` if gone).

**Patch as designed (the generating script's output was never seen, but the boot result above shows it applied, built and worked):**
- `tools/mkufsmap.py FF.TXT OUT.bin`: parses `filefrag -v` into 12-byte little-endian records `(logical_start, physical_start, length)` in 4 KiB blocks; asserts contiguous logical coverage, no `unwritten`, physical < 2^32. 1564 extents = about 18.8 KB.
- `tools/mkadt.py`: `UFSMAP=<file>` env var adds a `ufsmap` property to the `/sweet-ufs` node (`ph(), compatible qcom,ufshc, reg`, plus `ufsmap`). The property syntax (`('name', bytes)` tuples) was inferred from the existing code, not verified against `node()`.
- `0028`: `static uint32_t su_ext[3*4096]; static unsigned su_next;` and a lookup in `sweet_ufs_strategy` (`pb = su.first_block + ext.physical + (lba - ext.logical)`, `want` clamped to the extent end, `EINVAL` if lba is past the table); in `sweet_ufs_init`, before `bdevsw_add`, it reads `ufsmap` (size must be a multiple of 12 and <= `sizeof(su_ext)`, logical gaps rejected: `SWEET UFS: ufsmap gap at %u`), sets `su.nblocks` to the total, prints `SWEET UFS: ufsmap %u extents, %llu blocks`. Reads past the table end return `EINVAL`, so nothing outside the file is reachable. Hunk headers were recounted by the script; `git apply --numstat` should be clean (not confirmed).
- Commit message planned: `0028: ufsmap extent table so ufsroot can be a pinned file inside userdata (read-only)`. Marker string for the artifact check: `ufsmap gap`.

**Run script `~/go.sh` (written; its console output, including the `SAME_EXTENTS` line, was never seen, so the pin is still unproven; the boot result above only shows the extents were right for that boot):** checks the kernel zip contains `ufsmap gap` (else stops); `scp hfs-root.img`; `dd if=/root/hfs-root.img of=/mnt/android-data/linux/iosroot.img bs=1M conv=notrunc,fsync`, `sync`, `filefrag -v` into `/tmp/ff2.txt`; diffs extents against `/tmp/ff.txt` and prints `SAME_EXTENTS` (if not, the pin did not hold: stop); `mkufsmap.py /tmp/ff2.txt ufsmap.bin`; `UFSMAP=ufsmap.bin mkadt.py adt.bin --highmem`; builds `payload-ufs11.bin` with `ufsroot=userdata`; scp + kexec. `hfs-root.img` is only 64 MiB, so the HFS+ volume is much smaller than the 4 GiB device; whether HFS accepts that is unverified (it normally does).
**Expected on screen if it works:** `"userdata" on LUN 0: first block 0x33a800` (corrected value: 27082752 / 8 = 3385344 = `0x33A800`; an earlier message said `0x33ab80`, which was wrong), `ufsmap 1564 extents, 1048576 blocks`, `BSD root: ufsroot0`, then the init v2 tests from the HFS root as in milestone 4.
**Safety:** XNU has no write path; the only write is the Linux `dd` into that one file. Run `sync` on the phone before each kexec. If the pin did not hold and f2fs relocated blocks, the map would point at stale data; always regenerate the map after the last write and compare it.

### Milestone 5c (HS-G4 power-mode change): DONE on 24 Oct. Attempt 1 = clean failure, attempt 2 (v2) = HS-G4 works
**Patch:** `0083-sweet-ufs-hs-g4.patch`, generated by `~/mk0083.py` (generator output and commit hash not seen; the on-screen `hsg4` lines prove CI applied it). It adds `su_cal_pre_pmc/su_cal_post_pmc` (ports of the Linux functions), `su_dme_set/su_dme_get` (UIC `0x02`/`0x01`, arg1 `attr<<16`, arg2 0, arg3 value), `su_try_hs()` (called after `ufs_device_init()` succeeds in `ufs_take_over_host`) and `su_hs_verify()` (called after `ufs_find_partition`). The `SU_PA_*` and `SU_DL_*_Default` constants are parsed from `unipro.h` by the generator, not typed. Boot-arg `ufshs`: absent or 0 = untouched PWM G1 path; 1 = try and keep booting; 2 = try, then re-read block 0, log it and return `ENXIO` to force the panic that shows SULOG.
Sequence in `su_try_hs`: DME_GET/DME_PEER_GET `PA_MAXRXHSGEAR` (clamp gear), `su_cal_pre_pmc`, then DME_SETs in this order: `PA_RXGEAR`, `PA_ACTIVERXDATALANES`, `PA_RXTERMINATION=1`, `PA_TXGEAR`, `PA_ACTIVETXDATALANES`, `PA_TXTERMINATION=1`, `PA_HSSERIES=B`, `PA_PWRMODEUSERDATA0..5`, `DME_LocalFC0ProtectionTimeOutVal`, `DME_LocalTC0ReplayTimeOutVal`, `DME_LocalAFC0ReqTimeOutVal`; then `PA_PWRMODE = FAST<<4 | FAST`, wait for IS bit 4, require HCS bits 10:8 == 1 (`PWR_LOCAL`), `su_cal_post_pmc`, read back `PA_PWRMODE`. **Any nonzero DME_SET result returns before the mode change** (this turned out too strict, see below).

**Boot (photo IMG_0311, kernel UUID not visible):** cmdline `... sweetlog=0 rd=ufsroot0 ufsroot=vbmeta ufshs=2`, no `UFSMAP`. The panic `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6` is the intended `ENXIO` from `su_hs_verify`. Lines, with the `SWEET UFS: ` prefix dropped:
```
entry hce 0x0 hcs 0x10f cap 0x1303ff1f dma 0x8c3400000
u1 smc fmp 0 smu 0
u2 hce 0x1 hcs 0x8 force 0xde0 clkstop 0x0
u3 try 0 pre 0 uic 0 hcs 0xf
u4 link up tx 2 rx 2 post 0
u6 nop -> 0
u6 fDeviceInit done after 2 ms
hsg4 pmc: max hs gear peer 4 (0) host 4 (0), using G4 L2
hsg4 pmc: DME_SET 0xd041 -> 3, mode not changed
req 0x01 ocs 0x7 IS 0x1
u7 TUR err=0 hex=0x0 OK
hsg4 verify PA_PWRMODE 0x55 read 0 part0 30425641 01000000 00000000 00000000
```
**What it shows:**
- Peer and host both report max HS gear 4 (UIC result 0 on both reads), so G4 L2 is the target the hardware advertises.
- The 13 DME_SETs before the failing one (gear, lanes, terminations, `PA_HSSERIES`, `PA_PWRMODEUSERDATA0..5`) returned 0. This is inferred: the loop aborts on the first nonzero result and there is no per-set logging.
- The failing attribute is `0xD041` = `DME_LocalFC0ProtectionTimeOutVal`, UIC result 3. Confirmed afterwards: `ufshci.h:351` defines `UIC_CMD_RESULT_READ_ONLY_ATTR = 0x03`, so the `DME_Local*` attributes are read-only on this host.
- **Bug in my port (confirmed from `ufshcd.c`):** a failed `DME_Local*` set aborted the whole change. In `ufshcd.c` lines 4937 to 4945 Linux calls the `PA_PWRMODEUSERDATA5` and three `DME_Local*` sets as bare calls with no return check, and only acts on the result of `ufshcd_uic_change_pwr_mode`. The e2100 driver does not set `UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING` (no hit in `ufs-e2100-drv.c`), so Linux issues these sets and they fail silently there too. Still a guess: the Samsung CAL tables already write the DL/PA timeouts (`0x2041..0x2043`, `0x15B0..0x15B2`) through the SFR mirror, which would make them redundant.
- The fail-safe worked: mode unchanged. `PA_PWRMODE` reads back `0x55` (my recollection of the enum: `SLOWAUTO_MODE` = 5 in both directions, consistent with the PWM G1 link; `FAST_MODE` = 1 per 0028), TUR passes and the partition read is correct. `part0` is printed as raw little-endian words: `30425641 01000000` = bytes `41 56 42 30 00 00 00 01` = `AVB0`, version 1, the same data as the earlier `41564230 00000001` print (that one was byte-swapped) and `xxd /dev/sda23`.
- Caveat: `su_cal_pre_pmc` (HS rate-B CAL tables and the `PA_ERROR_IND_RECEIVED` mask) ran before the failing DME_SET and stays applied while the link remained in PWM. Reads were still fine afterwards; that shows this boot survived, not that those writes are harmless in general.
- `req 0x01 ocs 0x7 IS 0x1` before `u7` is the same first-TUR line as in earlier boots.
**Attempt 1's "Not done yet" list is superseded by attempt 2 below.**

**Attempt 2 (0083 v2, regenerated with `~/fix0083.py` + `~/mk0083.py`; marker `hsg4 pmc: v2`; photo IMG_0312, top of screen cut off so the kernel UUID is not visible): HS-G4 WORKED.** Change: the first 7 sets (`PA_RXGEAR`, `PA_ACTIVERXDATALANES`, `PA_RXTERMINATION`, `PA_TXGEAR`, `PA_ACTIVETXDATALANES`, `PA_TXTERMINATION`, `PA_HSSERIES`) stay fatal; `PA_PWRMODEUSERDATA0..5` and the three `DME_Local*` sets only set a bit in a `softfail` mask printed on the last line. Boot: `ufsroot=vbmeta ufshs=2`, no `UFSMAP`. Lines, `SWEET UFS: ` prefix dropped:
```
hsg4 pmc: max hs gear peer 4 (0) host 4 (0), using G4 L2
hsg4 pmc: v2 pre 0 uic 0 upmcrs 1 post 0 PA_PWRMODE 0x11 G4 L2 soft 0xe000
req 0x01 ocs 0x7 IS 0x1
u7 TUR err=0 hex=0x0 OK
hsg4 verify PA_PWRMODE 0x11 read 0 part0 30425641 01000000 00000000 00000000
```
- The mode-change UIC command returned 0, `UPMCRS` was 1 (`PWR_LOCAL`), pre- and post-PMC CAL returned 0, and `PA_PWRMODE` reads back `0x11` (FAST/FAST by 0028's `FAST_MODE = 1`; inferred, not printed by name) instead of `0x55` before.
- `soft 0xe000` = bits 13 to 15 = exactly the three `DME_Local*` sets (sequence indexes 13 to 15). All six `PA_PWRMODEUSERDATA` sets succeeded. My earlier prediction of `0x3800` was a miscount.
- `part0` is `AVB0`, version 1 (little-endian words, same bytes as before), so a real partition read works in the new mode.
- **Not proven:** "G4 L2" in the log is the requested value; `PA_TXGEAR`, `PA_RXGEAR` and the active lanes were never read back. Nothing is timed, so "faster" is an expectation, not a measurement. One 4 KiB read says nothing about stability under load.
- Not implemented: `bRefClkFreq` (Linux `ufshcd_set_dev_ref_clk`). HS rate B and FAST mode were Linux-driver assumptions; the link accepted them.

### Milestone 5d (file-backed HFS+ root with HS-G4 active): DONE on 24 Oct
**Run:** `ufsroot=userdata ufshs=1` with the `UFSMAP` ADT, built by `~/hsroot.sh` (photo IMG_0314 matches it: `ufsmap` line, `userdata`, HS-G4 lines; the photo cannot show the cmdline). **The script's terminal output (`PIN HELD` / `WARNING`) was not seen, so the pin is still unproven.** The console wrapped in the photo, so the newest text (init output) is at the top and the kernel log is below it. Seen, newest first:
```
init_program: attempting to load /usr/appleinternal/sbin/launchd.development
init_program: attempting to load /sbin/launchd
*** SWEET userspace: init running as PID 1 ***
T1 read /sbin/launchd magic: OK
T0 remount / read-write: FAIL err=2
T2 mkdir+write+readback /tmp: FAIL err=30
SWEET child: exec OK
T3 fork+exec+wait4: OK
Darwin Kernel Version 22.6.0: Mon Aug 14 05:06:07 UTC 2023; ... xnu-8796.141.3
T4 sysctl kern.version: OK
T5 mmap anon: OK
T6 read /dev/random: OK
SWEET init v2: tests done
SWEET init: still alive
```
and below that, in boot order: `SWEET CPU: 1/1 CPUs initialized`, AMFI and Sandbox stand-ins loaded, `SWEET HFS: hfs-627.100.6 registered`, then the UFS lines (`entry hce 0x0 hcs 0x10f ... dma 0x8c3404000`, `u2`..`u6`, `hsg4 pmc: max hs gear peer 4 (0) host 4 (0), using G4 L2`, `hsg4 pmc: v2 pre 0 uic 0 upmcrs 1 post 0 PA_PWRMODE 0x11 G4 L2 soft 0xe000`, `req 0x01 ocs 0x7 IS 0x1`, `u7 TUR err=0`), `ufsmap 1564 extents, 1048576 blocks`, `"userdata" on LUN 0: first block 0x33a800, 4096 MiB, read-only`, `BSD root: ufsroot0, major 1, minor 0`, `hfs: mounted SWEETROOT on device b(1, 0)`, `SWEET: root volume is not authenticated (error 25); continuing unsealed`, `Swap Subsystem is ON`.
**What it shows:**
- HS-G4 and the file-backed HFS+ root work in the same boot: the root mounts through the extent map and `sweet-init` runs from it with the HS-G4 power mode applied.
- **Correction to my earlier statement:** SULOG lines (`SWEET UFS: ...`) do print to the console and are visible in the scrollback of a booting boot, not only in the `IOFindBSDRoot` panic string. They scroll away, so a photo still has to catch them.
- T0 and T2 fail where Milestone 4 (writable ramdisk root) passed. Expected: the UFS root is a read-only block device (`DKIOCISWRITABLE` 0, writes return `EROFS`). `err=30` is EROFS by my recollection of Darwin errno values (unverified). `err=2` for the remount is unexplained: I expected EROFS or EPERM, and I did not investigate (it could be how init's `mount(2)` call names the device).
- T1, T3 to T6 pass: `/sbin/launchd` is readable from the HFS root, fork+exec+wait4, sysctl, mmap and `/dev/random` work.
- **Not shown:** read speed (never timed), the pin check line, or behavior under heavy reads. This boot did mount plus one small exec, which is a light workload.

### Milestone 6 (real iOS userland): findings so far (all read-only on the PC)
- **No APFS in the kernel** (`AppleAPFS`, `com.apple.filesystems.apfs`, `apfs_mount` absent), so the IPSW rootfs cannot be mounted directly; it has to become HFS+ (or be loaded another way).
- IPSW `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` (91 files, 7.87 GB). From `BuildManifest.plist`: `OS` = `087-86698-044.dmg` (4.73 GB, Apple DMG v4 wrapper, `apfs-fuse` reads it directly); `Cryptex1,SystemOS` = `087-86593-047.dmg` (2.74 GB, raw APFS, holds the dyld shared cache); `Cryptex1,AppOS` = `087-86715-047.dmg`; `RestoreRamDisk` = `087-86719-047.dmg`.
- Extracted to `~/ipsw/` and mounted read-only with `apfs-fuse -o ro` at `~/ipsw/mnt_os` and `~/ipsw/mnt_sys` (FUSE mounts do not survive a reboot; `du` reports 0 through FUSE, use `find -printf %s`). **The tree is under `root/`**: `mnt_os/root/{Applications,bin,cores,dev,Developer,etc,Library,private,sbin,System,tmp,usr,var}`; `mnt_sys/root/{System,usr}`.
- `mnt_os/root/sbin/launchd` is a 541,600-byte Mach-O arm64 (PIE, DYLDLINK).
- Dyld shared cache: `mnt_sys/root/System/Library/Caches/com.apple.dyld/` has 44 files, 3176 MB (`dyld_shared_cache_arm64` + `.01`...; `.symbols` 592 MB, `.42.dyldlinkedit` 334 MB, `.27.dylddata` 187 MB, many ~115 MB parts). On iOS 16 launchd expects the cryptex at `/System/Cryptexes/OS` (so for an offline merge the cryptex content goes there; my understanding, unverified).
- File sizes: OS root 8,291,487,156 bytes, cryptex 3,360,034,133 bytes (about 11.65 GB merged). OS root by directory (MB): `System/Library` 6583 (`PrivateFrameworks` 2783, `LinguisticData` 957, `Frameworks` 702, `PreinstalledAssetsV2` 660, `Fonts` 377, `NanoTimeKit` 171, `Health` 154, `Audio` 115, `OnBoardingBundles` 81, `Assistant` 73, `CoreServices` 68, ...), `usr` 542, `private` 488, `Applications` 217, `Library` 68, `Developer` 0.
- Trim idea (not tested): keep cache (minus `.symbols`, a guess that dyld does not need it), `usr`, `sbin`, `bin`, `LaunchDaemons`, `private/etc` + `var` skeleton; drop `LinguisticData`, `PreinstalledAssetsV2`, `NanoTimeKit`, `Health`, `Audio`, `OnBoardingBundles`, `Assistant`, `Applications`, probably `Fonts`. Minimum about 3.2 GB (estimate); about 6.7 GB if `Frameworks`/`PrivateFrameworks` are kept. The 4 GiB `iosroot.img` therefore fits only a trimmed root; a larger file would need another `fallocate` and a new extent map.
- PC disk: `/` has 73 GB free; `/tmp` is a 5.5 GB tmpfs (do not build the root there).
- Remaining hurdles after the root exists: HFS+ conversion (hard links, symlinks, xattrs, permissions; plan: loop-mounted non-journaled HFS+ on the PC), the root is read-only (no XNU write path), launchd will spawn daemons needing IOKit services the S21 does not have, and PWM G1 is very slow for page-in from a multi-GB root.

### Milestone 6b (trimmed iOS root): image built on the PC and written to the phone, DONE (the text below was written before the write; the result is in this section's later lines and in the Latest session update)
**`tools/mkiosroot.sh` already existed** (135 lines, listed in the README): `mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE=16G]`. What it does (from reading it): apfs-fuse (as root) mounts the IPSW dmgs, `mkfs.hfsplus -s -J` makes a **journaled, case-sensitive** HFS+ image, loop-mounted with `-o force`, `rsync -aH --numeric-ids` copies the OS root, puts the SystemOS cryptex at `/private/preboot/Cryptexes/OS` (the OS root's `/System/Cryptexes/OS` is a symlink to it), hard-links the cryptex shared cache into `/System/Library/Caches/com.apple.dyld` (its comments say dyld at ignition level 1 looks there), hard-links cryptex files that have no OS-root counterpart to their root paths (comment: dyld does not search the cryptexes at that level), creates `_logd` (uid 272) log directories, writes a plain-plist system keybag `/private/var/keybags/systembag.kb` (for keybagd with `/defaults/no-effaceable-storage`, set in `tools/mkadt.py`), then `fsck.hfsplus`. Xattrs are NOT copied (`-aH`, no `-X`); whether anything needs them is unknown.
**Sizes** (`~/iossize.py`, lstat, hard links counted once): cache 3176 MiB in 44 files (one `.symbols` file is 565 MiB); `/private/var` is 488 MiB of which `staged_system_apps` is 487 MiB (what it is: unverified, my guess is first-boot app staging launchd does not need); every component reported 0 hard-linked paths (whether apfs-fuse reports link counts honestly was not verified). Scenarios vs 4 GiB, before HFS+ overhead: S1 (cache without `.symbols`, `usr`, `sbin`, `bin`, `private`, LaunchDaemons) 3.57 GiB (+0.43 headroom), S2 (S1 + CoreServices + Frameworks) 4.33 GiB (-0.33), S3 (+ PrivateFrameworks) 7.13 GiB (-3.13).
**What was actually built** (filter `~/trim.filter` excludes `/private/var/staged_system_apps`, `/Applications`, `/Developer`, and under `/System/Library`: `PrivateFrameworks`, `Frameworks`, `LinguisticData`, `PreinstalledAssetsV2`, `Fonts`, `NanoTimeKit`, `Health`, `Audio`, `OnBoardingBundles`, `Assistant`; the cryptex copy excludes `*.symbols`, which matched 2 regular files, the second was never inspected; the App cryptex is dropped): dry run OS root 1,183,404,197 bytes + cryptex 2,767,408,469 bytes = 3,950,812,666 bytes. `tools/mkiosroot-trim.sh` was derived by `~/mkderive.py` (3 args, size default 4G, no appos, `--exclude-from` on the OS copy, `--exclude='*.symbols'` on the cryptex copy, cryptex loop `for c in OS`).
**Result:** `~/ios-sweet/iosroot-trim.img`, 4294967296 bytes (3.8G on disk). No rsync errors; the build log shows 1407 `linked` lines. The first `fsck.hfsplus` pass repaired the root folder's `HasFolderCount` flag and folder count (should be 11; cause unverified, my guess is the Linux hfsplus driver does not maintain them); the recheck and the `NO WRITE` pass say `appears to be OK`. Read-only mount: used 4,081,651,712 bytes, **available 213,315,584 bytes (203 MiB, 96 percent full)**, so HFS+ overhead was a little over 3 percent. Present: `/sbin/launchd` (541,600 bytes), `/usr/lib/dyld` (922,912), `SystemVersion.plist` (574), `/System/Cryptexes/OS` symlink, `systembag.kb` (227 bytes, mode 0600, in a 0700 directory), top level `bin cores dev etc Library private sbin System tmp usr var`. Both cache directories hold 43 files with link count 2 (hard links worked).
**What launchd and dyld say** (`strings`, not run): launchd carries `mount-phase-1`/`mount-phase-2` entries that run `/sbin/mount` on the `/private/preboot/Cryptexes/{App,OS}/System/Library` paths, needs entitlements such as `com.apple.rootless.storage.early_boot_mount` and `system-mount-authority`, prints `Loading Cryptex overlay plist from: %s`, and references `/private/var/db/com.apple.xpc.launchd`, `/private/var/db/launchd.db` and `/private/var/log/com.apple.xpc.launchd`, so it will try to write under `/private/var` and the UFS root is read-only. dyld has cryptex handling and `DYLD_SHARED_CACHE_DIR`. Whether the stand-in AMFI accepts the real launchd, and whether those mounts succeed on this kernel, are unknown.
**Risks recorded before the write:** (1) every HFS+ root this port's XNU has mounted so far was a plain non-journaled, case-insensitive `mkfs.hfsplus` 64 MiB image; this one is journaled and case-sensitive, so a panic at the `hfs:` mount is possible (it would not be a launchd problem); (2) the pin is unproven (see below); (3) the write replaces what is inside `iosroot.img`.
**The write (`~/wroot.sh`, not confirmed executed):** it checks the image is exactly 4294967296 bytes, notes whether `~/ios-sweet/hfs-root.img` exists (the known-good 64 MiB root; **the output seen did not include that line, so its presence is unconfirmed**; it can be rebuilt with `~/s21hfs.sh`/`~/s21hfs2.sh`), mounts the image read-only and lists the keybags directory, then over ssh (one password, a ControlMaster connection) lists the target and takes `filefrag -v` into `/tmp/ffA.txt`, asks `Type yes to overwrite:`, streams `cat IMG | ssh ... dd of=/mnt/android-data/linux/iosroot.img bs=1M conv=notrunc,fsync`, then takes `/tmp/ffB.txt`, prints `SAME_EXTENTS` or `EXTENTS_CHANGED`, and prints the sha256 of the first 4294967296 bytes on the phone and of the local image (they must match). **Before-state seen:** `/mnt/android-data/linux/iosroot.img` `-rw------- root`, 4294967296 bytes, dated Oct 24 22:28; `/dev/sda34` (107G, 89G used, 19G available) mounted at `/mnt/android-data`; `filefrag`: 1564 extents, last one logical 1048064..1048575, physical 2075136..2075647, length 512.
**What "overwrite" means:** only the contents of that one file. It is a normal file write through the phone's Linux f2fs filesystem (same size, no truncate), not a raw write to `sda34`, so other files and the Linux root are not touched. The old contents are the 64 MiB test HFS+ root plus zeros. If the pin did not hold, f2fs may place the new data in different blocks (19G is free), which does not damage other files but makes the old extent map stale; that is exactly what `SAME_EXTENTS`/`EXTENTS_CHANGED` detects, and `hsroot.sh` builds a fresh map anyway. If the write is interrupted the file holds a mix of old and new data: re-run it.

### Milestone 6c (0084 RAM write overlay): DONE as of 25 Oct (built and booted; correct operation not yet confirmed)
**Goal:** let a read-write HFS root write somewhere without ever touching the disk. **Patch:** `0084-sweet-ufs-ram-overlay.patch` (generated by `~/mk0084.py`, 151 lines added, 4 removed, round trip OK; reviewed by reading, then built by CI; kernel marker `ufsov: RAM overlay`). **Boot-args:** `ufsov=1` turns it on, `ufsovmb=N` sets the size (default 64 MiB, clamped to 8..512).
**Design:** a directory of `uint32` per 4 KiB logical block of the device (4 MiB for the 1048576-block image) in contiguous kernel memory; overlay pages in 1 MiB chunks. A write copies into the overlay (partial blocks first read the old block from the device, then merge); a read reads the device and then patches overlaid blocks over the result; `DKIOCISWRITABLE`, `DKIOCGETMAXBLOCKCOUNTWRITE` and `DKIOCSYNCHRONIZE` answer for a writable device only when `ufsov` is on; `open(FWRITE)` is allowed. Full overlay returns `ENOSPC` and logs `ufsov: overlay full` once. All of it is lost at reboot. **Safety:** there is no UFS WRITE command anywhere in the driver, so nothing can reach `sda`; the generator refuses to emit added code that mentions `0x2a`, `0x8a`, `ufs_scsi`, `dw0` or `UTRLDBR`. The write path is still the one thing never approved for the disk, and it stays absent.
**History:** the first generator run stopped with `ANCHOR write branch: expected 1 match, found 3` (see the generator lessons above); fixed by `~/fix0084.py`. Local commit `3ab3b3e`; the push was rejected because `s21xnu/main` had a README-only commit `1e93027` (authored "Tom", 12:35 UTC, probably from the GitHub web editor); `git pull --rebase --autostash` then push worked. The rebased hash of 0084 was not printed (inferred from the later push range `ea46b51..3ee8081`: probably `ea46b51`).
**Not verified:** that the overlay was active in the iOS-root boots (the `SWEET UFS: ufsov: RAM overlay on, ...` line scrolled away in every photo); that the HFS mount is read-write because of it (the `hfs_mark_inconsistent` messages in IMG_0338 suggest a writable mount, a guess); any behavior when the overlay fills.

### Milestone 6d (first boot of the trimmed iOS root): launchd ran; AMFI panic found and symbolicated
**Boot (photo IMG_0326, kernel UUID `3A68E6D2-99CE-3D90-B8B0-26917BCBF688`, the 0084 build):** the journaled, case-sensitive HFS+ root mounted and the kernel read from it (the panic header shows `OS version: 20H392`, which comes from the root's `SystemVersion.plist`), real launchd ran and spawned helpers, then: `panic(cpu 0 caller 0xfffffe0007af4e10): launchd_cache_loader[21] exited -- exit reason namespace 7 subcode 0x1 description: Unable to find AMFI`, followed by `handshake structure not initialized` (a generic kernel panic-debugger string, not part of the reason).
**Backtrace (symbolicated with the fixed `sym.py`, UUIDs matched):** `ast_taken_user` -> `bsd_ast` -> `postsig_locked` -> `exit_with_reason` -> `proc_exit` -> `proc_exit.cold.1` (panic caller) -> `panic_trap_to_debugger`. So a process exit with a reason panics the kernel on this path. Why `proc_exit` panics for this process was not investigated (guess: launchd marks the early-boot helper as must-not-fail).
**Cause (verified by string search and disassembly):** `Unable to find AMFI` exists in `/usr/libexec/launchd_cache_loader` and `/usr/libexec/amfid` and **not** in the kernel. The loader calls `IOServiceMatching("AppleMobileFileIntegrity")` and `IOServiceGetMatchingService`; 0 means the "Unable to find AMFI" noreturn helper. The existing SWEET AMFI (patches 0025, 0035, 0036, 0050) was only a MAC policy and an internal interface, not an IOKit service. Then (from the disassembly) `IOServiceOpen(..., type 0)` and **`IOConnectCallMethod(conn, selector 6, scalar {1} x1, struct = cdhash)`**; return 0 = "cdhash is trusted", `0xe00002f0` = "not in trust cache", anything else = "Unknown error". The loader's only sysctl string is `kern.bootargs`.

### Milestone 6e (0085 AMFI IOKit service; real launchd boots userland): DONE as of 25 Oct
**Patch:** `0085-sweet-amfi-iokit-service.patch` (generated by `~/mk0085.py`; adds `iokit/Kernel/arm/SweetAMFI.cpp` with class `AppleMobileFileIntegrity` (IOService, provider `IOResources`, registered via a personality in `iokit/KernelConfigTables.cpp`, built via `iokit/conf/files.arm64`) and `AppleMobileFileIntegrityUserClient`; selector 6 returns success for every cdhash, everything else is `kIOReturnUnsupported`, every call is logged as `SWEET AMFIKIT: <proc> sel <n> arg0 <v> in <scalars>/<struct bytes> out <scalars>/<struct bytes> -> <ret>`). Copies the pattern of 0038 (AppleKeyStore). 97 lines added, 0 removed. Pushed as `3ee8081`. This is a bring-up stand-in: it reports every cdhash trusted, i.e. there is no trust-cache enforcement in userland.
**Boot (photos IMG_0330, IMG_0332, IMG_0334, IMG_0338; kernel UUID not visible in any of them; the `PIN HELD`/`WARNING` line of the extent check was not seen, so the pin is still unproven for this boot):**
- IMG_0330 (launchd boot tasks): `exec tzinit` with `tzinit: Bad zoneinfo link : No such file or directory`; `exec FinishRestoreFromBackup`, `finish_demo_restore`, `sysstatuscheck`, `prng_seedctl` with `kern.prng.user_reseed_count: (-1) (2) No such file or directory`, `failed to load kernel prng seed: (-76) (2)`, `failed to load virtual random: (-147)`; `exec launchd_cache_loader`; `Found valid port` lines; `Using default cache paths`, `Code: /System/Library/xpc/launchd.plist Sig: /System/Library/xpc/launchd.plist.sig`; **`SWEET AMFIKIT: launchd_cache_loader sel 6 arg0 1 in 1/20 out 0/0 -> 0x0`** (so the struct input is 20 bytes, matching a SHA-1 length cdhash, which confirms the disassembly reading); `cdhash: {length = 20, bytes = 0x17f4f2e7...28bc2} is trusted`; `Attached signature to file, checking ...`; `Trying to send bytes to launchd: 2307 16384`; `Sending validated cache to launchd`; `Cache sent to launchd successfully`; then launchd `(boot) <Notice>: Early boot complete. Continuing system boot.`, `exec xpcproxy` several times and **`exec SpringBoard`**.
- IMG_0332 (later `exec` lines): `ptpd`, `thermalmonitord`, `driverkitd`, `locationd`, `usermanagerd`, `configd`, `rapportd`, `fseventsd`, `bluetoothd`, `fairplayd.H2`, `nfcd`, `logd_helper`, about 60 `xpcproxy`, `notifyd`, `IOMFB_FDR_Loader`, **`amfid`**, `mediaserverd`, `sharingd`, `DumpPanic`, `UserEventAgent`, `logd`, `keybagd`, `remoted`, `watchdogd`, `containermanagerd`, `backboardd`, `timed`, `distnoted`, `lockdownd`; and **`SWEET AKS: keybagd sel 0 arg0 0 in 0/0 out 0/0 -> 0x0`** (the 0038 keystore stand-in answering the real keybagd).
- IMG_0334: same list plus launchd errors `Missing executable detected` for `com.apple.identityservicesd` (`/System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app/identityservicesd`), `com.apple.imagent` (`.../IMCore.framework/imagent.app/imagent`) and `com.apple.AppleCredentialManagerDaemon`. Expected: `PrivateFrameworks` and `Frameworks` were excluded when the root was trimmed to fit 4 GiB.
- IMG_0338 (newest text; this is where the console ends up): see Current blocker below.
**What it shows:** the whole chain works: UFS at HS-G4 -> extent map -> HFS+ journaled/case-sensitive mount -> real launchd -> cache loader/AMFI service -> launchd early boot complete -> mass daemon spawn. **What it does not show:** which daemons stay alive (the `exec` lines only show spawns; the source of those prints, kernel or shim, has not been checked); whether `amfid` called the AMFI service and with what selectors (no `AMFIKIT` line for it was visible); whether SpringBoard or backboardd draws anything; whether the overlay is active; stability under the load of this many processes.

### Theories tested and dropped (do not revisit)
- Bouncing UFS DMA through `ufs-dma@a8000000`; IOMMU/SysMMU in the UFS path; TCXO shift/sysreg IOCC value; missing cache maintenance (0028 already uses `flush_dcache64`); chained fixups in the kernel.

### Open items
- **HS-G4 follow-ups** (the change itself works, see 5c and 5d): (1) no read-speed measurement exists; add a timed multi-MiB read to the verify path (via `mk0083.py`) and compare `ufshs` unset vs 2. (2) Read back and print `PA_TXGEAR`, `PA_RXGEAR`, `PA_ACTIVETXDATALANES` and `PA_ACTIVERXDATALANES`, because only the requested G4 L2 is logged today. (3) No heavy-read test yet: a real iOS root will page a lot; for example have `sweet-init` read many MiB of a large file, print a checksum and compare with `sha256sum` on Linux. (4) `bRefClkFreq` stays unset; only a suspect if HS misbehaves.
- Milestone 5b/5d boot results are known (mounted `SWEETROOT`, PID 1, now also with HS-G4). Still unseen: the `SAME_EXTENTS` output (go.sh) and the `PIN HELD`/`WARNING` line (hsroot.sh), so the pin is unproven. Before trusting the map after any further write, re-run `filefrag -v` on the phone and compare with the listing taken before the write (`/tmp/ff.txt`; if it is gone, take two fresh listings around the next write).
- Rebase the skipped patches onto the new 0028: 0047, 0054, 0060, 0072, 0077 fail on context near the top of `sweet_ufs.c`; 0030 (writes), 0069 (faster), 0082 (diag) edit rewritten functions and need redoing; 0034 is obsolete. Check the CI "Skipping patch" lines first.
- **Intermittent early crash seen once (IMG_0294):** `Kernel data abort`, `pc` in `_strlen`, from `_sdt_load_machsect` <- `_dtrace_module_loaded` <- `_dtrace_postinit` <- `_bsd_init`+0x514, `far 0x0000ffffa3bb0c30`; before any UFS code, non-deterministic, has not recurred over the later boots. Untested ideas: boot without `keepsyms=1`; a device (e.g. wifi DMA) still writing into the kernel's physical range after kexec; how the shim places and zeroes the kernel image.
- HCE/HCS immediately before kexec is unknown; at XNU entry HCE is 0 and HCS `0x10f` (consistent with the Linux shutdown hook stopping the host, inference).
- Patches 0060 to 0081 hardcode Qualcomm MMIO addresses; skipped in CI now.
- Framebuffer photos are the only log, cut by the rounded corner, and the console can wrap so the newest text lands at the top of the photo (IMG_0314). Panic strings are the reliable channel. SULOG prints to the console scrollback on a booting boot too (seen in IMG_0314) but scrolls away; a diag that forces the `IOFindBSDRoot` panic (`ufshs=2`) is how to keep it on screen.

- **[PARTLY SUPERSEDED 25 Oct: 0084 adds a RAM-only write overlay, see Milestone 6c; whether the mount is rw and big enough is unverified] The root is read-only, which blocks a real launchd.** `sweet-init` T0 (remount rw) and T2 (write under `/tmp`) fail on the UFS root. A real launchd wants a writable `/private/var`. Options, none investigated: redo the skipped 0030 (writes) onto the new 0028 (0031 `read-write-root` is not in the skipped list and what it does with a read-only `ufsroot` is unchecked), or give `/private/var` and `/tmp` a memory-backed filesystem mounted by init (whether this XNU build has one is unverified). Any write path to `sda` still needs my explicit approval of the exact target.
- **[SUPERSEDED 25 Oct: it has now mounted it and ran real launchd from it, see Milestone 6d; the HFS node errors under Current blocker are a new problem] The kernel has never mounted a journaled, case-sensitive HFS+** (the trimmed iOS image; earlier roots were plain 64 MiB `mkfs.hfsplus`). If it fails, expect a panic at `hfs:`. Keep `hfs-root.img` as the fallback and confirm it exists: `ls -la ~/ios-sweet/hfs-root.img`.
- **Trimmed root write is now confirmed** (Milestone 6b): `SAME_EXTENTS`, 1564 extents retained, and phone/local SHA-256 match (`824ffa37…c9a963d`). The extent pin held for this overwrite.
- Never-inspected details: what the second `*.symbols` file in the cryptex is; what `staged_system_apps` really is; whether xattrs matter; whether apfs-fuse link counts are honest.

### Next steps (current order)
1. **Find out where the `hfs_swap_BTNode` errors come from** (Current blocker, below). PC-only first: inspect catalog node 3766 inside `~/ios-sweet/iosroot-trim.img` (parse the HFS+ volume header, find the catalog file's extents, read the node at `3766 * nodeSize`; the kernel reported `blockSize 0x2000`) and say whether it is a valid B-tree node. Invalid in the image = an image/build problem; valid in the image = a runtime read/write problem.
2. Boot variants, **one variable at a time** (copy `~/ovroot.sh`, edit the cmdline, see Commands): (a) `ufsovmb=512` (rules out a full 64 MiB overlay); (b) `ufsov` and `rootrw` removed (read-only root, rules the overlay in or out); (c) `ufshs` removed (PWM G1, rules HS-G4 under load in or out). Photograph each; the newest text is at the top of the photo if the console wraps.
3. If the first error is still out of reach (photos only show the tail), add a small diag patch (0086): log the first read or write error, the first `ENOSPC`, and the free overlay pages, into SULOG, and consider forcing the `IOFindBSDRoot`-style panic on the first anomaly so the string is on screen.
4. When the root is stable: read what `amfid` asks AMFI for (`SWEET AMFIKIT` lines), what `keybagd` asks (`SWEET AKS` lines), and whether `backboardd`/`SpringBoard` stay up or crash-loop.
5. Missing `PrivateFrameworks` executables (identityservicesd, imagent, AppleCredentialManagerDaemon) need a bigger root image (a larger pinned file, new extent map); not now.
6. HS-G4 follow-ups: timed read, gear/lane read-back, a heavy-read test.
7. Rebase the skipped patches; investigate the DTrace/`strlen` crash if it recurs.

### Rules
- Keep answers short. Give exact commands. If a file has not been read, say so. Mark guesses as guesses. Never write to a partition without my explicit approval of its exact name.

---

## Latest session update (25 Oct 2026, evening)

### What happened this session (compressed)
1. Trimmed iOS root written to the phone and verified earlier the same day (Milestone 6b: `SAME_EXTENTS`, 1564 extents, matching SHA-256 `824ffa37...c9a963d`).
2. `0084` RAM overlay: generator fixed, patch reviewed, committed, pushed (rebased over a README-only remote commit), built (Milestone 6c).
3. First boot of the iOS root (`bash ~/hsroot.sh` with the 0084 kernel and `ufsov=1 rootrw=1`): panic in `launchd_cache_loader`, `Unable to find AMFI` (Milestone 6d). Cause found by string search and loader disassembly.
4. `0085` AMFI IOKit service: generator, review, push, CI (markers verified), boot via `bash ~/ovboot.sh` (Milestone 6e): launchd reached `Early boot complete`, spawned the daemon set, `SpringBoard`, `backboardd`, `amfid`, and the console ended in an HFS error loop.

### Current blocker: endless HFS+ node errors (IMG_0338)
Repeating block, same numbers every time (the console is full of it; the newest text is at the bottom of this photo):
```
hfs: node=3766 fileID=4 volume=iOSRoot device=root_device
hfs_mark_inconsistent: Runtime corruption detected on iOSRoot, fsck will be forced on next mount.
hfs_swap_BTNode: offset #0 invalid (0x0000) (blockSize 0x2000 numRecords 0)
```
- **Facts from the photo:** volume name `iOSRoot`, device `root_device`, node 3766 of file ID 4 (by HFS+ convention, my recollection, fileID 4 is the catalog file), node size 0x2000, `numRecords 0` and offset #0 = 0, i.e. the node looks zero-filled or uninitialized. The same node repeats, so something retries it.
- **Not known:** the first occurrence (it scrolled away); whether the machine is hung or still doing other work; whether the node is invalid on disk, wrongly read, or wrongly written; whether the overlay is on and whether it filled up. `fsck will be forced on next mount` is irrelevant for the disk: nothing reaches `sda`.
- **Hypotheses (all guesses, in the order of what is cheapest to test, none checked):**
  (a) The node really is zero or invalid in the image. Test: inspect node 3766 in `iosroot-trim.img` on the PC (Next step 1). The image passed `fsck.hfsplus`, so this is unlikely.
  (b) Overlay problem: 64 MiB is small for journal replay plus launchd and ~60 daemons writing, a full overlay returns `ENOSPC`; or a bug in `su_ov_patch_read` / `su_ov_write` (reviewed by eye only). Test: `ufsovmb=512`, then no overlay with a read-only root.
  (c) Read problem under load at HS-G4 (heavy reads were never tested; earlier boots were light). Test: `ufshs` removed.
  (d) Stale extent map (the `PIN HELD` line was not seen for this boot). Test: read the `ovroot.sh` output for `PIN HELD` or `WARNING`; re-take `filefrag -v` on the phone and compare with `/tmp/ff.txt` or the listing from the last write.
- Optional safety re-check after the boots: SHA-256 of the first 4294967296 bytes of `iosroot.img` on the phone against `824ffa377597fdea4158d90b29d2528607a05483f816b69dacb9230c6c9a963d` (XNU has no write path, so this should be unchanged; the check was not run).

### Other observations to keep
- `tzinit`, `prng_seedctl` and the "launchd/virtual random" failures are first-boot state missing under `/private/var` and `/System/Library/...`; they did not stop launchd.
- `exec <name>` lines are console prints; their source was not checked.
- `0085` makes every cdhash trusted; `amfid` is exec'd and its AMFI selectors have not been seen.
- Not yet inspected: what `launchd` does when a job's executable is missing (it logged and carried on for the three above).

### Immediate state / queue
1. Do not rewrite `iosroot.img`; it holds the verified trimmed root.
2. Next step 1 (PC-only inspection of catalog node 3766), then the boot variants in order.
3. Keep `sweetlog=0`. Never write to `sda` or any partition directly; the only write allowed is the earlier Linux `dd` into the single file `iosroot.img`.

---

## Latest session update (26 Oct 2026, current screen unchanged)

### Current state: minimal iOS boot; backboardd/display bring-up is the active investigation

**User report:** the phone is still showing the same screen as the supplied photo. Treat that as the current state; no later boot output or new photo has been supplied.

The current photo/log shows:
- `backboardd` reaches the SWEET framebuffer stand-in and calls selectors `57, 15, 22, 29, 33, 79, 74, 12, 6, 27, 17, 84, 50`; each shown call returns `0x0`.
- `SWEET SURF: backboardd sel 13` appears, the first recorded IOSurface request from `backboardd`.
- `MobileGestaltHelper` calls framebuffer selectors 8 and 28; both return `0x0`.
- `SWEET AKS: SpringBoard sel 35 ... -> 0xe00002c7` remains unsupported.
- `SWEET UFS: ufsdiag86 overlay 25% full (32894/131072 pages)` is shown. The `131072` page count is consistent with a 512 MiB overlay; this is an inference from 4 KiB pages, not proof of the boot arguments.
- `SWEET AMFIKIT: lsd sel 6 ... -> 0x0` appears near the bottom.
- No `SWEET FB: 100 swaps` line and no `SMP backboardd ...` sampler line are visible in this photo.
- The repeated `hfs_swap_BTNode` node-3766 loop from IMG_0338 is absent from this current screen. The pasted session notes say it did not reproduce on the minimal image (`iosroot-min.img`), with a 512 MiB overlay.

**Interpretation / limits:** backboardd is opening/configuring the framebuffer service and making an IOSurface call, but this does **not** establish that SpringBoard is rendering frames. The absence of the swap counter and sampler output in one photo does not prove a stall; the sampler's first sample is delayed by 90 seconds. Selector 35 remains unsupported but has not been shown to stop boot. The screen has reportedly remained unchanged, but the exact elapsed time on this boot was not supplied.

### 0087 status: proposed, not verified

The pasted notes contain a proposed generator `~/mk0087.py` for `0087-sweet-accelerator-iokit-stub.patch`. It would register a logging-only `IOAcceleratorES` stand-in and return `kIOReturnUnsupported` from its user client. The notes explicitly say the generator was not built or tested in the previous workspace. **No `mk0087.out`, successful real-tree `git apply --check`, commit, push, CI run, `SWEET ACCEL` marker check, or boot with 0087 has been supplied.** Do not assume 0087 exists on the PC or that it has been applied. Its proposed stub also does not cover `IOAcceleratorESLegacy`.

### Immediate next action

1. Since the phone is still on the same screen, first let this boot sit for about **3 minutes total from the last boot**, if that has not already elapsed, then photograph the newest/bottom-of-screen text.
2. Interpret the result:
   - `SWEET FB: 100 swaps` (or a higher count): framebuffer swaps are happening; next try a boot-argument-only `sweetfb_noconsole=1` variant to see whether console text is covering the GUI.
   - `SMP backboardd tN pc ...` with no swaps: capture the sampler lines plus the `Kernel text base` / Mach-O header information needed to symbolicate the PCs.
   - Neither line and still unchanged after the wait: report the approximate total time on this screen; investigate why the sampler/swap counter is not appearing before making another patch.
3. Do **not** start by committing/pushing 0087. If proceeding with it later, first verify the generator and patch against the real working tree, fix the `%llu` format arguments with explicit `(unsigned long long)` casts as a precaution, confirm CI did not silently skip the patch, and grep the built kernel for `SWEET ACCEL` before booting.
4. Keep the HFS node-3766 investigation parked for the minimal boot unless that loop returns. Do not rewrite `iosroot.img` or write to any partition. Keep `sweetlog=0`.

### Current handoff state

- Latest photo: unchanged screen described above.
- Active question: is `backboardd` actually producing framebuffer swaps, or is it looping before the first draw?
- Last verified major milestone remains 0085: real `launchd` reached `Early boot complete` and spawned SpringBoard/backboardd and many daemons on the full trimmed root.
- 0087 and all actions after the proposed generator remain unverified.



---

## Latest session update (27 Oct 2026): S9 comparison, `lsd` disassembly, current display investigation

### New evidence received

- S9 `NOTES.md` reportedly shows the same `backboardd`/AppleCLCD/IOSurface selector sequence and failed `IOAcceleratorES` / `AGXAccelerator` lookups as S21+. S9 did **not** add an accelerator stub. Treat 0087 as unnecessary unless new evidence shows otherwise.
- S9 got from the logo/progress screen to pixels after migration. Its progress bar took roughly **30–60 minutes** on `cpus=1`; patch 0056 reduced framebuffer-present overhead by about half, but progress could still crawl near 99%. Do not declare a stall after only a few minutes.
- S9 route includes ADT `/chosen/display-scale=3`, `/chosen/unique-chip-id` and `chip-id`, AKS selector 35 answered like selector 17, selector 101 success, `sweet-root-tweaks.py` migration/setup/audio/CommCenter changes, and patches 0050, 0057 and 0068. These are source-derived S9 findings and must still be verified against the actual S21+ ADT/kernel/root.
- `chk31.out`: files for patches 0048, 0050, 0052, 0056, 0057, 0064, 0068 and 0070 exist in `~/ios-sweet/xnu/patches/`. `grep -a -c sweetfb_hz xnu/out/kernel.development.sweet` returned `0`. This is evidence only about that checked artifact; it does not by itself establish whether the booted kernel differs or whether the argument is implemented. `~/mktweak.sh` references `~/sweet-root-tweaks.py` at lines 3 and 6.
- Root inspection: `/Applications` is absent from the trimmed root; `/private/var/containers/{Bundle,Data,Shared,Temp}` exist. S9's root was 16 GiB and retained `/Applications`; the trimmed S21+ root reportedly had about 115 MB free and omitted `/Applications` and some PrivateFrameworks. This difference could contribute to `lsd` failure but is **not proven causal**.
- `ipsw dyld disass` is a dead end on this machine because `ParseAllObjc` attempts an approximately 21 GB allocation. A custom Capstone script found `0x181c56f00` in `dyld_shared_cache_arm64.01` at file offset `0x1bfaf00`. Disassembly around the reported crash:
  ```asm
  0x181c56f60 bl   #0x181c88b40
  0x181c56f64 mov  x21, x0
  0x181c56f68 ldr  x1, [sp, #8]
  0x181c56f6c mov  x0, x20
  0x181c56f70 bl   #0x181d275e4
  0x181c56f74 ldr  x8, [x19, #0x28]
  0x181c56f78 ldr  x8, [x8, #8]
  0x181c56f7c strb w21, [x8, #0x18]
  ```
  The supplied crash address is `0x181c56f74` (+116). The old guess that this is directly an `objc_storeStrong` fault is not established: at the supplied address the instruction is `ldr x8, [x19, #0x28]`, following a call at `0x181c56f70`. Need actual exception PC/registers and caller context to identify the invalid pointer. The custom parser's mapping-header assumptions are not independently validated.
- Previous screen notes: `SWEET UFS: ufsdiag86 overlay 25% full (32894/131072 pages)` was visible, consistent with a 512 MiB overlay using 4 KiB pages, but this does not independently prove the boot args. No `SWEET FB: 100 swaps` or `SMP backboardd` sampler line was visible in that photo. The sampler's first sample is delayed; absence from one photo does not prove no swaps or a hang. The node-3766 HFS loop was reportedly absent on `iosroot-min.img` with a 512 MiB overlay.

### Current interpretation

1. Failed accelerator lookups are not, by themselves, a blocker: S9 saw them too and reached pixels without an accelerator stub.
2. The long migration wait is a serious confounder. S9's 30–60 minute duration means a short wait cannot distinguish slow migration from a stall.
3. S21+ AKS selector 35 still returned `0xe00002c7` twice in the supplied screen. Compare the S9 implementation with the exact source, built artifact and booted kernel before patching.
4. The root trim is materially different from S9's 16 GiB root, but no evidence yet proves `/Applications` or missing PrivateFrameworks caused the `lsd` crash.
5. The earlier HFS node-3766 loop appears specific to the previous full-root run in the notes; keep it parked unless it returns.

### Next actions (updated priority)

1. **Verify ADT first:** inspect the actual generated ADT and confirm `/chosen/display-scale`, `/chosen/unique-chip-id`, and `/chosen/chip-id` are present and appropriate. Do not infer from source alone.
2. **Verify AKS selector 35:** inspect the S21+ implementation and build/CI artifact. Confirm the relevant marker is present in the exact kernel used for the next boot.
3. **Use a long observation window:** record the starting time and let the same migration boot run for up to 30–60 minutes if it remains stable. Capture periodic screen photos; do not change multiple variables during this test.
4. **Check framebuffer activity:** look for `SWEET FB: 100 swaps` or later counts. If the screen text is masking the GUI, only then test `sweetfb_noconsole=1`, after verifying the boot argument is supported. Do not rely on `sweetfb_hz=10` until the source and artifact prove `sweetfb_hz` is implemented; the local artifact grep returned zero.
5. **If `lsd` still crashes:** collect full exception/panic registers and the exact crash PC, then trace `x19` and the memory it points to around `0x181c56f74`. Do not claim the missing directory caused it. A root-side restore/tweak is a possible controlled test, but preserve the verified image and get explicit approval before any phone write.
6. **Do not build/commit 0087 as the next move.** The S9 comparison argues against it. Establish whether migration advances and whether framebuffer swaps occur first.
7. Keep existing safety rules: no direct writes to `sda` or partitions; the only previously approved write was Linux writing the single `iosroot.img` file. Do not rewrite that image without explicit approval.

### Current handoff state (27 Oct)

- Latest known display state: same apparent screen; no new elapsed-time measurement supplied in the paste.
- Active questions: is migration still progressing, are framebuffer swaps happening, and what invalid pointer causes the `lsd` crash?
- S9 comparison is a guide, not proof that the same fix is missing on S21+.
- 0087 remains unverified and is not the recommended next action.

## Latest continuation — 27 Oct 2026: overlay progress and SpringBoard failure photo

### New evidence (user report and attached photo)

- Reported `ufsdiag86` overlay progress is `32828/131012` pages (~25.1%). Earlier notes recorded `32894/131072`; retain both as separate snapshots because their denominators differ slightly. Neither snapshot alone proves completion or a stall.
- User reports the display slowly faded to black and then returned to logs. The supplied photo shows console output and no confirmed lockscreen. This establishes a visible transition, not its cause.
- The photo shows repeated `SWEET AKS: SpringBoard` calls, including selector 35 lines that display `-> 0x0`. This conflicts with an earlier screen note reporting selector 35 returning `0xe00002c7` twice. They may reflect different boots/stages/logging paths. Timestamp and boot/kernel identity are needed before drawing a conclusion.
- Visible SpringBoard-related output includes an `FBSDisplayConfiguration` / `SBDDisplayManager` assertion involving `oldPreferences`, failed service lookups for `com.apple.MobileTimer` and `com.apple.mobileactivationd`, and a SpringBoard termination due to `NSInvalidArgumentException`. The exception appears to mention `NSPlaceholderDictionary` initialization with a nil object at `objects[0]`; the photo resolution makes exact transcription uncertain. Obtain the raw crash/log before treating that wording as definitive.
- Other visible lines include `No such file or directory`, `Could not get the foreground UID`, `FBScene - com.apple.springboard`, `SpringBoard reason ns 2 code 0x6 (none)`, and an `EXC_CORPSE_NOTIFY` error code 5. Together these indicate SpringBoard is not reaching a stable UI, but they do not prove why.

### `chk32.out` findings supplied in the conversation

- In local `xnu/out/kernel.development.sweet`, grep counts were: `sweetfb_hz` = 0, `SMP %s` = 0, `SWEET FB:` = 7, and `kAKSGetExtDeviceState` = 0.
- The broad `SWEET FB:` hits are not proof that the 0054 swap counter is present. The zero counts for `sweetfb_hz` and `SMP %s` are consistent with 0054/0056 instrumentation being absent from this artifact; they do not establish that this is the exact kernel used for the pictured boot.
- 0054's `sweet_ufs.c` hunk adds `sweet_sample_start()` after `sweet_log_start();`. The output shows that anchor in 0054, but does not establish it exists in the S21 source/patch base. This supports a likely hunk incompatibility. It is still an inference—not confirmed CI behavior—until the current Actions log's “Skipping patch” lines or a dry-run/apply check are provided.
- 0055 touches `SweetKeyStore.cpp`, `exception.c`, and `sweet_sample.c`; 0056 touches `SweetFramebuffer.cpp`. Their being skipped remains a hypothesis pending direct build-log or apply-check evidence.
- A zero literal-string count for `kAKSGetExtDeviceState` is not meaningful by itself; it may be an enum/source symbol rather than a literal embedded in the kernel.

### Updated assessment

Keep these as distinct questions: (1) is overlay/migration still progressing, and (2) why is SpringBoard crashing instead of holding a stable UI? The current evidence does not show that one explains the other. The photo's nil-object dictionary exception is a specific lead for SpringBoard, while the missing sampler/framebuffer marker evidence is a separate build-provenance issue.

### Next actions

1. Record boot start time and elapsed time; keep periodic overlay readings and photos. Do not call 25% a hang without a stable time series.
2. Capture the full raw SpringBoard crash/exception, including backtrace/registers and exact exception reason. The apparent nil-object dictionary exception is not yet a proven root cause.
3. Paste the latest CI “Skipping patch” lines. Verify the exact kernel artifact used for this boot, then run patch checks against the actual S21 tree before editing 0054/0055/0056.
4. If repairing the patch series, generate the corrected patch and require clean ordered apply-checks before any build/commit. Do not add 0087 based on this photo alone.
5. Preserve storage safety: no direct writes to `sda` or phone partitions; do not rewrite `iosroot.img` without explicit approval of the exact target.

---
## Latest continuation — 27 Oct 2026: SpringBoard nil-dictionary exception and IconServices comparison

### New evidence supplied after the previous update

The latest user-provided `casite3.sh` output has now resolved the frame-6 class references and compared two framework directories against the real IPSW. This is the current stopping point; the script output is evidence, while causal conclusions remain hypotheses.

#### Disassembly / pointer decoding

- `~/casite.py` was patched successfully (`casite.py patched`).
- Pointer/class decoding returned:
  - `0x1dd928178` → class `IFImageBag` at `0x1e204c330`
  - `0x1dd928140` → class `NSDictionary` at `0x1e1d48108`
  - `0x1dd928410` → class `ISCompositor` at `0x1e20248c8`
- The three inspected stubs resolve to selectors:
  - `0x1a7b8c080` → `images`
  - `0x1a7b8c580` → `initWithImages:`
  - `0x1a7b8ab00` → `count`
- Combined with the earlier frame-6 analysis, the exception path appears to build a one-entry dictionary using `kISPrimaryResourceKey` and an `IFImageBag`-related value. The earlier register/argument interpretation says the value passed as the object is nil. Treat the register flow and the `kISPrimaryResourceKey` annotation as the current disassembly interpretation, not independently verified source-level semantics.
- The class lookup supports `IFImageBag` as the class at the previously unresolved reference. It does not yet establish which call returned nil or why.

#### Read-only IPSW versus trimmed-root comparison

The script mounted/used the IPSW OS volume read-only and compared file lists for these paths:

`System/Library/PrivateFrameworks/IconServices.framework`
- IPSW: 5 files
- Trimmed image: 1 file
- IPSW-only entries reported:
  - `2467 _CodeSignature/CodeResources`
  - `137 framework.sb`
  - `85 InfoPlist.loctable`
  - `1876 Localizable.strings`
- The output did not report an IPSW-only `Info.plist`, consistent with the image retaining that one file. This is a file-list/size comparison, not a test of whether any omitted file is required for the failing code path.

`System/Library/CoreServices/CoreTypes.bundle`
- IPSW: 33 files
- Trimmed image: 33 files
- The `diff` output was empty, so the compared file paths and sizes match for this directory under the script's comparison method. This does not prove every resource is semantically intact, but it removes a simple missing-file-count discrepancy for `CoreTypes.bundle`.

#### Current interpretation

- **Stronger lead:** the trimmed root omits four files present in the IPSW's `IconServices.framework`, while the exception path involves an `IFImageBag` and a nil object in a dictionary initialization. Missing IconServices resources are a plausible contributor.
- **Not proven:** no evidence yet ties any of those four files to the nil return, and `CoreTypes.bundle` does not show a path/size difference in this comparison. Do not state that restoring the files will fix SpringBoard.
- The earlier hypothesis that the absent `/Applications` directory or crashing `lsd` causes the nil remains unproven. Keep it separate from this narrower IconServices lead.
- SpringBoard's repeated `NSInvalidArgumentException` remains the immediate observed failure. `EXC_CORPSE_NOTIFY` error 5 still means the usual corpse/crash-report route has not yielded the needed trace in the supplied observations.

### Next actions — updated priority

1. **Inspect the exact call site around frame 6.** Use the current `~/casite.py` against the same shared-cache file and exact addresses from the readable raw trace, and preserve the full disassembly/register notes. Do not infer the crashing caller from strings scraped near the exception.
2. **Compare the four omitted IconServices files and their metadata with the real IPSW.** Establish whether they are required by the relevant `IFImageBag`/resource path before changing the root. The current comparison already identifies the missing entries; do not rerun it unless the mount or image changes.
3. **If testing a root-side repair, do it off-phone first.** Build a candidate image or a controlled test copy with the missing IconServices files restored, check size/available space and HFS+ integrity, and compare its contents before proposing a boot. Do not write `iosroot.img` or any phone partition without explicit approval of the exact target.
4. **Keep collecting a legible raw trace.** If the `First throw call stack` can be emitted as text, use that instead of transcribing addresses from the low-resolution video. Capture the exception reason, stack addresses, boot identity and timestamps together.
5. **Keep the earlier instrumentation/build-provenance task open but separate.** Confirm whether 0054/0055/0056 applied by checking the current CI “Skipping patch” lines and the exact artifact used for the boot. Do not treat a missing marker in a possibly different local kernel as proof about the booted kernel.
6. **Do not add 0087.** Nothing in the new evidence makes an accelerator stub the next justified action.

### Current handoff state (27 Oct, latest continuation)

- `casite.py` pointer mode works for the tested inputs and identifies the referenced class as `IFImageBag`.
- The inspected selectors are `images`, `initWithImages:`, and `count`.
- The IPSW has four additional files in `IconServices.framework`; `CoreTypes.bundle` has matching file counts and no reported size/path differences.
- Working hypothesis: the missing IconServices framework resources may be related to the nil `IFImageBag` value used in the dictionary construction. This is a testable lead, not a confirmed root cause.
- Next useful input: the full, legible frame-6 call-site disassembly and/or raw `First throw call stack`, followed by a controlled off-phone comparison/test of the missing IconServices resources.
- Safety unchanged: no direct writes to `sda` or phone partitions; do not rewrite `iosroot.img` without explicit approval of the exact target.


---

## Latest session update (27 Oct 2026, late morning): root repairs, Apple logo, new SpringBoard crash

This section supersedes the older "Next actions" and "Current handoff state" lists. Facts come from outputs or photos the user supplied; guesses are marked.

### 1. The nil `IFImageBag` chain is traced (PC-only disassembly of the dyld shared cache)
- Frame 6 (`casite2.out`, call site `0x1a7b47f98`) builds `x20 = [[IFImageBag alloc] initWithImages:[self images]]`. If `[[self images] count]` is 0 it replaces `x20` with `+[IFImageBag defaultAppClipAssetsBag]` (shape 5 or 6) or `+[IFImageBag defaultAppIconAssetsBag]`. `x20` then goes in as `objects[0]` of `dictionaryWithObjects:forKeys:count:` with key `kISPrimaryResourceKey`, count 1. Callers: `-[ISIcon prepareImageForDescriptor:]` and `-[ISIcon prepareImagesForImageDescriptors:]`.
- `defaultAppIconAssetsBag` is a lazy singleton (getter `0x1a7b47a18`, `dispatch_once` token `0x1e2025168`, global `0x1e2025160`, zero in the cache file). Its block (`0x1a7b68254`) runs `[IFImageBag imageBagWithResourcesNamed:@"DefaultIcon" fromBundle:[IFBundle mobileIconsFrameworkBundle]]`; the AppClip block (`0x1a7b68328`) uses `@"AppClipDefaultIcon"`. `IFBundle` is the class at `0x1e204c0d8`. The `IFImageBag` class methods enumerate loose image files in a bundle.
- **The missing four IconServices files were a dead end** (code signature, sandbox profile, loctable, strings). `IconServices`/`IconFoundation` hold only `Info.plist` in the IPSW too; `CoreTypes.bundle` and `IconsetResources.bundle/Assets.car` match the IPSW.
- **Root cause found:** in the image every `System/Library/PrivateFrameworks/*.framework` (1738 directories) holds only an `Info.plist` stub, because the trim excluded `PrivateFrameworks`. `MobileIcons.framework` has 108 files (323,060 bytes) in the IPSW, including `DefaultIcon-*` and `AppClipDefaultIcon-*`, and only `Info.plist` in the image. The nil is therefore explained by the missing resources (an inference from disassembly plus the file lists; the boots below are the test).
- Decoder lessons (`~/ifbag2.py`..`~/ifbag6.py`, they exec the top of `~/casite.py` and read the shared cache from the mounted image): small objc method lists in the cache use selector-name offsets from a base that must be found empirically. Anchoring on the selector `init` (string `0x182e2491f`, method IMP `0x1c85fa68c`) gives base `0x18229979f`, score 15/16, and all `IFImageBag` method names then decode correctly. An earlier base (`0x183c60c6c`) was wrong and gave truncated names. A script that reads the cache needs the image mounted at `/mnt/iosro` (`push_tw*.sh` unmounts it; remount with `sudo mount -t hfsplus -o loop,ro,force ~/ios-sweet/iosroot-min-tw7.img /mnt/iosro`).

### 2. Root repairs (all built on the PC from copies; every phone write was approved by the user for the single file `iosroot.img` only)
- `~/mk_tw6.sh`: copy of `iosroot-min-tw5.img` plus the IPSW's `MobileIcons.framework` (108 files). `fsck.hfsplus` OK. SHA-256 `debab8c2e3b53ce960ef6601ed2cd7bd1c3503f4d60bd1733c1dc168cc1e0461`.
- `~/push_tw6.sh` wrote it into `/mnt/android-data/linux/iosroot.img` (`dd conv=notrunc,fsync` over ssh, 436 s). Phone and local SHA matched; the boot script printed `PIN HELD: extents identical` (1564 extents, 1048576 blocks). The script's own `EXTENTS_CHANGED` line came from a sloppy diff that compared outputs with different headers; ignore it. Booted via `~/ovroot2.sh`.
- **Boot with tw6 (video `IMG_0417.mov`, 10 s; that it was the tw6 boot is assumed, not proven):** the old nil-`objects[0]` exception was replaced by `*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '*** -[__NSArrayM insertObject:atIndex:]: object cannot be nil'`, plus `RuntimeThemeRefForBundleIdentifierAndName() couldn't find Assets.car in bundle with identifier: com.apple.SpringBoardHome` (also `com.apple.AppPredictionUIWidget`), image-asset lookups by path in `SpringBoardHome.framework`, `BatteryCenterUI.framework`, `AppPredictionUIWidget.framework`, and `STORE_LOOKUP ... RETURNING_PLACEHOLDER` for `ISGenericApplicationIcon`. SpringBoard crash-looped (pid 57 then 58, `exec SpringBoard` each time, `reason ns 2 code 0x6`, then `0x5`).
- Size study (read-only): all non-`.lproj` PrivateFrameworks resources are 55,606 files / 2,682,116,473 bytes (about 2.5 GiB), far over the image's roughly 448 MiB free; ML/media payloads dominate (`MediaAnalysis` 245 MB, `SFSymbols` 194 MB, `TextToSpeechMauiSupport` 110 MB, plus 123 MiB of `.loctable` files). A UI-resource filter (extensions `car png plist caml nib strings stringsdict json mom omo otf ttf pdf jpg jpeg ktx icns momd`, only `en`/`Base`/`en_GB` `.lproj`, files up to 4 MiB) measured 13,851 files / 333 MiB on disk (4096-byte blocks).
- `~/mk_tw7.sh`: copy of tw6 plus that filtered set, with SpringBoard-related frameworks (`SpringBoardHome`, `BatteryCenterUI`, `AppPredictionUIWidget`, `SpringBoardUI`, `SpringBoardFoundation`, `SpringBoardUIServices`, `SpringBoardServices`, `MobileIcons`, `FrontBoardServices`, `BackBoardServices`, `FrontBoard`, `FrontBoardUI`, `BackBoardUI`) copied in full up to 4 MiB per file, `CameraEffectsKit` excluded, and a refusal if the stage exceeds 388 MiB. **`mk_tw7.out` was not shown to the assistant**; what is known is that `push_tw7.sh` ran `fsck.hfsplus -fn` on the image and printed `The volume iOSRoot appears to be OK`.
- `~/push_tw7.sh` wrote tw7 (298 s, 14.4 MB/s). SHA-256 `e17869d8dee15bd24e9930a3723e25c9d54fa6692c69653298427541623598cd` on phone and PC; `PIN HELD`; `date` printed `Tue 27 Oct 09:24:44 GMT 2026` shortly before the kexec. **The phone's `iosroot.img` now holds tw7.** Rollback image on the PC: `iosroot-min-tw6.img`.
- Image bookkeeping oddities (unexplained): before tw6 the phone's `iosroot.img` hashed to `9c6f727020d882be75af3c68cd60eaeb4ab62c97143f87edc162082e77c5584c`; no local image was ever compared with it (a later `sha256sum` reported `iosroot-min-tw5.img` and `iosroot-trim.img` missing from `~/ios-sweet`, although `ls` had shown tw5 earlier and `mk_tw6.sh` copied it). So "tw6 equals the phone's old image plus one directory" was never verified, and tw5 may be gone.
- Boot cmdline (`~/ovroot2.sh`): `-v debug=0xc serial=3 cpus=1 keepsyms=1 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0 sweetlog=0 rd=ufsroot0 ufsroot=userdata ufshs=1 ufsov=1 ufsovmb=512 rootrw=1 unentitled_ios_sim_launch=1` (the grep in the script output showed the `ufsroot=... rootrw=1 unentitled_ios_sim_launch=1` part; the rest is from the earlier notes).

### 3. The tw7 boot
- `IMG_0418`: the **Apple logo** on the S21 screen (first time). What drew it, and whether a progress bar exists, was not established (guess: it matches the S9 first-boot migration screen, where the bar took 30 to 60 minutes on `cpus=1`).
- `IMG_0420` (console again, later): SpringBoard crash-loops with a **new, deterministic crash**: pids 74, 75, 76 each die with `sig 11 code 0x100000001 sub 0x74` (invalid address, `far 0x74`), `slide 0x51bb0000`, `reason ns 2 code 0xb`, then `exec SpringBoard`. Same registers and backtrace every time (read from a photo, so single digits may be wrong): `pc 0x1dac3d3f8`, `lr 0x1dac3d3f0`, `bt 0x1daaf0cc4 0x2440456ec 0x244044f70 0x24404cc1c 0x24403fa3c 0x1dac868cc 0x1da94c0f8 0x1f1a194e0 0x21765ae34 0x217659b50`. The `insertObject:atIndex:` exception no longer appears there. `SWEET AKS: SpringBoard sel 35 ... -> 0x0` and sel 17/7 also return 0x0 in this photo.
- Not seen: any `SWEET FB` swap line, the `ufsdiag86` overlay percentage, or the boot's elapsed time at that photo. The user was leaving the phone running.

### 4. Symbolication mistake to fix first
`~/ifbag6.py` looked up the crash addresses **without removing the slide**. They landed in `dyld_shared_cache_arm64.27.dylddata` (data, so the `udf` garbage) or outside every mapping. The crash report prints `slide 0x51bb0000`; subtract it. Unslid values (computed): pc `0x18908d3f8`, lr `0x18908d3f0`; backtrace `0x188f40cc4 0x1f24956ec 0x1f2494f70 0x1f249cc1c 0x1f248fa3c 0x1890d68cc 0x188d9c0f8 0x19fe694e0 0x1c5aaae34 0x1c5aa9b50`. All fall inside the cache's mappings. `pc - lr = 8` looks odd and may be a misread digit.

### Next actions
1. Re-run the disassembly with the slide removed (window around the pc and each backtrace address) and name the function and selector that reads `[nil + 0x74]`. Then decide what resource or service is missing; do not guess from the address alone.
2. Collect from the running boot, in order: overlay percentage, whether any `SWEET FB` line appears, elapsed time since `09:24:44 GMT`. Do not reboot unnecessarily: the 512 MiB RAM overlay and any migration progress are lost on reboot.
3. If the next crash is again a missing resource, widen the restore selectively from the IPSW (the free space after tw7 is smaller than 448 MiB; check it with `df` on a read-only mount of tw7 before sizing). Keep the standing rule: build on the PC from a copy, fsck, and write the phone only after the user approves the exact target (`iosroot.img` contents via `dd conv=notrunc`).
4. Still open and separate: confirm which patches the CI skipped (0054/0055/0056 instrumentation; `sweetfb_hz` and `SMP %s` were absent from the local kernel, which is not proof about the booted kernel), `/Applications` absent from the trimmed root, and the parked HFS node-3766 loop.
5. Do not add 0087 (accelerator stub) on current evidence.

# S21+ iOS Boot Progress — 27 October 2026

## Current status

**Major milestone: the device has displayed the iOS “hello” screen with “Swipe up to open”.** This is substantially further than the earlier Apple-logo-only and console-only states. The photos also show SpringBoard running and producing UI-related diagnostics. The current evidence supports successful display of a graphical iOS screen; it does not yet establish that the device can be unlocked, complete setup, or remain stable.

## Latest evidence from the photos

### 1. `IMG_0428.jpg` — visible “hello” screen

Observed directly:
- Black screen with the white **“hello”** greeting.
- **“Swipe up to open”** and the information (`i`) button.
- Low-battery indicator at the top right.
- No verbose kernel text visible over the screen.

Interpretation:
- This is the clearest evidence so far that the display path can present a coherent iOS graphical screen rather than only console text.
- Do not call this a confirmed usable homescreen. The image shows a greeting / welcome-style screen, and the next check is whether swiping up progresses normally.
- The low-battery icon is worth noting. Keep the phone connected to power during observation, but do not infer that battery state caused the earlier crashes.

### 2. `IMG_0431.jpg` — SpringBoard sampling output

Observed directly:
- The screen contains SpringBoard sampler/backtrace-like output and ends with `--- sample 5`.
- A visible process listing includes SpringBoard PID 105 and a `taskState: Running` entry.
- UI-related names include `FluidSwitcher`, `CoverSheet`, `FBSceneManager`, and keyboard/focus-related output.
- Several frames contain address/backtrace data.

Interpretation:
- SpringBoard was alive and executing UI setup/work at the time represented by this sample.
- This image is useful context but does not, by itself, show a crash or prove that every listed component completed initialization.

### 3. `IMG_0434.jpg` — SpringBoard state and diagnostic output

Observed directly:
- The visible version line reads **iOS 16.7.16 (Build 20H39)**.
- A timestamp is shown as `12/31/69, 4:24:04 PM PST`, consistent with a clock near the Unix epoch / unset system time in this environment.
- The log mentions `dlsym ... rosetta_is_current_process_translated: invalid handle`, `Launchd could not find job`, `Wallpaper scale 1.000000`, `FBSceneManager`, and keyboard/focus-related entries.
- The screen shows many `matrix(0.17 ...)` lines.

Interpretation:
- The epoch-like timestamp is a real clue to investigate, but no evidence yet ties it causally to the earlier SpringBoard exception.
- The `dlsym` and “Launchd could not find job” lines are not proven causes of a failure.
- The matrix lines appear to be UI/layout diagnostic data; they are not, on their own, evidence of a hang.

## How this progress was achieved (as established by the current handoff)

This is a cumulative status summary from the ongoing S21+ work, not a claim that every earlier change was re-tested in this latest boot:

- The S9 baseline established that SpringBoard/backboardd could reach a stable console state while querying AppleCLCD selectors, without an IOAcceleratorES/AGXAccelerator service being found in that log. S9 also showed that the boot could be very slow on `cpus=1`; progress-bar completion could take roughly 30–60 minutes.
- S21+ boot preparation has included ADT/display-scale and platform identity considerations, root-volume tweaks, and XNU logging/diagnostic patches. The handoff records patches and experiments separately; the exact patch set present in the kernel used for these latest photos must still be confirmed.
- Earlier S21+ photos showed repeated SpringBoard crashes (`sig 11`) at reported PC `0x1dac3d3f8`, and another photo showed a transition to the Apple logo. The current “hello” screen is a better outcome than those earlier observations. It is not yet proven whether it came from the same image/boot attempt or a later tw8/tw9 iteration; record the actual boot/image identifier before attributing it to a particular change.
- The previous asset investigation found that the IPSW’s `SFSymbols.framework/CoreGlyphs.bundle/Assets.car` is about 178,049,096 bytes (~169.8 MiB), larger than the reported 4 MiB copy-filter limit. The UIKitCore top-level `Assets.car` was not present in the IPSW listing; `UIKitCore.framework/Artwork.bundle/Assets.car` was about 2.65 MB. Missing CoreGlyphs assets were a **hypothesis** for the earlier `systemImageNamed:withConfiguration:` path returning a nil asset manager, not a confirmed root cause.
- The proposed tw8 experiment was to copy only CoreGlyphs `Assets.car` into a *new image file*, validate matching hashes and filesystem health, and keep the phone’s `iosroot.img` untouched until explicit approval. This document does not assert that tw8 was successfully built or booted.

## Current best assessment

**The display/UI path has reached a welcome screen.** The priority shifts from “can it draw anything?” to “can the welcome screen advance, and is the running system stable?”

Still unconfirmed:
- Whether “Swipe up to open” responds and leads to the next screen.
- Whether Setup Assistant, SpringBoard, and required services remain alive after interaction.
- Whether this is a boot from tw8, tw9, or another root image.
- Whether the earlier SpringBoard SIGSEGV is fixed, avoided, or merely not occurring in this photo.
- Whether the system clock is being initialized correctly.
- Which exact patch set and kernel binary produced the latest screen.

## Next steps

1. **Document the current state before changing anything.** Record the boot start time, exact root image filename/hash, kernel artifact/build ID, and whether this screen is from tw8/tw9 or another image.
2. **With the phone connected to power, try the visible “Swipe up to open” action once.** Photograph the next screen and note whether it advances, stalls, reboots, or returns to verbose logs. Do not repeatedly interact if it starts a crash loop.
3. **Capture a short timestamped log around that interaction.** Look for SpringBoard/Setup Assistant crashes and the first exception/backtrace if one occurs.
4. **If the prior asset hypothesis is still being tested, verify the image contents directly.** Check whether `System/Library/PrivateFrameworks/SFSymbols.framework/CoreGlyphs.bundle/Assets.car` exists in the exact image that booted, and compare its hash to the IPSW source. Do not assume a planned copy was completed.
5. **Keep patch provenance separate from the UI result.** Obtain the latest CI “Skipping patch” lines and identify the exact kernel artifact before concluding that patches 0054/0055/0056 were applied or skipped.
6. **Preserve storage safety.** Continue to avoid direct writes to `sda` or phone partitions. Do not replace or rewrite `iosroot.img` unless the exact target and write are explicitly approved.

## Evidence ledger

| Item | Status |
|---|---|
| Coherent iOS “hello / Swipe up to open” screen | **Observed in `IMG_0428.jpg`** |
| SpringBoard PID 105 shown as Running in sample output | **Observed in `IMG_0431.jpg`** |
| iOS 16.7.16 build 20H39 | **Visible in `IMG_0434.jpg`; transcription should be checked against raw logs** |
| Epoch-like system timestamp | **Observed; cause/effect unknown** |
| Earlier repeated SpringBoard SIGSEGV at `0x1dac3d3f8` | **Observed in earlier photo; not present in the latest hello-screen photo** |
| Missing CoreGlyphs assets caused earlier crash | **Hypothesis only** |
| Latest boot used tw8/tw9 | **Unknown from current photos** |
| Usable/unlocked homescreen | **Not yet demonstrated** |

---

## Latest session update (27 Oct 2026, evening, 19:30 BST): tw10/tw11, stock SpringBoard, swipe-up still broken

**This section is the newest state and supersedes the older "Next steps" lists above it, including the "Next steps" of the "hello screen" section.** Facts come from outputs or messages the user supplied; guesses are marked. The tw8/tw9/tw10 build steps and the SpringBoard patching were done in a part of the session not visible to the writer of this note, so what is known about them is only what the user's messages say.

### 1. What is established
- **The SpringBoard crash-loop / launchd panic is fixed.** The user reports a SpringBoard Mach-O patch (dylib-style hooks: activation state, setup reason, Metal nil, auto-unlock) had been applied by the old `~/s21fix.sh`. A patched SpringBoard is not a platform binary and launchd panicked; the user says that is now fixed. The fix is `iosroot-min-tw10.img`, which holds the **stock** SpringBoard (SHA-256 `ea835980610a95591fb349bca077840fab7bdd663efaa11ef054d91bc2125475`, from the script's `STOCK_SB` constant; that tw10 really holds it was not hashed in anything seen here, the script only warns if the input differs). Whether the panic mechanism is exactly "not a platform binary" is the assistant's explanation in the session, not independently verified.
- **`~/s21fix.sh` was rewritten to be plists-only** (old copy kept as `~/s21fix.old.sh`). Usage `bash ~/s21fix.sh INPUT.img OUTPUT.img` (re-execs under sudo, refuses to overwrite the output, copies the image, mounts it `loop,rw,force`, writes two plists, checks SpringBoard's hash is unchanged, unmounts, `fsck.hfsplus -fn`). It never touches SpringBoard or `/System/Library` and never contacts the phone. The two plists, under `private/var/mobile/Library/Preferences` (owner 501:501, mode 0644): `com.apple.purplebuddy.plist` `SetupDone = True`; `com.apple.assistant.support.plist` `Assistant Enabled = False`. The dylib hooks (activation state, setup reason, Metal nil, auto-unlock) are gone with the Mach-O patch.
- **tw11 built (output seen, `~/tw11.out`):** `iosroot-min-tw11.img` from `iosroot-min-tw10.img`; both plists written; no WARNING line; `fsck.hfsplus` says `The volume iOSRoot appears to be OK` (journaled, case-sensitive). SHA-256: tw10 `698292637a5ccb29acdc8ecb914262d3aeeabd9498270f65c867b0a2b27b8036`, tw11 `8b31bc1f5778dd60eb7a7f75b71e2974eba6ea53c8b2102f65918a4607831103`.
- **tw11 was pushed and booted** (the user's wording; the `push_tw11.out` output was not supplied, so the usual `PIN HELD` line and the phone/PC SHA match were not seen by the note's writer). `~/push_tw11.sh` was made from `~/push_tw10.sh` with `sed 's/tw10/tw11/g'` and asks for `yes` before overwriting `iosroot.img`. Report after boot: **SpringBoard no longer crashes.** Whether the Hello/setup screen is now skipped was **not stated**; the user only said it still cannot be swiped up.

### 2. Current blocker: the lock-screen "Swipe up to open" gesture does nothing
- **Reported symptoms (user):** swiping works in the console (scrolls the log up and down); **tapping the `i` button on the lock/hello screen works and so does tapping "Regulatory"**. The unlock swipe does nothing.
- **Meaning:** touch down/up reach SpringBoard (taps work), and the console scroll shows the kernel side receives movement. It is not known whether **move** events reach SpringBoard as proper drag events. All of this is a guess until the HID code is read and the tests below are run.
- **Previously the only workaround was the dylib's auto-unlock**, which is gone with the Mach-O patch. So the swipe problem is separate from the crash, and it was present before (the user: "the bit i couldnt swipe up on").
- Guesses on cause (unverified; the touch code has not been read yet): the 100 px bottom edge from patch 0097 may not match SpringBoard's view of the screen (375x812 pt at 3x, per the assistant's recollection; the S21 framebuffer is 1080x2400, presented as 1125x2436 per the earlier notes); iOS 16's unlock swipe has to start in the thin system-gesture strip at the very bottom; move events may lack the right phase/range flags.

### 3. Touch patches (partly read)
- `~/touchp.out` (311 lines) was generated with the read-only block that prints `0097-sweet-hid-touch-fields.patch` and `0094-*.patch` from `~/ios-sweet/xnu/patches` plus a `ls | grep -i -E 'touch|hid|key'`. **Only its first 7 lines reached the assistant:** the header of `0097` (`iokit/Kernel/arm/hid/SweetHID.cpp`, hunk `@@ -356,6 +356,7 @@`, context mentioning "app-switcher gesture"). The patch bodies, 0094, and the list of other touch/HID patches were **not seen**. So nothing is known yet about how move events or the edge region are generated.
- Reminder from the older notes: patches 0060 to 0081 hardcode Qualcomm MMIO addresses and the CI skips several (0060 keys/touch among them). 0094 and 0097 are numbered later and are in the touch/HID area; whether they applied in CI and whether the booted kernel contains them has not been checked (grep a marker string from them in the exact kernel).

### 4. Next actions (current order)
1. **Two tests on the phone, no changes:** (a) on the lock screen, swipe left/right and pull down from the very top; (b) swipe up starting in the middle of the screen versus from the very bottom edge. Record which, if any, does anything. If sideways or top swipes work, moves arrive and only the bottom-edge region or its flags is wrong. If nothing moves at all, look at how move events are sent (phase and range flags); taps only prove down and up.
2. **Paste the full `~/touchp.out`** (e.g. `cat ~/touchp.out`, only 7 lines arrived) so 0094/0097 can be read, and the list of touch/HID/key patch filenames.
3. **Verify the booted kernel contains 0094/0097:** pick a new string from each patch and `grep -a -c` it in the exact `kernel.development.sweet` used; check the CI "Skipping patch" lines (still outstanding from earlier sections).
4. **Fallback only if the gesture route stalls:** put auto-unlock back without editing SpringBoard's binary, by injecting the dylib via SpringBoard's launchd plist with `DYLD_INSERT_LIBRARIES`. Guesses, none checked on this build: dyld may ignore the variable for platform binaries, and the AMFI stand-in (0085 trusts every cdhash) may or may not accept an unsigned dylib. Do **not** patch the SpringBoard Mach-O again (that was the cause of the launchd panic).
5. Confirm after the next boot whether the Hello/setup screen is gone (the plists' intended effect); record that as its own observation.
6. Record provenance for the next photo: which image (tw10 or tw11), kernel artifact, boot start time and elapsed time.
7. Still open from earlier sections and separate: system clock is near the epoch (`12/31/69`) in SpringBoard logs; the CI skipped-patch list; HS-G4 follow-ups; root image is trimmed (several PrivateFrameworks only partly restored).

### 5. Image and safety state
- On the PC: `iosroot-min-tw10.img` (stock SpringBoard base) and `iosroot-min-tw11.img` (tw10 + the two plists). Older rollback images from earlier sections (`iosroot-min-tw6.img` and others) may exist; not re-checked.
- Phone `iosroot.img` should now hold tw11 (user reports it was pushed and booted; output not seen).
- Standing rules unchanged: build on the PC from copies, `fsck.hfsplus -fn`, hash before and after; no direct writes to `sda` or any partition; the only approved phone write is the single file `iosroot.img` through the `dd conv=notrunc` push scripts, and only after the user types `yes` for the exact image. Keep `sweetlog=0`. Do not add 0087.

---
Last updated: 27 October 2026, 19:30 BST (after tw11 boot: SpringBoard stable, unlock swipe still not working)


---

## Latest session update (28 Oct 2026): S21 Exynos USB gadget plan and latest userspace log

This section records the USB-gadget handoff and the latest boot-log observation supplied after the 27 Oct update. It is newer than the older USB and userspace notes, but the gadget has **not yet been compiled or boot-tested**.

### 1. Exynos DWC3 USB gadget: proposed patch 0100

- The S21 tree is reported to still contain Qualcomm-oriented USB patches `0072–0081`. Some earlier handoff notes warn that patches in this range contain Qualcomm MMIO assumptions and may be skipped or unsafe on Exynos. In particular, boot with `sweetusb=0` while testing the new path so the old Qualcomm probe does not read GCC registers at `0x00100000`.
- New script: `xnu/patches/0100-s21-usb-gadget.sh`. It is intended to run after `0099-s21-usb-probe.sh` and reuse the PHY state established by `0099`.
- Target controller: Exynos DWC3 at `0x10e00000`. The probe reportedly showed host mode (`GCTL = 0x30c11004`), so the new path writes `GCTL.PRTCAPDIR = 2` to select device mode before the core soft reset.
- Intended features, rebased from the Qualcomm gadget implementation:
  - endpoint-zero control handling;
  - CDC-ECM network interface `ue0`, device address `192.168.7.2`;
  - receive mbuf fix and link-state events;
  - live UDP logging to host `192.168.7.1:5140`.
- The Qualcomm QSCRATCH VBUS override from patch `0074` is omitted. The stated rationale is that `t2s_usbphy.c` already forces VBUS-valid.
- New `s21usb_init()` modes:
  - `s21usb=3`: start the gadget without re-running PHY power-on; intended for a kexec from Linux that leaves the PHY initialized.
  - `s21usb=4`: run the PHY power-on sequence first.
  - modes 1 and 2 remain the existing panic/register-snapshot paths.
- Validation so far is limited to the script staging cleanly and being safe to run twice. **It has not been compiled** because XNU headers were unavailable. The `GUSB2PHYCFG` UTMI-width/turnaround-time settings and DMA cache coherency remain unverified potential issues.

### 2. Proposed test sequence

Intended kernel boot arguments: `s21usb=3 sweetusb=0`.

The supplied initial recipe assumed `~/s21xnu`, workflow `xnu.yml`, and artifact name `xnu-kernel`, but those assumptions conflict with the repository/build details recorded earlier in this handoff. The established environment in the earlier sections is:

- working checkout: `~/ios-sweet`, branch `s21-stage2`;
- live build remote: `s21xnu` (`github.com/tiktop101/s21xnu`), push with `git push s21xnu s21-stage2:main`;
- GitHub CLI `gh` is recorded as **not installed**;
- CI artifact is recorded as `xnu-kernel.zip`, with kernel extracted to `/tmp/newk/kernel.development.sweet`;
- CI may silently skip a patch after `git apply --check` fails, so check the build log and grep a unique `0100` marker in the exact artifact before booting it.

Do not blindly use the original `gh run ... -n xnu-kernel` command until the actual repository, workflow, artifact name, and availability of `gh` are verified. Use the known S21 kexec flow from the earlier “Commands that work” section (build on PC, `scp` to the phone, then run `kexec` over SSH on the phone). Do not run kexec on the PC.

After boot, the proposed PC-side test is to start `tools/sweet-usbnet.sh` and listen for UDP logs on port `5140`, for example with `nc -ukl 5140`. The expected network endpoint is `192.168.7.2` on the device and `192.168.7.1` on the host. If USB does not enumerate, capture the `S21 USB:` console lines covering soft reset, DSTS, and events. No successful enumeration or gadget boot is established yet.

### 3. Latest userspace log observation

The latest report says:

- `mobileactivationd` never starts;
- the log does not show `Setup` launching;
- `backboardd` is stuck repeating a lookup, and the specific lookup is being investigated.

The lookup name, surrounding log lines, and the exact root image/kernel used for this observation have not yet been supplied. Do not infer a cause from these symptoms alone. Capture the repeated lookup and nearby launchd/backboardd messages before changing the root image or patches.

### 4. Immediate next actions

1. Confirm the current checkout and remote (`pwd`, `git remote -v`, `git branch --show-current`) before copying or committing `0100`.
2. Inspect `0100-s21-usb-gadget.sh`, verify executable permissions, and review its diff. Confirm it stages after `0099` and does not accidentally invoke the Qualcomm probe.
3. Push using the established `~/ios-sweet` / `s21xnu` workflow, then identify the exact CI run and artifact. Check for skipped-patch warnings and verify a unique `0100` marker in the downloaded kernel before booting.
4. Boot with `s21usb=3 sweetusb=0`; run the host USB-network/log listener and photograph all `S21 USB:` lines if enumeration fails.
5. Separately, capture the exact repeated lookup from `backboardd` and nearby launchd messages. Do not conflate this userspace issue with the untested USB gadget work.
6. Keep the existing safety rules: do not write directly to `sda` or phone partitions; only replace `iosroot.img` through the established approved workflow, with explicit confirmation for that exact write.

---
Last updated: 28 Oct 2026 (USB gadget 0100 plan and latest userspace log observation)

---

## 8-core SMP and touch bring-up (2026-10-31)
**Result (observed by the user):** all 8 cores initialize (4x A55 MPIDR 0x000-0x300, 3x A78 0x400-0x600, 1x X1 0x700) with `cpus=8`; touch works, and swipe-up unlocks to the home screen. UI is much faster than with 1 core. Boot takes noticeably longer than with `cpus=4` (cause not investigated; bring-up starts cores one at a time with per-core waits, which is a guess). Staged boots `CPUS=2`, `CPUS=4`, `CPUS=8` all reached the lock screen/home screen. No long-run stability test has been done yet.

**Fixes (patches `0107`, `0108`, plus `tools/mkadt.py` and `boot0104.sh`):**
- `0107` `pexpert/arm/pe_fiq.c`: the GIC redistributor walk used a fixed `GICR_PE_SIZE` of 0x20000. The S21 DTS gives a 2 MiB window for 8 CPUs, i.e. 0x40000 each (GICv4 frame layout). The walk now steps 0x40000 when `GICR_TYPER.VLPIS` (bit 1) is set, else 0x20000. `tools/mkadt.py` `GICR_SIZE` raised from `0x100000` to `0x200000` (1 MiB only covered 4 CPUs). Not tested in isolation; after this change the failure moved from the redistributor lookup to the `CPU_ON` timeout below.
- `0107` `iokit/Kernel/arm/SweetCPU.cpp`: PSCI `CPU_ON` context id is now 0 (Linux passes 0; earlier probes only worked with 0; the entry stub ignores x0).
- `0108` `SweetCPU.cpp`: with `cpus>1` the kernel panicked `SWEET CPU: cpu=1 startup timeout (reset/MMU/GIC/SGI)` at `SweetCPU.cpp:164`, although `CPU_ON` had been accepted. Cause: the kernel lives at ~0x8c0000000 (above 4 GiB) while every `CPU_ON` that ever worked (Linux, probe payloads) used an entry below 4 GiB. `startCPU` now maps physical `0x80300000` (dead shim image from the kexec) with `ml_io_map`, writes a 16-byte trampoline (`ldr x16,#8; br x16; .quad entry`) there, and passes that address to `CPU_ON`. It worked from the first boot with it (`CPUS=2`). Assumption, not verified: `0x80300000` is unused after the shim hands over. `ml_io_map` must be declared `extern "C"` at file scope (the first 0108 failed to link without it).
- `0094` touch driver (`bsd/dev/sweet_touch.c`, bit-banged I2C on gpp4-0/1, ATTN gpa1-0, polled): `touch_pwr` now defaults to 2 (power-cycle the S2DOS05 LDO2/LDO3 touch supplies) and the device-ID read retries up to 4 times, power-cycling again on each NAK. Touch started working with this build. Likely cause (inferred, not proven): Linux's `slsi_ts` shutdown leaves the controller asleep after kexec. Which part of the change fixed it (power-cycle vs retry) is unknown.
- `boot0104.sh` now uses `cpus=${CPUS:-8}`; run `CPUS=4 ./boot0104.sh` to step down if a core misbehaves.

**Build facts:** CI run with 0107+0108 first failed to link (missing `extern "C"`); the corrected 0108 built. Patches still skipped by CI on the S21 (Qualcomm-era, do not apply on the Exynos `sweet_ufs.c`): 0030, 0031, 0034, 0041, 0047, 0054, 0060, 0062, 0063, 0069, 0072-0077, 0081, 0082, 0089. Check `Skipping patch` lines after every push. Live repo/remote: `github.com/jimmycarles9991-glitch/s21xnu`, branch `main` (remote name `s21`; the older `tiktop101/s21xnu` remote in older notes is stale).

**Open / next:**
- The scheduler treats all 8 cores as identical; nothing steers work to the A78/X1 cores (no AMP/cluster topology in the ADT). Per-core `cluster-id` is 0 for all.
- `CNTFRQ_EL0` is 0 or junk on the secondary cores (firmware never programs it); XNU takes 26 MHz from the ADT so it has not mattered so far.
- No soak test yet: watch for random panics on the big cores and report the first panic line (`cpu=N`).
- Move `0105-sweet-hid-edge-toggle-log.patch` from the repo root into `xnu/patches/` if it should be in the build.
- CPU hotplug/suspend (`CPU_OFF`, resume) is still unimplemented and panics by design.

