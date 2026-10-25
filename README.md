# ios-sweet — XNU on Samsung Galaxy S21+ (Exynos 2100)

Experimental work to bring Apple's open-source **XNU kernel (`xnu-8796`, iOS 16 generation)** up on a Samsung Galaxy S21+ using the Exynos 2100 platform. The current target is kernel bring-up and storage access; this is **not yet a bootable iOS phone**.

The project started as an XNU port for the Xiaomi Redmi Note 10 Pro (`sweet`, Qualcomm SM7150). The S21+ work reuses that kernel and shim code but retargets hardware-specific pieces. Some inherited patches still contain Qualcomm-specific addresses and must not be assumed safe on Exynos.

> **Current status (handoff dated 2026-10-24, updated after the first HS-G4 boot):** XNU has reached first light, a custom userspace init as PID 1, and a small HFS+ ramdisk root. The Exynos UFS controller now works for **read-only** access: link startup, GPT lookup, real partition reads, and a 4 GiB HFS+ root image stored as a file inside `userdata` (read through a boot-time extent map) mounts and runs the custom init as PID 1. An optional HS-G4 power-mode change (boot-arg `ufshs`) has been accepted by the controller and device. Still open: measured speed, proof that the file's extents stay put, a trimmed iOS root, a writable path for `/private/var`, and running Apple's real `launchd`. **This is not yet a bootable iOS phone.**

## Status at a glance

| Area | Status |
| --- | --- |
| Boot path | Linux `kexec` on the phone loads the shim and XNU payload |
| XNU first light | **Passed** — kernel initialization reaches the SWEET platform stubs and framebuffer setup |
| RAM-disk root | **Passed** — `sweet-init` runs as PID 1 from `md0` |
| HFS+ ramdisk root | **Passed** — `/sbin/launchd` is loaded from a 64 MiB HFS+ image |
| Init/syscalls | **Passed** — remount, file I/O, `mkdir`, `fork`/`exec`/`wait4`, `sysctl`, `mmap`, and `/dev/random` tests |
| Display | Kernel framebuffer console verified at 1080×2400 |
| SMP | Not working in the current setup; use `cpus=1` |
| Exynos UFS (read-only) | **Passed** — host init, link startup (2 TX / 2 RX lanes), NOP OUT, `fDeviceInit`, GPT parse, and real partition reads (`vbmeta` shows `AVB0`); XNU's UFS block device is read-only |
| HFS+ root on UFS | **Passed** — a 4 GiB file inside `userdata`, read through an extent map, mounts as `SWEETROOT`; `sweet-init` runs as PID 1. Extent stability of that file is not yet proven |
| UFS HS-G4 | **Passed in boot tests, behind a boot-arg** — `PA_PWRMODE` changes from `0x55` to `0x11` and reads stay correct; speed not yet measured. The default remains PWM Gear 1 |
| Full iOS userland | Not yet demonstrated on the S21+ — Apple's real `launchd` has not been run, and the UFS root is read-only |
| Hardware peripherals | Inherited Qualcomm-specific driver patches are not audited for Exynos and are disabled in the current early-boot command |

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
- **CPU:** secondary CPUs did not come up in the PSCI probe; use `cpus=1`.

These values are from the current handoff and bring-up observations; they are not a complete hardware specification.

## Milestones completed

1. **First light, no ramdisk:** XNU passed early initialization, initialized the SWEET platform stand-ins, set up the framebuffer (`SWEET FB: AppleCLCD 1080x2400`), and registered the HFS implementation.
2. **Mach-O ramdisk root:** `/sbin/launchd` loaded a small custom init as PID 1. The Mach-O image must be padded to a 16 KiB boundary; the unpadded image triggered a VM-map alignment panic.
3. **HFS+ ramdisk root:** XNU mounted a 64 MiB `SWEETROOT` HFS+ image and loaded `/sbin/launchd` from it.
4. **Init v2 syscall tests:** the init successfully tested root remount, file creation/readback, `fork` + `exec` + `wait4`, `sysctl kern.version`, `mmap`, and `/dev/random`.
5. **Read-only UFS:** `bsd/dev/sweet_ufs.c` (patch `0028`) takes over the Exynos UFS host the way the Linux Exynos driver does (PMU/sysreg writes, SMCs, vendor CAL, device reset, link startup, `fDeviceInit`), then reads the GPT and exposes a partition as a read-only block device (`ufsroot0`). Verified by reading the `vbmeta` partition (`AVB0` magic).
6. **HFS+ root from a file inside `userdata`:** a usable iOS root does not fit in any safe partition, so a 4 GiB file is allocated and pinned inside the f2fs filesystem on `userdata`, and its physical extents (1564 of them) are handed to XNU as a `ufsmap` property in the device tree. XNU exposes `userdata` as read-only `ufsroot0`, translates reads through the map, mounts the HFS+ volume (`SWEETROOT`) and runs `sweet-init` as PID 1. Whether f2fs keeps the file's extents fixed has **not** been conclusively proven: regenerate and compare the map after every write to the file.
7. **HS-G4 power-mode change (patch `0083`):** after device init the driver can request HS-G4 on two lanes (boot-arg `ufshs`, below). Verified in one boot with `ufshs=2` (mode change accepted, `PA_PWRMODE` `0x55` to `0x11`, partition data still correct) and in one with `ufshs=1` (the file-backed root mounted and `sweet-init` ran with the new mode active).

## Exynos UFS: status and remaining work

The first UFS-root attempts panicked with `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6`. The controller was disabled at XNU takeover (`HCE=0`, `HCS=0x10f`), so the driver now performs a full controller initialization that mirrors the Linux Exynos UFS driver instead of assuming the bootloader's state. That path is what works today.

UFS boot arguments:

- `rd=ufsroot0 ufsroot=<GPT partition name>` — use the named partition as the root device (the default name is `userdata`).
- `ufshs=` — absent or `0` keeps the controller in PWM Gear 1, the long-proven path. `1` tries the HS-G4 power-mode change and keeps booting. `2` tries it, re-reads the first block of the selected partition, logs the result, and then fails the root lookup on purpose so the log lines are shown in the panic string.

Remaining limitations:

- **Speed:** PWM Gear 1 is the default and HS-G4 is opt-in. No read-speed measurement exists yet, the negotiated gear and lane count are not read back (only the requested values are logged), and the device's `bRefClkFreq` is not set.
- **No writes:** XNU's UFS block device is read-only.
- **Pinning:** that the file's physical extents stay unchanged after it is written has not been conclusively proven.
- **Not iOS yet:** a trimmed HFS+ iOS root and Apple's real `launchd` are future milestones. Because the UFS root is read-only, the init's remount and write tests fail there; a real `launchd` will need a writable `/private/var`, which is not yet decided.
- **Patch series:** according to the 2026-10-24 handoff, later patches that touch `sweet_ufs.c` (writes, persistent log, USB, keys/touch and others) do not apply on top of the current `0028` and are skipped by CI until they are rebased.

Next: measure HS-G4 against PWM G1, verify the pin, build a trimmed HFS+ iOS root, decide how `/private/var` becomes writable, then boot the real `launchd` with verbose logging (the goal is `launchd` running and logging). A visible iOS UI such as SpringBoard would additionally need display, GPU and touch drivers that do not exist for this SoC yet.

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
| `xnu/patches/` | Ordered XNU patch series; currently numbered through `0083` (`0028` creates `bsd/dev/sweet_ufs.c`; `0083` adds the HS-G4 power-mode change) |
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
