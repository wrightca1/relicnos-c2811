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
# 1. microcode header, from the firmware in this repo
./tools/gen_ucode_header.py firmware/cd2481_ucode.bin \
    -o kernel/cisco2811/cd2481_ucode.h
# (or re-extract it yourself: tools/extract_cd2481_ucode.py <ios-image>.bin)

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

## Firmware

`firmware/cd2481_ucode.bin` is the CD2481 microcode, extracted from a Cisco IOS
image. **The card is inert without it** — the UARTs have no protocol code in
ROM, so this is what makes the hardware function and there is no substitute. It
is unmodified, and `tools/extract_cd2481_ucode.py` will reproduce it byte-for-
byte from your own IOS image if you would rather not trust the copy here.

`firmware/nm_as16_fw.bin` is the FPGA image IOS carries. It is **not used** by
this driver: on this board revision the XC4013E configures itself from its own
PROM, and IOS only loads that image into the later V2 hardware. Included for
reference. See `firmware/README.md`.

IOS images themselves are not included — 122 MB, past GitHub's file size limit,
and unnecessary once the microcode is extracted.

## Licence

Permissive where it can be:

| | |
|---|---|
| `tools/`, `docs/`, `initramfs/` | **MIT** |
| `kernel/` | **GPL-2.0** — a Linux board port and driver is derivative work of the kernel and cannot be relicensed |
| `firmware/` | Cisco's, extracted unmodified for interoperability; no claim made |

Every file carries an SPDX identifier. See [`LICENSE`](LICENSE) for the detail,
including why the kernel half is not MIT and why that is not a practical
restriction for building, running, modifying or shipping this.
