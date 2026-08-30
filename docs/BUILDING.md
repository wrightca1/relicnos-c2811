# Building

## What you need

* a big-endian MIPS cross toolchain (`mips-linux-gnu-`)
* Linux 6.6.x source
* a static big-endian MIPS BusyBox
* **an IOS image for your own 2811** -- the NM-32A microcode is extracted from
  it at build time and is not distributed here

```
sudo apt install gcc-mips-linux-gnu binutils-mips-linux-gnu
```

BusyBox, if you do not already have one:

```
git clone https://git.busybox.net/busybox && cd busybox
make defconfig
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j$(nproc)
file busybox      # must say: ELF 32-bit MSB, MIPS, statically linked
```

`MSB` matters.  The 2811 is mips-**be**; a little-endian BusyBox produces a
kernel that boots and then goes silent at `Run /init as init process`.

## 1. Extract the CD2481 microcode

The card does nothing without it.  From an IOS image you possess:

```
./tools/extract_cd2481_ucode.py c2800nm-advipservicesk9-mz.124-24.T3.bin \
    -o cd2481_ucode.bin
./tools/gen_ucode_header.py cd2481_ucode.bin \
    -o kernel/cisco2811/cd2481_ucode.h
```

The extractor locates the blob by its own signature -- a big-endian count of
8192 followed by a payload whose tail is padded with the `0x1E000` fill IOS
writes -- so it is not tied to one image version.  Expect:

```
found 1 candidate(s); using file offset 0x446ad70 (VA ~0x44479d10)
  8192 instructions x 18 bits = 18432 bytes
  first: 26018 26008 23908 23808 24038 2C098
```

Both generated files are in `.gitignore`: they are Cisco firmware, keep them
local.

## 2. Install the board port into a kernel tree

```
KDIR=~/linux-6.6.154 ./kernel/install.sh
```

That copies `kernel/cisco2811/` into `arch/mips/cisco2811/` and patches the
three files a new MIPS platform has to touch (`Kbuild.platforms`,
`arch/mips/Kconfig`, and the Marvell Ethernet Kconfig, which otherwise refuses
to build on MIPS).  It also writes `cisco2811_defconfig`.  Re-running is safe.

## 3. Build the initramfs

```
BB=/path/to/busybox ./initramfs/build_initramfs.sh
```

Produces `initramfs.cpio.gz` and an `initramfs_root/` tree containing the
NM-32A tools and `nm32a-setup.sh`.

## 4. Build the kernel

```
cd $KDIR
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- cisco2811_defconfig
./scripts/config --set-str CONFIG_INITRAMFS_SOURCE /path/to/initramfs_root
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- olddefconfig
make ARCH=mips CROSS_COMPILE=mips-linux-gnu- -j$(nproc) vmlinuz
```

Build **`vmlinuz`**, not `vmlinux`.  `vmlinuz` is the zboot self-extracting
image, about 3.6 MB; `vmlinux` is ~8.8 MB and will not fit on the CF alongside
a 57 MB IOS image on a 61 MB filesystem.

## 5. Stamp e_machine

ROMMON rejects `EM_MIPS`:

```
./tools/set_e_machine.py vmlinuz          # 0x0008 -> 0x0087
```

Without this: `loadprog: error - Invalid image for platform, e_machine = 8`.

## Note on `olddefconfig`

It keeps values already in `.config`.  Editing a `default` in the Kconfig has
no effect on an already-configured tree -- set such options explicitly with
`./scripts/config`.  This has cost debugging time more than once.
