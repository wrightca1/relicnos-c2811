#!/usr/bin/env python3
"""Set the ELF e_machine field of a big-endian ELF32.

Why this exists: every ELF the Cisco 2811 boot chain handles carries a
Cisco-custom e_machine, never the standard EM_MIPS (0x08):

    0x0087   the IOS image (outer decompressor glue AND the inner IOS kernel)
    0x0069   monlib, the ROMMON filesystem library

`file(1)` misreads 0x0087 as "Sunplus S+core7". The real ISA is MIPS big-endian.

Two consequences:
  1. Our own payload should present 0x0087 too. If this ROMMON validates
     e_machine before loading, a stock EM_MIPS binary gets rejected -- and that
     failure would look identical to a dozen other failure modes, so eliminate
     it up front.
  2. Going the OTHER way (0x0087 -> 0x08) is what makes a genuine Cisco image
     disassemble in tools that dispatch on e_machine (Ghidra, objdump -d with no
     explicit -m).

Usage:
    set_e_machine.py <elf> [value]      # default 0x0087; prints old -> new
    set_e_machine.py <elf> 0x08         # back to EM_MIPS for disassembly
"""
import struct, sys

EM_MIPS, CISCO_C2800NM = 0x08, 0x0087
NAMES = {0x08: "EM_MIPS", 0x0087: "Cisco c2800nm custom", 0x0069: "Cisco monlib custom"}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    new = int(sys.argv[2], 0) if len(sys.argv) > 2 else CISCO_C2800NM

    with open(path, "r+b") as f:
        hdr = f.read(20)
        if hdr[:4] != b"\x7fELF":
            print(f"{path}: not an ELF"); return 1
        if hdr[4] != 1 or hdr[5] != 2:
            print(f"{path}: expected ELF32 big-endian (got class={hdr[4]} data={hdr[5]})")
            return 1
        old, = struct.unpack(">H", hdr[18:20])
        if old == new:
            print(f"{path}: e_machine already 0x{new:04x} ({NAMES.get(new,'?')})")
            return 0
        f.seek(18)
        f.write(struct.pack(">H", new))
    print(f"{path}: e_machine 0x{old:04x} ({NAMES.get(old,'?')}) -> "
          f"0x{new:04x} ({NAMES.get(new,'?')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
