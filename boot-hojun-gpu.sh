#!/bin/bash
# hojun-style boot, prepped for SweetMetal GPU compositing (mali + sweetmetal + sweetgpucomp).
# Default kernel: hojun128/s21xnu run 44452977047 artifact (71b58dac, 8192-cap).
# Base cmdline follows hojun-boot0105.sh; adds GPU args (mali=1, gpu_scratch, sweetmetal, sweetgpucomp).
# SWEETMETAL default 2: backboardd/PosterBoard only. METAL=4 was tested 2026-11-21 and added
# 66 SpringBoard/chronod "NOT in pack" shaders without improving the display; override to 4 if wanted.
# Builds ufsmap/ADT/payload, scps shim+payload to device, then kexecs by default.
# Set DO_KEXEC=0 for staging only. SSH/SCP password is supplied automatically (S21_SSH_PASSWORD, default root).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)

# Feed the device password non-interactively without sshpass/expect being installed.
SSH_PW_HELPER="$HERE/tools/ssh_pw.py"
S21_SSH_PASSWORD=${S21_SSH_PASSWORD:-root}
export S21_SSH_PASSWORD
ssh() { python3 "$SSH_PW_HELPER" ssh "$@"; }
scp() { python3 "$SSH_PW_HELPER" scp "$@"; }

cd ~/ios-sweet
mkdir -p kern-hojun
KERNEL_SRC=${KERNEL_SRC:-/home/tom/Downloads/hojun-latest/extract/1a1a81f-kernel.development.sweet}
cp "$KERNEL_SRC" kern-hojun/kernel.development.sweet
K=$PWD/kern-hojun/kernel.development.sweet

# hojun markers (from boot0105.sh)
grep -a -q 'hsg4 pmc: v2' "$K" || { echo "STOP: not the v2 build"; exit 1; }
grep -a -q 'S21 USB: ue0' "$K" || { echo "STOP: kernel has no 0100 gadget"; exit 1; }
grep -a -q 'touch %d move at' "$K" || { echo "STOP: no 0102 move log"; exit 1; }
grep -a -q 'sweet-hid clean first sample' "$K" || { echo "STOP: no 0103"; exit 1; }
grep -a -q 'sweet-hid field toggles' "$K" || { echo "STOP: no 0104"; exit 1; }
grep -a -q '_sweet_console_muted' "$K" || { echo "STOP: kernel lacks the safe SWEET framebuffer console mute"; exit 1; }

# cap depends on kernel
if grep -a -q 'SWEET UFS: extent cap 8192' "$K"; then
  CAP=8192
elif grep -a -q 'SWEET UFS: extent cap 65536' "$K"; then
  CAP=65536
else
  echo "STOP: kernel lacks a recognized UFS extent cap marker"; exit 1
fi
echo "Kernel: $K ($(sha256sum "$K" | awk '{print $1}')), cap=$CAP"

grep -a -q 'no gpu_scratch boot-arg' "$K" || { echo "STOP: kernel lacks the Mali core driver (0111)"; exit 1; }
grep -a -q 'sweetgpucomp' "$K" || { echo "STOP: kernel lacks sweetgpucomp (0121)"; exit 1; }
grep -a -q 'sweetmetal' "$K" || { echo "STOP: kernel lacks sweetmetal/IOAcceleratorES support"; exit 1; }
grep -a -q 'fb src %u layer' "$K" || { echo "STOP: kernel lacks 0208 source probe (old kernel?)"; exit 1; }

ssh -F /dev/null root@192.168.1.66 'sync; filefrag -v /mnt/android-data/linux/iosroot.img' > /tmp/ff-hojun-gpu.txt
python3 /home/tom/s21xnu-main/tools/mkufsmap.py /tmp/ff-hojun-gpu.txt ufsmap-hojun-gpu.bin
python3 -c 'import os,sys; n=os.path.getsize("ufsmap-hojun-gpu.bin")//12; print("UFS map entries", n); assert n <= int(sys.argv[1]), "map exceeds kernel capacity"' "$CAP"

UFSMAP=ufsmap-hojun-gpu.bin python3 /home/tom/s21xnu-main/tools/mkadt.py adt-hojun-gpu.bin --highmem --gpu-scratch 2> /tmp/mkadt-hojun-gpu.err
SCR=$(grep -o 'gpu_scratch=0x[0-9a-f]*' /tmp/mkadt-hojun-gpu.err | tail -1)
[ -n "$SCR" ] || { echo "STOP: mkadt did not print gpu_scratch=0x..."; exit 1; }

WIFI_ARGS=()
PCIE_LEVEL=2
WIFI_CMD=
if [ "${WIFI:-0}" = 1 ]; then
  grep -a -q 'BCM4375/' "$K" || { echo "STOP: kernel lacks Stage 3 BCM4375 firmware support"; exit 1; }
  WIFI_FW=${WIFI_FW:-/home/tom/s21/fw/bcmdhd_sta.bin_b1}
  WIFI_CLM=${WIFI_CLM:-/home/tom/s21/fw/bcmdhd_clm.blob}
  : "${WIFI_NVRAM:?Set WIFI_NVRAM to the desired nvram.txt_1rh_es43_b1 or nvram.txt_CS01_semco_b1 file}"
  for f in "$WIFI_FW" "$WIFI_NVRAM" "$WIFI_CLM"; do
    [ -s "$f" ] || { echo "STOP: Wi-Fi input missing or empty: $f"; exit 1; }
  done
  WIFI_ARGS=(--wifi-firmware "$WIFI_FW" --wifi-nvram "$WIFI_NVRAM" --wifi-clm "$WIFI_CLM")
  PCIE_LEVEL=3
fi

# hojun base args + GPU compositing (SweetMetal) + interactive stay
# GPUCOMP=0 forces the CPU copy path; default 1 keeps the existing GPU-blit behaviour.
# SWEETMETAL=1 refuses every client except malitest (backboardd/SpringBoard get no device);
# default 2 gives the render server a device; 4 allows all clients and adds untranslated shaders.
GPUCOMP=${GPUCOMP:-1}
SWEETMETAL=${SWEETMETAL:-2}
# EXTRA appends arbitrary boot args (e.g. sweetnopair=1 sweetpresflush=1) for A/B tests.
EXTRA=${EXTRA:-}
# -v makes XNU draw verbose text into the framebuffer console immediately;
# sweetfb_noconsole=1 only mutes it at the first swap (patch 0067), so the text
# scrolls over the shim's centered Apple logo before iOS's boot layer appears.
# Default off so the logo stays visible; VERBOSE=1 restores it for debugging.
VERBOSE=${VERBOSE:-0}
VARG=""
[ "$VERBOSE" = 1 ] && VARG="-v "
CMDLINE="${VARG}debug=0xc serial=3 cpus=${CPUS:-8} keepsyms=1 sweetfb_noconsole=1 sweetfb_hz=30 sweetusb=0 sweetkeys=0 sweettouch=0 touch_ts=1 sweetbattery=0 rd=ufsroot0 ufsroot=userdata ufshs=1 ufsov=1 s21usb=3 ufsovmb=1024 rootrw=1 unentitled_ios_sim_launch=1 sweethidf=${F:-0} sweetpcie=${PCIE_LEVEL}${WIFI_CMD} mali=1 $SCR sweetmetal=${SWEETMETAL} sweetgpucomp=${GPUCOMP} sweetstay=1${EXTRA:+ $EXTRA}"
echo "GPU args: mali=1 $SCR sweetmetal=${SWEETMETAL} sweetgpucomp=${GPUCOMP} sweetstay=1 (hojun-style base: cpus=${CPUS:-8} sweetfb_noconsole=1 sweetbattery=0 s21usb=3)"
echo "CMDLINE: $CMDLINE"

python3 "$HERE/tools/mkpayload.py" "$K" adt-hojun-gpu.bin payload-hojun-gpu.bin \
  --cmdline "$CMDLINE" \
  "${WIFI_ARGS[@]}"

SHIM=${SHIM:-/home/tom/ios-sweet/s21-shim.Image}
[ -s "$SHIM" ] || { echo "STOP: shim missing: $SHIM"; exit 1; }
scp -F /dev/null "$SHIM" payload-hojun-gpu.bin root@192.168.1.66:/root/
echo "staged shim+payload on device (/root/s21-shim.Image, /root/payload-hojun-gpu.bin)"

if [ "${DO_KEXEC:-1}" = 1 ]; then
  echo "kexec enabled (set DO_KEXEC=0 to stage only)"
  ssh -F /dev/null -t root@192.168.1.66 'sync; cd /root; kexec -u </dev/null 2>/dev/null; kexec -l s21-shim.Image --initrd=payload-hojun-gpu.bin </dev/null; echo jumping; sleep 2; kexec -e </dev/null' \
    || echo "ssh session ended after kexec -e (expected)"
else
  echo "NOT running kexec (DO_KEXEC=0). To boot manually on device:"
  echo "  cd /root; sync; kexec -u </dev/null 2>/dev/null; kexec -l s21-shim.Image --initrd=payload-hojun-gpu.bin </dev/null; echo jumping; sleep 2; kexec -e </dev/null"
fi
