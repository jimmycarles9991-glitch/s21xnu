# ios-sweet — XNU on Samsung Galaxy S21+ (Exynos 2100)

Experimental work to bring Apple's open-source **XNU kernel (`xnu-8796`, iOS 16 generation)** up on a Samsung Galaxy S21+ using the Exynos 2100 platform. The current target is kernel bring-up and storage access; this is **not yet a bootable iOS phone**.

The project started as an XNU port for the Xiaomi Redmi Note 10 Pro (`sweet`, Qualcomm SM7150). The S21+ work reuses that kernel and shim code but retargets hardware-specific pieces. Some inherited patches still contain Qualcomm-specific addresses and must not be assumed safe on Exynos.

> **Current status (handoff dated 2026-10-24):** XNU has reached first light, a custom userspace init as PID 1, and a small HFS+ ramdisk root. The current milestone is getting the Exynos UFS controller working for a read-only root-device path. The UFS bring-up is unresolved. No successful boot of the full iOS root filesystem on the S21+ is documented.

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
| Exynos UFS | **In progress** — controller HCE is observed as zero at XNU takeover; link startup/root discovery not working yet |
| Full iOS userland | Not yet demonstrated on the S21+ |
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
- **CPU:** secondary CPUs did not come up in the PSCI probe; use `cpus=1`.

These values are from the current handoff and bring-up observations; they are not a complete hardware specification.

## Milestones completed

1. **First light, no ramdisk:** XNU passed early initialization, initialized the SWEET platform stand-ins, set up the framebuffer (`SWEET FB: AppleCLCD 1080x2400`), and registered the HFS implementation.
2. **Mach-O ramdisk root:** `/sbin/launchd` loaded a small custom init as PID 1. The Mach-O image must be padded to a 16 KiB boundary; the unpadded image triggered a VM-map alignment panic.
3. **HFS+ ramdisk root:** XNU mounted a 64 MiB `SWEETROOT` HFS+ image and loaded `/sbin/launchd` from it.
4. **Init v2 syscall tests:** the init successfully tested root remount, file creation/readback, `fork` + `exec` + `wait4`, `sysctl kern.version`, `mmap`, and `/dev/random`.

## Current blocker: Exynos UFS

The first UFS-root attempt panicked with:

```text
IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6
```

A later diagnostic showed `HCE=0`, `HCS=0x10f`, and `CAP=0x1303ff1f`. `CAP` bit 24 (`64AS`) is set, so the observed failure is not the driver's 64-bit DMA capability check. It is not yet known whether Linux's `kexec` shutdown path leaves the UFS host disabled before XNU takes over.

A proposed patch to `xnu/patches/0028-sweet-ufs-root-device.patch` would integrate Exynos vendor CAL setup, link startup, and `b1`–`b5` diagnostics. **Do not assume that proposal has been applied, reviewed, built, or pushed.** Check the actual patch and repository state before testing it.

Important unresolved items include:

- Read the relevant Linux `__ufshcd_wl_suspend()` / `UFS_SHUTDOWN_PM` path and the full Exynos UFS host driver.
- Determine the UFS controller state immediately before `kexec`, using a valid register-access method.
- Verify vendor CAL integration, reset and clock-stop bits, lane configuration, and any required PMU / SMC setup.
- After link startup, investigate NOP OUT / `fDeviceInit` handling if the device does not become ready.
- Only after read-only UFS access works should GPT partition discovery and an iOS root be attempted.

## Safety rules

- **Do not write to any phone storage partition during current UFS testing.**
- `sda` is the phone's internal UFS disk. Its `sda34` `userdata` partition contains the Linux root filesystem image and kexec launcher. Overwriting it would destroy the current Linux environment.
- The current UFS test path must use `sweetlog=0` so the inherited persistent console-log feature cannot write into `userdata`.
- Do not touch bootloader, radio, `vbmeta`, or other partitions.
- Keep device backups and calibration data out of git.
- Do not run `kexec` on the PC. Build on the PC, copy artifacts to the phone, and execute `kexec` **on the phone**.
- Never use `set -e` in a shell block intended for interactive Konsole paste; put multi-step commands in a script and run it with `bash`.
- Verify the current patch, build output, and command results before acting. A proposed command is not evidence that it ran successfully.

## Build and test workflow

The kernel is built by the GitHub Actions workflow `.github/workflows/xnu.yml`, using the `xnu-8796` source, the ordered patches under `xnu/patches/`, and the SWEET machine configuration. The workflow is configured for `macos_version=13.5`, `kernel_config=DEVELOPMENT`, and `machine_config=SWEET`. The known kernel artifact is `kernel.development.sweet`; retrieve the artifact from the relevant successful CI run rather than assuming an old local binary is current.

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

`tools/mkpayload.py` has a local change to zero-pad ramdisk bytes to the `rd_size` boundary. Preserve and inspect that change before replacing the file or building from another checkout. The exact UFS-root boot arguments and payload should only be used after reviewing the current UFS patch; do not switch to a writable UFS root.

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
| `xnu/patches/` | Ordered XNU patch series; currently numbered through `0081` in the handoff |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the Apple Device Tree |
| `tools/mkpayload.py` | Packs kernel, ADT, boot arguments, and optional ramdisk |
| `tools/sweet-init2.S` | Minimal userspace init used for syscall tests |
| `tools/mkiosroot.sh` | Existing helper for constructing the full HFS+ iOS root image |
| `research/` | Research artifacts, including the extracted iPhone X ADT |
| `NOTES.md` | Engineering log and prior experiments |

The working tree has local modifications to `tools/mkpayload.py` and `tools/mkadt.py` according to the handoff. Check `git status` before resetting, rebasing, or overwriting files. Generated payloads, root images, device trees, firmware, and backups should not be committed unless explicitly intended.

## Legacy Redmi Note 10 Pro target

The original `sweet` target uses a Qualcomm SM7150 / Snapdragon 732G and has a separate history of hardware support and an iOS 16.7.16 iPhone X userland booting to SpringBoard. That status does **not** describe the current S21+ target. Qualcomm-specific drivers and MMIO addresses must be reviewed before enabling them on Exynos.

## Credits

Based on Apple's open-source XNU and related sources, plus the existing `ios-sweet` project and its build infrastructure. The iOS userland and Apple firmware are Apple's and are not distributed here.
