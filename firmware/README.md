# Firmware

## `cd2481_ucode.bin` — CD2481 microcode, 18432 bytes

8192 instructions of 18 bits, packed little-endian, four per nine bytes.  The
driver compiles this in (see `tools/gen_ucode_header.py`) and downloads it to
all eight CD2481s at probe.

Extracted from `c2800nm-advipservicesk9-mz.124-24.T3.bin` at virtual address
`0x44479D10`, with its instruction count in the word immediately before it at
`0x44479D0C`.  IOS carries this copy as a fallback: it first tries to open
`flash:<slot>:cd2402-FIRMWARE` and, when that file is absent — which is the
normal case — downloads the built-in image instead.

    md5  4c487530fb0785c69b8f88df2e274762

The tail is padded with `0x1E000`, the same fill IOS writes to top the 8K-word
store up when the data runs out.  `tools/extract_cd2481_ucode.py` uses that as
its signature and will reproduce this file byte-for-byte from any IOS image
carrying it.

**The part is inert without this.**  The CD2481 has no protocol code in ROM,
only a boot ROM that initialises hardware and clears the RAM-based register
file, so an undriven card reads exactly like a block of RAM.

## `nm_as16_fw.bin` — FPGA image, 260044 bytes

Two Virtex/Spartan-II bitstreams (sync words at `0x4` and `0x1FBE8`), stored
bit-reversed.

**Not used by this driver, and not needed on this board.**  IOS loads it only
into cards of type 913 — a later `NM-8/16AM-V2` revision whose bus interface is
a Spartan-II with no configuration PROM.  The board this project targets is the
original `800-02245-07`, whose XILINX XC4013E configures itself from the PLCC-20
PROM at U12; that is why the card answers PCI as `10ee:4013` on a cold boot
before the host has sent it anything.  Included for reference and for anyone
working on the V2 hardware.

## Provenance

Both are Cisco firmware, extracted from an IOS image for interoperability —
they are what makes the hardware function, and there is no substitute.  Neither
has been modified.  `tools/extract_cd2481_ucode.py` remains in the tree so the
microcode can be reproduced from your own image if you prefer.
