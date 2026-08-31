#!/bin/bash
# SPDX-License-Identifier: MIT
# Build a minimal BusyBox initramfs for relicnos first-boot-to-shell on the Cisco 2811.
# Output: initramfs.cpio.gz -> bake into the kernel with
#         CONFIG_INITRAMFS_SOURCE="<path to initramfs_root>", so no disk driver is
#         needed for the first boot.
#
# BusyBox must be STATIC and BIG-ENDIAN MIPS (the 2811 is mips-be, not mipsel):
#   git clone https://git.busybox.net/busybox && cd busybox
#   make defconfig
#   sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
#   make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j"$(nproc)"
#
# Usage:  BB=/path/to/busybox ./build_initramfs.sh
set -euo pipefail
BB="${BB:-}"
[ -n "$BB" ] && [ -x "$BB" ] || { echo "set BB=/path/to/static big-endian MIPS busybox (see header)"; exit 1; }

file "$BB" | grep -q 'MSB'            || echo "WARNING: busybox is not big-endian -- the 2811 is mips-be"
file "$BB" | grep -q 'MIPS'           || echo "WARNING: busybox is not MIPS"
file "$BB" | grep -q 'statically linked' || echo "WARNING: busybox is not static -- initramfs will fail"

cd "$(dirname "$0")"
OUT=initramfs_root
rm -rf "$OUT" && mkdir -p "$OUT"/{bin,sbin,dev,proc,sys,etc,mnt}
cp "$BB" "$OUT/bin/busybox"
# A minimal set, enough for init itself to run before busybox installs the rest.
# Everything else is symlinked at boot by `busybox --install -s` (see init), which
# covers every applet this busybox was actually built with rather than whichever
# ones someone remembered to list here.  Hand-maintaining this list is how you end
# up on the box without grep.
for a in sh ls cat mount umount mknod insmod rmmod lsmod modprobe dmesg \
         cttyhack setsid poweroff mdev uname ln mkdir; do
    ln -sf busybox "$OUT/bin/$a"
done

# NOT a symlink: busybox `reboot` signals PID 1, and PID 1 here is a plain shell
# that ignores it, so `reboot` silently does nothing.  `reboot -f` makes the
# syscall directly and reaches the kernel's restart hook.  Wrap it so the obvious
# command does the obvious thing.
cat > "$OUT/bin/reboot" <<'REBOOT'
#!/bin/busybox sh
exec /bin/busybox reboot -f
REBOOT
chmod +x "$OUT/bin/reboot"

# The NM-32A bring-up tools, if a cross toolchain is available.  Baked into the
# initramfs rather than fetched at runtime: relicnos has no network client, and
# the CompactFlash is mounted read-only, so this is the only delivery route.
# Static + stripped -- the whole image has to fit in the flash slot beside IOS.
CC="${CC:-$HOME/relicnos-build/mips3-toolchain/bin/mips-linux-musl-gcc}"
if [ -x "$CC" ]; then
    echo "== building nm32a tools =="
    for src in nm32a/*.c; do
        t=$(basename "$src" .c)
        "$CC" -static -Os -Wall -o "$OUT/bin/$t" "$src" &&             "${CC%gcc}strip" "$OUT/bin/$t" && echo "   $t"
    done
    # The FPGA bitstream IOS shifts into the card (see DYNAMIC_ANALYSIS.md §43):
    # two bit-reversed Xilinx streams, loaded as one contiguous daisy-chain.
    [ -f nm32a/nm_as16_fw.bin ] && cp nm32a/nm_as16_fw.bin "$OUT/" &&         echo "   nm_as16_fw.bin"
    # The CD2481 microcode, lifted out of the IOS image at 0x44479D10 with its
    # length word at 0x44479D0C (see DYNAMIC_ANALYSIS.md §91).  The parts have
    # no protocol code in ROM and do nothing at all until this is downloaded.
    [ -f nm32a/cd2481_ucode.bin ] && cp nm32a/cd2481_ucode.bin "$OUT/" &&         echo "   cd2481_ucode.bin"
    # operator front end for the async card
    [ -f nm32a/nm32a-setup.sh ] && install -m755 nm32a/nm32a-setup.sh "$OUT/bin/" &&         echo "   nm32a-setup.sh"
else
    echo "== no cross toolchain at $CC -- skipping nm32a tools =="
fi

cat > "$OUT/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
/bin/busybox mount -t proc     proc /proc
/bin/busybox mount -t sysfs    sys  /sys
/bin/busybox mount -t devtmpfs dev  /dev
# Reopen stdio on the console. An initramfs has no /dev/console at exec time (the
# kernel only auto-mounts devtmpfs on the non-initramfs path, and a cpio built by a
# normal user cannot carry device nodes), so init starts with fds 0/1/2 CLOSED and
# the kernel prints "unable to open an initial console". Everything echoed before
# this line goes nowhere -- mounting devtmpfs above is not enough on its own.
exec </dev/console >/dev/console 2>&1

# Give ourselves the whole busybox command set: grep, ps, top, sed, awk, find,
# vi, ip, netstat, stty, telnet, wget and the rest appear as real commands
# instead of needing a `busybox ` prefix.
/bin/busybox --install -s /bin 2>/dev/null
/bin/busybox echo ""
/bin/busybox echo "==================================================="
/bin/busybox echo " RELICNOS IS ALIVE ON THE CISCO 2811"
/bin/busybox uname -a
/bin/busybox echo -n " CPU: "; /bin/busybox grep -m1 "cpu model" /proc/cpuinfo
/bin/busybox echo -n " mem: "; /bin/busybox grep -m1 MemTotal  /proc/meminfo
/bin/busybox echo "==================================================="
# The CompactFlash holds IOS and our own kernel; mount it read-only at boot so
# it is simply there.  Remount rw only when writing a kernel (see bootios).
/bin/busybox mkdir -p /mnt/flash
/bin/busybox mount -t vfat -o ro /dev/sda1 /mnt/flash 2>/dev/null \
    && /bin/busybox echo " flash: mounted read-only on /mnt/flash"

# Loopback.  Nothing here needs it yet, but an interface that is DOWN will
# surprise anything expecting 127.0.0.1 to work, and it costs one line.
/bin/busybox ip link set lo up 2>/dev/null

# Resilience.  This box serves 32 consoles unattended, so a panic that sits
# waiting for someone at the power strip is the wrong failure mode -- reboot and
# come back instead.  An oops is promoted to a panic for the same reason: an
# oops in the NM-32A poll thread otherwise leaves a half-working machine, ttys
# registered but nothing servicing them.
#
# Set here rather than relying on CONFIG_CMDLINE: a value you can read back from
# /proc/sys is worth more than one you have to trust.
/bin/busybox echo 10 > /proc/sys/kernel/panic          2>/dev/null
/bin/busybox echo 1  > /proc/sys/kernel/panic_on_oops  2>/dev/null

# NM-32A: the driver does the hardware at probe (microcode into all eight
# CD2481s, 32 ttys, poll thread).  All that is left is the network and the
# console server, so a freshly booted box answers on TCP 2000+port with no
# operator steps.  Port 16 -- first port of the third octal cable -- is 2016.
if [ -c /dev/ttyNM0 ]; then
    # Set these for your network, or replace with a DHCP client.
    NM_ADDR="${NM_ADDR:-192.0.2.2/24}"
    NM_GW="${NM_GW:-192.0.2.1}"
    /bin/busybox ip addr add "$NM_ADDR" dev eth0 2>/dev/null
    /bin/busybox ip link set eth0 up 2>/dev/null
    /bin/busybox ip route add default via "$NM_GW" 2>/dev/null
    /bin/nmconsole --base 2000 --ports 32 --speed 9600 >/nmconsole.log 2>&1 &
    /bin/busybox echo " NM-32A: 32 console ports on TCP 2000-2031 (port 16 = 2016)"
fi
# Respawn the shell instead of exec'ing it.  With `exec` the shell IS pid 1, so
# typing `exit` kills init and the kernel panics with "Attempted to kill init".
# Looping means exit just hands you a fresh prompt, which is what anyone typing
# it actually expects.
while :; do
    /bin/busybox setsid cttyhack /bin/busybox sh
    /bin/busybox echo ""
    /bin/busybox echo "-- shell exited; starting another (this is pid 1, it cannot die) --"
    /bin/busybox sleep 1
done
INIT
cat > "$OUT/bin/bootios" <<'BOOTIOS'
#!/bin/busybox sh
# Prepare the box to boot IOS on its next COLD start.
#
# ROMMON boots what its BOOT variable names (flash:vmlinuz-c2811, set by "boot
# system" in the IOS config).  That variable lives in NVRAM and cannot be changed
# from here, so the lever is to make the file unfindable: ROMMON then falls
# through to the other bootable image, which is IOS.
#
# This does NOT reboot.  Our restart is a warm jump to the reset vector, which
# leaves devices exactly as this kernel had them -- PCI enumerated, CompactFlash
# driver attached -- and IOS's initialisation does not survive that: it boots and
# then dies with a TLB exception.  IOS needs a COLD start.
set -e
echo "hiding the kernel so ROMMON falls through to IOS..."
/bin/busybox mount -o remount,rw /mnt/flash
/bin/busybox mv /mnt/flash/vmlinuz-c2811 /mnt/flash/vmlinuz.off
/bin/busybox sync
/bin/busybox mount -o remount,ro /mnt/flash
echo
echo "done.  Now POWER CYCLE the box (not 'reboot' -- a warm restart crashes IOS)."
echo "To come back afterwards, from IOS:"
echo "    rename flash:vmlinuz.off flash:vmlinuz-c2811"
echo "    reload"
BOOTIOS
chmod +x "$OUT/bin/bootios"

chmod +x "$OUT/init"

( cd "$OUT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 ) > initramfs.cpio.gz
echo "wrote initramfs.cpio.gz ($(du -h initramfs.cpio.gz | cut -f1))"
