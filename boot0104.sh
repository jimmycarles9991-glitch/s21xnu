set -e
cd ~/ios-sweet
mkdir -p kern-new && cp xnu/out/kernel.development.sweet kern-new/
K=$PWD/kern-new/kernel.development.sweet
grep -a -q 'hsg4 pmc: v2' $K || { echo "STOP: not the v2 build"; exit 1; }
grep -a -q 'S21 USB: ue0' $K || { echo "STOP: kernel has no 0100 gadget"; exit 1; }
grep -a -q 'touch %d move at' $K || { echo "STOP: no 0102 move log"; exit 1; }
grep -a -q 'sweet-hid clean first sample' $K || { echo "STOP: no 0103"; exit 1; }
grep -a -q 'sweet-hid field toggles' $K || { echo "STOP: no 0104"; exit 1; }
grep -a -q 'SWEET UFS: extent cap 8192' $K || { echo "STOP: kernel lacks the 8192-entry UFS map"; exit 1; }
grep -a -q '_sweet_console_muted' $K || { echo "STOP: kernel lacks the safe SWEET framebuffer console mute"; exit 1; }
ssh -F /dev/null root@192.168.1.66 'sync; filefrag -v /mnt/android-data/linux/iosroot.img' > /tmp/ff3.txt
python3 tools/mkufsmap.py /tmp/ff3.txt ufsmap3.bin
python3 -c 'import os; n=os.path.getsize("ufsmap3.bin")//12; print("UFS map entries", n); assert n <= 8192, "map exceeds kernel capacity"'
UFSMAP=ufsmap3.bin python3 tools/mkadt.py adt.bin --highmem
python3 tools/mkpayload.py $K adt.bin payload-s21usb.bin --cmdline "-v debug=0xc serial=3 cpus=1 keepsyms=1 sweetfb_noconsole=1 sweetfb_hz=30 sweetusb=0 sweetkeys=0 sweettouch=0 touch_ts=1 sweetbattery=0 rd=ufsroot0 ufsroot=userdata ufshs=1 ufsov=1 s21usb=3 ufsovmb=1024 rootrw=1 unentitled_ios_sim_launch=1 sweethidf=${F:-0}"
scp -F /dev/null s21-shim.Image payload-s21usb.bin root@192.168.1.66:/root/
ssh -F /dev/null -t root@192.168.1.66 'sync; cd /root; kexec -u </dev/null 2>/dev/null; kexec -l s21-shim.Image --initrd=payload-s21usb.bin </dev/null; echo jumping; sleep 2; kexec -e </dev/null'
