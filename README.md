# relicnos — Linux on the Cisco 2811

A Linux 6.6 board port for the Cisco 2811 Integrated Services Router, including
a driver for the **NM-32A** 32-port asynchronous network module, so a retired
router works as a 32-port serial console server.

```
   $ telnet 10.0.0.1 2016
   Trying 10.0.0.1...
   Connected to 10.0.0.1.

   sw7150-lab login:
```

That is a switch console on port 16 of the NM-32A, reached over Ethernet from
a Linux kernel running on the router.

## Why this is not trivial

The 2811 has no public documentation, no device tree, and no in-tree support.
The MV96340 system controller's Linux glue was removed from mainline years ago.
The NM-32A's eight Cirrus CD2481 UARTs have **no protocol code in ROM** — they
do nothing until a host downloads microcode, and Cisco ships that microcode
inside the IOS binary. An undriven card reads exactly like a block of RAM,
which is a very convincing wrong answer.

Everything here was worked out by measurement on the hardware and by reading
the IOS binary and the CD2481 datasheet. `docs/FINDINGS.md` records what was
not obvious.

## How it fits together

```
  +---------------------------- Cisco 2811 ------------------------------+
  |                                                                      |
  |  RM5261A "Nevada" MIPS  ----  MV96340 system controller               |
  |         |                          |         |                       |
  |         |                       PCI1      Ethernet                    |
  |         |                          |         |                       |
  |     16550 UART                 NM-32A     Fa0/0                       |
  |         |                          |         |                       |
  +---------|--------------------------|---------|-----------------------+
            |                          |         |
       console 9600               4x octal    10/100
            |                       cables       |
            v                          |         v
     your terminal            32 RS-232 ports   LAN  <---- telnet 2000+n
                                       |
                                       v
                          consoles of other equipment
```

Software:

```
   telnet client
        |  TCP 2000+n
   nmconsole            userspace, one select loop, speaks telnet
        |  /dev/ttyNM<n>
   tty layer
        |
   nm32a driver         poll thread; acknowledge cycle at BAR+0
        |  PCI
   XC4013E FPGA
        |  local bus
   CD2481 x8            microcode downloaded at probe
        |
   RS-232
```

## Layout

```
  kernel/cisco2811/     board port and the NM-32A driver
  kernel/install.sh     drops it into a kernel tree and patches Kconfig
  initramfs/            BusyBox initramfs builder
  tools/                microcode extractor, e_machine stamper
  tools/nm32a/          on-target tools and the console server
  tools/host/           host-side helpers (serial console, TFTP, IOS xref)
  docs/                 hardware, building, installing, the card, findings
```

## Quick start

```
# 1. microcode, from an IOS image you own (not distributed here)
./tools/extract_cd2481_ucode.py <ios-image>.bin -o cd2481_ucode.bin
./tools/gen_ucode_header.py cd2481_ucode.bin -o kernel/cisco2811/cd2481_ucode.h

# 2. board port into a kernel tree
KDIR=~/linux-6.6.154 ./kernel/install.sh

# 3. initramfs
BB=/path/to/static-be-mips-busybox ./initramfs/build_initramfs.sh

# 4. kernel  (vmlinuz, not vmlinux -- size matters, see docs/BUILDING.md)
cd $KDIR && make ARCH=mips CROSS_COMPILE=mips-linux-gnu- cisco2811_defconfig
./scripts/config --set-str CONFIG_INITRAMFS_SOURCE /path/to/initramfs_root
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- olddefconfig
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j$(nproc) vmlinuz

# 5. ROMMON needs a non-standard e_machine
./tools/set_e_machine.py $KDIR/vmlinuz

# 6. copy to the CompactFlash, then:  rommon> boot flash:vmlinuz-c2811
```

Full detail in `docs/BUILDING.md` and `docs/INSTALLING.md`.

## Status

Working: boot from ROMMON, serial console, CompactFlash (`flash:` is plain
FAT16), FastEthernet0/0, PCI, reboot, and all 32 NM-32A ports as
`/dev/ttyNM0..31` with a telnet console server on TCP 2000+n.

Not done: the NM-32A interrupt is not wired, so the driver polls (1 ms while a
port is open). USB, the crypto engine and the HWIC slots are untouched.

## Firmware, and what is deliberately absent

The CD2481 microcode is **Cisco's firmware** and is not distributed here.
`tools/extract_cd2481_ucode.py` pulls it out of an IOS image you already have
for your own hardware; the extracted blob and the generated header are both
in `.gitignore`.

The FPGA image IOS carries (`nm_as16_fw`) is likewise absent, and is not needed
— on this board revision the XC4013E configures itself from its own PROM.

## Licence

Kernel sources are GPL-2.0, matching Linux. Userspace tools in `tools/` are
GPL-2.0 as well.
