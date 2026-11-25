# ios-sweet — XNU on Samsung Galaxy S21+ (Exynos 2100)

Experimental work to bring Apple's open-source **XNU kernel (`xnu-8796`, iOS 16 generation)** up on a Samsung Galaxy S21+ using the Exynos 2100 platform, then make the iOS userland usable with Mesa/Panfrost GPU rendering.

The project started as an XNU port for the Xiaomi Redmi Note 10 Pro (`sweet`, Qualcomm SM7150). The S21+ work reuses that kernel and shim code but retargets hardware-specific pieces. Some inherited patches still contain Qualcomm-specific addresses and must not be assumed safe on Exynos.

The older first-light/ramdisk sections below are kept as historical bring-up detail. **Status at a glance is current as of 2026-11-25.** See `NOTES.md` and `ROADMAP-linux-mesa-port.md` for exact boot logs, hashes and open tasks.

## Status at a glance

| Area | Status |
| --- | --- |
| Boot path | Linux `kexec` on the phone loads the shim and XNU payload |
| XNU first light | **Passed** — kernel initialization reaches the SWEET platform stubs and framebuffer setup |
| Display | **Works** — iOS draws to the 1080×2400 framebuffer/panel; IOMFB display power control verified |
| SMP | **Passed** — all 8 cores (4× A55, 3× A78, 1× X1) initialize with `cpus=8`; GIC redistributor stride fix (0107) and low-memory `CPU_ON` trampoline (0108) are required |
| Touch | **Works** — `touch_ts=1`, swipe-up unlock and home-screen interaction reach iOS |
| Buttons | **Works (user-verified)** — power and volume keys reach iOS through the HID key path |
| Charging / battery | **User-reported working (2026-11-25)** — charging is reported working in the current iOS boot. No separate kernel log proof is recorded here; an older photo only proved the low-battery indicator was visible |
| Full iOS userland | **Running** — real iOS 16.7.16 userland boots to lock/home screen (SpringBoard, backboardd, Settings, Control Center) from the file-backed HFS+ root on UFS |
| UFS storage | Exynos UFS initializes in XNU and exposes the file-backed HFS+ root in `userdata` via a boot-time extent map; raw UFS remains read-only, with overlay/rootrw used for recent boots |
| UFS HS-G4 | **Passed in boot tests behind `ufshs`** — `PA_PWRMODE` changes from `0x55` to `0x11`; speed not yet measured |
| GPU proof | **Passed** — Mesa 26.2.4 Panfrost on Mali-G78: exact `glReadPixels()` PASS for clear, textured quad, blended quad and clear+quad (V5/V12 onward) |
| GPU UI | **Mostly works** — `backboardd` EAGL renders the iOS UI via Mesa in V14+; V22/V23 were stable with zero `export failed`/`CREATE_BO failed` and no respring. Still open: blue↔yellow/red↔blue icon swaps, missing Clock sub-icons, expanded-folder black bar, and ~0.3–0.4 s frames. V24 source is prepared but not yet deployed |
| USB / network | Kernel CDC-ECM console and USB networking work for bringing/logs; iOS itself does not yet use `ue0` |
| Wi-Fi / cellular / cameras / audio | Not part of the proven current state; keep separate from the GPU win |
| Safety | Preserve Android, bootloaders, recovery, modem, persist, backups and the `userdata` layout. No GPT changes. Confirm each destructive step |

## Verified platform facts

- **Platform:** Samsung Galaxy S21+, Exynos 2100.
- **PSCI:** version 1.1 over **HVC**, not SMC.
- **Interrupt controller:** GICv3; GICD `0x10200000`, GICR `0x10240000`.
- **Timer:** virtual timer INTID 27, 26 MHz.
- **MMU:** 16 KiB pages supported.
- **Framebuffer:** `0xfa000000`, 1080×2400, stride 4320.
- **Kernel/payload RAM window currently used by the shim:** `0x8c0000000`–`0x940000000` (a conservative 2 GiB window).
- **UFS host controller:** Exynos 2100 UFS host at `0x13100000`; related regions include UniPro `0x13180000` and PMA `0x13104000`.
- **UFS link (observed):** 2 TX / 2 RX lanes; host and device both report max HS gear 4. After the `0083` power-mode change `PA_PWRMODE` reads `0x11` (requested: G4, 2 lanes; gear and lane read-back are not logged yet).
- **CPU:** 4x Cortex-A55 (MPIDR 0x000-0x300), 3x Cortex-A78 (0x400-0x600), 1x Cortex-X1 (0x700). PSCI `CPU_ON` over HVC works with context id 0 and an entry address below 4 GiB, so XNU uses a trampoline at physical `0x80300000`. `cpus=8` boots; use `CPUS=n ./boot0104.sh` to step down.
- **GIC redistributors:** `GICR` `0x10240000`, 2 MiB window, 0x40000 per CPU (GICv4 layout).

These values are from the current handoff and bring-up observations; they are not a complete hardware specification.

## Milestones completed

1. **First light, no ramdisk:** XNU passed early initialization, initialized the SWEET platform stand-ins, set up the framebuffer (`SWEET FB: AppleCLCD 1080x2400`), and registered the HFS implementation.
2. **Mach-O ramdisk root:** `/sbin/launchd` loaded a small custom init as PID 1. The Mach-O image must be padded to a 16 KiB boundary; the unpadded image triggered a VM-map alignment panic.
3. **HFS+ ramdisk root:** XNU mounted a 64 MiB `SWEETROOT` HFS+ image and loaded `/sbin/launchd` from it.
4. **Init v2 syscall tests:** the init successfully tested root remount, file creation/readback, `fork` + `exec` + `wait4`, `sysctl kern.version`, `mmap`, and `/dev/random`.
5. **Read-only UFS:** `bsd/dev/sweet_ufs.c` (patch `0028`) takes over the Exynos UFS host the way the Linux Exynos driver does (PMU/sysreg writes, SMCs, vendor CAL, device reset, link startup, `fDeviceInit`), then reads the GPT and exposes a partition as a read-only block device (`ufsroot0`). Verified by reading the `vbmeta` partition (`AVB0` magic).
6. **HFS+ root from a file inside `userdata`:** a usable iOS root does not fit in any safe partition, so a 4 GiB file is allocated and pinned inside the f2fs filesystem on `userdata`, and its physical extents (1564 of them) are handed to XNU as a `ufsmap` property in the device tree. XNU exposes `userdata` as read-only `ufsroot0`, translates reads through the map, mounts the HFS+ volume (`SWEETROOT`) and runs `sweet-init` as PID 1. Whether f2fs keeps the file's extents fixed has **not** been conclusively proven: regenerate and compare the map after every write to the file.
7. **HS-G4 power-mode change (patch `0083`):** after device init the driver can request HS-G4 on two lanes (boot-arg `ufshs`, below). Verified in one boot with `ufshs=2` (mode change accepted, `PA_PWRMODE` `0x55` to `0x11`, partition data still correct) and in one with `ufshs=1` (the file-backed root mounted and `sweet-init` ran with the new mode active).
8. **Full iOS userland (2026-10-31 onward):** real `launchd`, SpringBoard, backboardd, Settings and Control Center run from the file-backed root; touch, power/volume buttons and swipe-up unlock work; all 8 cores run with `cpus=8`.
9. **Mesa/Panfrost GPU proof (V5/V12):** surfaceless GLES clear, textured quad, blended quad and clear+quad pass exact `glReadPixels()` checks on the Mali-G78. The full-screen colour flag is CPU-painted and is not the proof.
10. **Mesa/EAGL UI (V14–V23):** injected `backboardd` renders the iOS UI through `libs21eagl.dylib`; 8/8 EAGL methods are replaced, V22/V23 have zero `export failed`/`CREATE_BO failed` and no respring. V22 mean frame `422,309 us`; V23 `383,307 us` with App Library ghosting fixed. Colours and some icons/UI details remain wrong.
11. **V24 (prepared, not yet deployed):** source fixes page-aligned EGLImage imports and RGBA byte-order normalisation; CI/build/deploy/boot were approved and started on 2026-11-25 but no V24 boot result exists yet.

## Exynos UFS: status and remaining work

The first UFS-root attempts panicked with `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6`. The controller was disabled at XNU takeover (`HCE=0`, `HCS=0x10f`), so the driver now performs a full controller initialization that mirrors the Linux Exynos UFS driver instead of assuming the bootloader's state. That path is what works today.

UFS boot arguments:

- `rd=ufsroot0 ufsroot=<GPT partition name>` — use the named partition as the root device (the default name is `userdata`).
- `ufshs=` — absent or `0` keeps the controller in PWM Gear 1, the long-proven path. `1` tries the HS-G4 power-mode change and keeps booting. `2` tries it, re-reads the first block of the selected partition, logs the result, and then fails the root lookup on purpose so the log lines are shown in the panic string.

Remaining storage limitations (historical UFS scope, not a denial of the current iOS boots):

- **Speed:** PWM Gear 1 is the default and HS-G4 is opt-in. No read-speed measurement exists yet; the negotiated gear/lane count is not read back and `bRefClkFreq` is not set.
- **Raw UFS writes:** XNU's UFS block device remains read-only. Recent iOS boots use a RAM overlay/rootrw for writable paths; never write to a raw partition without explicit approval.
- **Pinning:** f2fs extent stability after image-file writes has not been conclusively proven. Regenerate and compare the extent map after every write.
- **Patch series:** the note about later `sweet_ufs.c` patches being skipped comes from the 2026-10-24 handoff; check current CI logs rather than assuming it is still true.

Next for the GPU route: finish V24 visual correctness, prove `shared IOSurface` or a reliable CPU path, run a long soak, and keep buttons/touch/charging/storage behaviour stable. Historical storage tasks (HS-G4 speed, pin proof) remain separate.

## Safety rules

- **XNU never writes to UFS** (its block device is read-only). The only write allowed in current testing is Linux-side, into the single pinned root-image file inside the `userdata` filesystem. Never write to a raw partition without explicit approval of the exact target.
- `sda` is the phone's internal UFS disk. Its `sda34` `userdata` partition contains the Linux root filesystem image and kexec launcher. Overwriting it would destroy the current Linux environment. The pinned root-image file lives inside that filesystem; write to the file, never to the raw partition.
- The current UFS test path must use `sweetlog=0` so the inherited persistent console-log feature cannot write into `userdata`.
- Do not touch bootloader, radio, `vbmeta`, or other partitions.
- Keep device backups and calibration data out of git.
- Do not run `kexec` on the PC. Build on the PC, copy artifacts to the phone, and execute `kexec` **on the phone**.
- Never use `set -e` in a shell block intended for interactive Konsole paste; put multi-step commands in a script and run it with `bash`.
- Verify the current patch, build output, and command results before acting. A proposed command is not evidence that it ran successfully.

## Build and test workflow

The kernel is built by the GitHub Actions workflow `.github/workflows/xnu.yml`, using the `xnu-8796` source, the ordered patches under `xnu/patches/`, and the SWEET machine configuration. The workflow is configured for `macos_version=13.5`, `kernel_config=DEVELOPMENT`, and `machine_config=SWEET`. The known kernel artifact is `kernel.development.sweet`; retrieve the artifact from the relevant successful CI run rather than assuming an old local binary is current. The build applies patches in order and **skips a patch that fails to apply instead of failing the build**, so after a push check the build log for `Skipping patch` lines and confirm that a marker string from your change is present in the downloaded kernel (for example `grep -a -c 'hsg4 pmc: v2' kernel.development.sweet`).

The following commands are known from the current bring-up workflow. Run them from the repository root.

### Generate the Apple Device Tree

Output path comes before flags:

```bash
python3 tools/mkadt.py adt.bin --highmem
```

The current local tree has a retargeted UFS node at `0x13100000`, size `0x200`. `mkadt.py` can also incorporate the `/product` node from the extracted iPhone X device tree at `research/ipsw-x/d22ap.adt`. Confirm the output and warnings rather than assuming the generated ADT is correct.

### Build a padded HFS+ ramdisk payload

The HFS+ test root is a 64 MiB image containing `/sbin/launchd`. A minimal recipe is:

```bash
truncate -s 64M hfs-root.img
mkfs.hfsplus -v SWEETROOT hfs-root.img
```

Mount the image, create `sbin` and `dev`, copy the chosen test init to `/sbin/launchd`, make it executable, sync, and unmount. Then build the payload:

```bash
python3 tools/mkpayload.py xnu/out/kernel.development.sweet adt.bin payload-rd.bin \
  --ramdisk hfs-root.img \
  --cmdline "-v debug=0xc serial=3 cpus=1 keepsyms=1 rd=md0 sweetlog=0 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0"
```

`tools/mkpayload.py` has a local change to zero-pad ramdisk bytes to the `rd_size` boundary. Preserve and inspect that change before replacing the file or building from another checkout. For a UFS-root boot see the next subsection; do not switch to a writable UFS root.

### Boot from the file-backed UFS root

1. On the phone, list the pinned image's extents: `filefrag -v <path to the image file> > ff.txt`, and copy `ff.txt` to the PC.
2. `python3 tools/mkufsmap.py ff.txt ufsmap.bin` converts the listing into 12-byte little-endian `(logical, physical, length)` records in 4 KiB blocks and rejects gaps and unwritten extents.
3. `UFSMAP=ufsmap.bin python3 tools/mkadt.py adt.bin --highmem` adds the `ufsmap` property to the `/sweet-ufs` device-tree node.
4. Build the payload with the UFS arguments:

```bash
python3 tools/mkpayload.py xnu/out/kernel.development.sweet adt.bin payload-ufs.bin \
  --cmdline "-v debug=0xc serial=3 cpus=1 keepsyms=1 sweetusb=0 sweetkeys=0 sweettouch=0 sweetbattery=0 sweetlog=0 rd=ufsroot0 ufsroot=userdata ufshs=1"
```

Drop `ufshs=1` for the PWM Gear 1 path. For a read-only diagnostic that needs no extent map, use `ufsroot=vbmeta ufshs=2` with an ADT built without `UFSMAP`; the boot ends in the intended `ufsroot0 unavailable` panic with the UFS log in the panic string. Run `sync` on the phone before every `kexec`, and regenerate the map after any write to the image file.

### Transfer and boot on the phone

The known flow is to copy the shim and payload to the phone, then run `kexec` remotely on the phone. Example command shape (replace filenames as appropriate):

```bash
scp s21-shim.Image payload-rd.bin root@PHONE:/root/
ssh -t root@PHONE 'cd /root; kexec -u </dev/null 2>/dev/null; kexec -l s21-shim.Image --initrd=payload-rd.bin </dev/null; echo jumping; sleep 2; kexec -e </dev/null'
```

The SSH session dropping after `jumping` is expected when the phone transfers to the new kernel. Do not run this until the exact payload, boot arguments, and storage safety conditions have been checked.

## Repository map

| Path | Purpose |
| --- | --- |
| `shim/` | Bare-metal boot shim |
| `xnu/patches/` | Ordered XNU patch series; currently numbered through `0108` (`0028` creates `bsd/dev/sweet_ufs.c`; `0083` adds HS-G4; `0107` GIC stride/PSCI/touch power-cycle; `0108` low-memory `CPU_ON` trampoline). Several Qualcomm-era patches are skipped by CI on the S21; see `NOTES.md` |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the Apple Device Tree |
| `tools/mkpayload.py` | Packs kernel, ADT, boot arguments, and optional ramdisk |
| `tools/mkufsmap.py` | Converts `filefrag -v` output into the `ufsmap` extent table used for the file-backed UFS root |
| `tools/sweet-init2.S` | Minimal userspace init used for syscall tests |
| `tools/mkiosroot.sh` | Existing helper for constructing the full HFS+ iOS root image |
| `research/` | Research artifacts, including the extracted iPhone X ADT |
| `NOTES.md` | Engineering log and prior experiments |

The working tree has local modifications to `tools/mkpayload.py` and `tools/mkadt.py` according to the handoff. Check `git status` before resetting, rebasing, or overwriting files. Generated payloads, root images, device trees, firmware, and backups should not be committed unless explicitly intended.

## Legacy Redmi Note 10 Pro target

The original `sweet` target uses a Qualcomm SM7150 / Snapdragon 732G and has a separate history of hardware support and an iOS 16.7.16 iPhone X userland booting to SpringBoard. That status does **not** describe the current S21+ target. Qualcomm-specific drivers and MMIO addresses must be reviewed before enabling them on Exynos.

## Credits

Based on Apple's open-source XNU and related sources, plus the existing `ios-sweet` project and its build infrastructure. The iOS userland and Apple firmware are Apple's and are not distributed here.
