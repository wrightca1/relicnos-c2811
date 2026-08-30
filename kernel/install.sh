#!/bin/bash
# Install the Cisco 2811 board support into a Linux source tree.
#
# The board files live in THIS repo (cisco2811/) rather than only in a
# downloaded kernel, so they are version-controlled and survive re-downloading
# or re-basing the tree. This script copies them in and patches the three
# arch/mips files that have to reference a new platform.
#
# Idempotent: re-running on an already-patched tree is a no-op.
#
#   KDIR=~/relicnos-build/linux-6.6.154 ./install.sh
#
# Then:
#   cd $KDIR
#   make ARCH=mips CROSS_COMPILE=mips-linux-gnu- cisco2811_defconfig   # (written below)
#   make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j$(nproc) vmlinux
set -euo pipefail

KDIR="${KDIR:-$HOME/relicnos-build/linux-6.6.154}"
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -f "$KDIR/arch/mips/Kbuild.platforms" ] || {
    echo "not a MIPS kernel tree: $KDIR (set KDIR=...)"; exit 1; }

echo "== installing board files into $KDIR/arch/mips/cisco2811 =="
mkdir -p "$KDIR/arch/mips/cisco2811"
cp "$HERE/cisco2811/"{setup.c,irq.c,time.c,serial.c,mv64340_irq.c,eth.c,pata.c,pci.c,nm32a.c,cisco2811.h,cd2481_ucode.h,Makefile,Platform} \
   "$KDIR/arch/mips/cisco2811/"

# 1. Kbuild.platforms -- list the platform directory
if ! grep -q CONFIG_CISCO_2811 "$KDIR/arch/mips/Kbuild.platforms"; then
    echo "== patching Kbuild.platforms =="
    sed -i 's|^platform-$(CONFIG_MIPS_COBALT)\(.*\)$|platform-$(CONFIG_CISCO_2811)\t\t+= cisco2811/\n&|' \
        "$KDIR/arch/mips/Kbuild.platforms"
fi

# 2. arch/mips/Kconfig -- the board entry itself.
#    Inserted immediately before "config MIPS_COBALT", which is the closest
#    relative in-tree: Cobalt is the other SYS_HAS_CPU_NEVADA board.
# Always refresh the block: keying this on "is the entry already there?" means
# any later edit to the entry (a new select, a new sub-option) is silently
# skipped on an already-patched tree. That cost two debug cycles.
if true; then
    # drop any previous (stale) block so the entry can be refreshed in place
    python3 - "$KDIR/arch/mips/Kconfig" <<'PYEOF'
import sys, re
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"config CISCO_2811\n(?:.*\n)*?\nconfig MIPS_COBALT\n", "config MIPS_COBALT\n", s, count=1)
open(p, "w").write(s)
PYEOF
    echo "== patching arch/mips/Kconfig =="
    python3 - "$KDIR/arch/mips/Kconfig" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
entry = '''config CISCO_2811
	bool "Cisco 2811 Integrated Services Router (relicnos)"
	select BOOT_ELF32
	select CEVT_R4K
	select CSRC_R4K
	select DMA_NONCOHERENT
	select IRQ_MIPS_CPU
	select SYS_HAS_CPU_NEVADA
	select SYS_HAS_EARLY_PRINTK
	select SYS_SUPPORTS_32BIT_KERNEL
	select SYS_SUPPORTS_BIG_ENDIAN
	select SYS_SUPPORTS_ZBOOT
	select SWAP_IO_SPACE
	select HAVE_PCI
	select USE_GENERIC_EARLY_PRINTK_8250
	help
	  Support for the Cisco 2811 Integrated Services Router: a QED/PMC-Sierra
	  RM52xx "Nevada" (MIPS IV) with a Marvell MV64340 system controller,
	  256 MB of DRAM and two 16550 UARTs on the device bus.

	  The board is booted from its stock ROMMON, which loads an arbitrary
	  ELF and jumps to it -- there is no secure boot on this generation.

config CISCO2811_IRQ_DISCOVERY
	bool "Log MV64340 interrupt cause bits as they first fire"
	depends on CISCO_2811
	default y
	help
	  The MV64340 datasheet assigns each interrupt source a bit in the main
	  cause registers, but include/linux/mv643xx.h carries only register
	  offsets, not the bit map -- so which bit is the Ethernet port is not
	  known from the sources we have.

	  With this enabled the demux logs each cause bit the first time it
	  fires, which identifies the Ethernet bit from observed link activity
	  instead of guesswork. Turn it off once CISCO2811_ETH_IRQ_BIT is known
	  to be correct.

config CISCO2811_PATA
	bool "CompactFlash (flash:) support"
	depends on CISCO_2811 && ATA && BLOCK
	default y
	help
	  The CompactFlash card IOS calls "flash:" is an ATA device on device
	  chip-select DevCS3, physical 0x1FB00000.

	  The task file is not at a uniform stride, so pata_platform cannot
	  drive it: ATA registers are grouped in pairs, one pair per 32-bit
	  word.  The layout was recovered from ROMMON's own ATA code and
	  confirmed on the hardware with an IDENTIFY DEVICE, which returned
	  "STI Flash 8.0.0", 125440 sectors.

	  The port runs polled -- no interrupt is wired to the slot.

config CISCO2811_NM32A
	bool "NM-32A 32-port async module"
	depends on CISCO_2811 && PCI
	default y
	help
	  Cisco NM-32A: eight Cirrus CD2481 quad UARTs behind a Xilinx XC4013E
	  PCI interface, giving 32 async serial ports.

	  The CD2481 has no protocol code on chip and is completely inert until
	  the host downloads a microcode image; that image was extracted from the
	  stock IOS binary and is built into the driver.  Data is moved by the
	  card bus-mastering into host memory through DMA buffer descriptors --
	  the transmit FIFO is not used.

	  The card can only reach the first 8 MB of DRAM, because the MV96340
	  enables just one chip-select window onto PCI, so the driver uses the
	  region reserved by the board's plat_mem_setup().

config CISCO2811_ETH_IRQ_BIT
	int "MV64340 cause bit for FastEthernet0/0"
	depends on CISCO_2811
	default 32
	help
	  Bit number within the MV64340 main interrupt cause registers that the
	  Ethernet port asserts (0-63).

	  Measured on the board, reading the cause registers from userspace with
	  /dev/mem while eth0 was up and being pinged.  The raw 32-bit read of
	  MAIN_INTERRUPT_CAUSE_HIGH (phys 0x1400000c) returned 0x01180000, which
	  byte-swaps to the 0x00001801 the kernel sees through readl() -- high-word
	  bits 0, 11 and 12, i.e. global bits 32, 43 and 44.  Bit 32 appears exactly
	  when the port comes up, and 32 is also MV64x60_IRQ_ETH_0 in the old
	  PowerPC mv64x60 PIC driver, which agrees.

	  An earlier guess of 42 was wrong: bit 42 is never set.  With it selected,
	  bit 32 stayed masked, so the MAC's interrupt never reached the CPU and
	  eth0 could transmit but never received.

'''
anchor = 'config MIPS_COBALT\n'
assert anchor in s, "anchor 'config MIPS_COBALT' not found"
s = s.replace(anchor, entry + anchor, 1)
open(p, 'w').write(s)
print("   inserted config CISCO_2811")
PY
fi

# 2b. drivers/net/ethernet/marvell/Kconfig -- MV643XX_ETH is gated to
#     "PPC32 || PLAT_ORION || COMPILE_TEST" upstream, which excludes MIPS even
#     though the MV64340 is a MIPS-era part and the driver is platform-based.
if ! grep -q "CISCO_2811" "$KDIR/drivers/net/ethernet/marvell/Kconfig"; then
    echo "== patching MV643XX_ETH for MIPS =="
    python3 - "$KDIR/drivers/net/ethernet/marvell/Kconfig" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "\tdepends on PPC32 || PLAT_ORION || COMPILE_TEST"
new = "\tdepends on PPC32 || PLAT_ORION || CISCO_2811 || COMPILE_TEST"
assert old in s, "MV643XX_ETH depends line not found"
open(p, "w").write(s.replace(old, new, 1))
print("   MV643XX_ETH now buildable on CISCO_2811")
PYEOF
fi

# 3. A defconfig, so the build is one command and reproducible.
cat > "$KDIR/arch/mips/configs/cisco2811_defconfig" <<'EOF'
CONFIG_CISCO_2811=y
CONFIG_CPU_NEVADA=y
CONFIG_KERNEL_LZMA=y
CONFIG_CPU_BIG_ENDIAN=y
CONFIG_SYSVIPC=y
CONFIG_LOG_BUF_SHIFT=14
CONFIG_BLK_DEV_INITRD=y
CONFIG_INITRAMFS_SOURCE=""
CONFIG_EXPERT=y
CONFIG_EARLY_PRINTK=y
# ROMMON passes NO kernel command line (the boot log shows "Kernel command line:"
# empty), so without this the kernel finds no console= argument, selects the dummy
# VGA console as tty0, and disables the serial bootconsole -- the board goes silent
# mid-boot while still running perfectly.  CMDLINE_OVERRIDE makes it unconditional.
CONFIG_CMDLINE_BOOL=y
CONFIG_CMDLINE="console=ttyS0,9600"
CONFIG_CMDLINE_OVERRIDE=y
# No video on this board; drop the VT layer so nothing can claim tty0.
# CONFIG_VT is not set
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_SERIAL_8250_NR_UARTS=2
CONFIG_SERIAL_8250_RUNTIME_UARTS=2
# /dev/mem so MV64340 registers can be read from the shell with `busybox devmem`
# and compared against the values IOS reports -- the only way to settle register
# access questions (offset vs endianness) by measurement instead of guesswork.
CONFIG_DEVMEM=y
# CONFIG_STRICT_DEVMEM is not set
CONFIG_DEVTMPFS=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_TMPFS=y
# CONFIG_SUSPEND is not set
# PCI is needed for the NM-32A, which appears on the MV96340's PCI1 bus. The
# host-bridge glue lives in cisco2811/pci.c: without it the bridge keeps ROMMON's
# state (command register 0x0000 -- not a bus master), no config cycle leaves the
# chip, and the bus reads as empty. See DYNAMIC_ANALYSIS.md sec 63.
CONFIG_PCI=y
CONFIG_NET=y
CONFIG_INET=y
CONFIG_PACKET=y
CONFIG_UNIX=y
CONFIG_NETDEVICES=y
CONFIG_ETHERNET=y
CONFIG_NET_VENDOR_MARVELL=y
CONFIG_MV643XX_ETH=y
CONFIG_BROADCOM_PHY=y
CONFIG_CISCO2811_IRQ_DISCOVERY=y
# Measured, not guessed -- see the help text on this symbol. Stated explicitly
# because a Kconfig 'default' only applies to a FRESH .config: olddefconfig on an
# already-configured tree silently keeps the old value, which cost a build cycle.
CONFIG_CISCO2811_ETH_IRQ_BIT=32
# Block layer + libata for the CompactFlash. The card is MBR-partitioned with
# a single FAT16 partition (type 0x04) starting at LBA 32 -- read straight off
# the medium -- so plain vfat mounts it; no Cisco-specific filesystem needed.
CONFIG_BLOCK=y
CONFIG_PARTITION_ADVANCED=y
CONFIG_MSDOS_PARTITION=y
CONFIG_ATA=y
CONFIG_ATA_SFF=y
# CONFIG_ATA_BMDMA is not set
CONFIG_SCSI=y
CONFIG_BLK_DEV_SD=y
CONFIG_CISCO2811_PATA=y
CONFIG_CISCO2811_NM32A=y
CONFIG_FAT_FS=y
CONFIG_VFAT_FS=y
CONFIG_NLS=y
CONFIG_NLS_CODEPAGE_437=y
CONFIG_NLS_ISO8859_1=y
CONFIG_NLS_DEFAULT="iso8859-1"
EOF
echo "== wrote arch/mips/configs/cisco2811_defconfig =="

echo
echo "installed. build with:"
echo "  cd $KDIR"
echo "  make ARCH=mips CROSS_COMPILE=mips-linux-gnu- cisco2811_defconfig"
echo "  ./scripts/config --set-str CONFIG_INITRAMFS_SOURCE <path to initramfs_root>"
echo "  make ARCH=mips CROSS_COMPILE=mips-linux-gnu- olddefconfig"
echo "  make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j\$(nproc) vmlinux"
