#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract the CD2481 microcode from a Cisco IOS image for the NM-32A.

The CD2481 has no protocol code in ROM.  Its on-chip ROM only initialises
hardware and clears the RAM-based register file; until a host downloads
microcode the part does nothing at all, which is why an undriven card looks
like a block of RAM.

Cisco ships that microcode inside the IOS binary.  IOS first tries to open
`flash:<slot>:cd2402-FIRMWARE`, and when that file is absent -- which is the
normal case -- it falls back to a copy compiled into the image and downloads
that on every boot.

This script finds and extracts the built-in copy.  The firmware is Cisco's, so
it is deliberately NOT distributed with this project: run this against an IOS
image you already possess for your own hardware.

    ./extract_cd2481_ucode.py c2800nm-advipservicesk9-mz.124-24.T3.bin \
        -o cd2481_ucode.bin

Layout, for c2800nm-advipservicesk9-mz.124-24.T3.bin:

    0x44479D0C   u32 big-endian   instruction count (8192)
    0x44479D10   payload          count * 18 bits, packed little-endian,
                                  four instructions per nine bytes

8192 instructions is exactly the "8K-word microcode store" the CD2481
datasheet describes.  Other images place it elsewhere, so the default is to
search rather than trust a fixed address.
"""
import argparse
import struct
import sys

# ELF load parameters for the c2800nm image family: virtual address of the
# first byte of the file body, and where that body starts in the file.
BASE, LOAD = 0x4000F000, 0x60
KNOWN_VA = 0x44479D10           # payload address in 12.4(24)T3
INSNS = 8192                    # the 8K-word store
BITS = 18                       # instruction width


def va_to_off(va):
    return va - BASE + LOAD


PAD = 0x1E000                   # the fill IOS writes to top up the store


def unpack(blob, i):
    """The i'th 18-bit instruction of the packed little-endian stream."""
    bit = i * BITS
    byte = bit >> 3
    v = 0
    for k in range(3):
        if byte + k < len(blob):
            v |= blob[byte + k] << (8 * k)
    return (v >> (bit & 7)) & 0x3FFFF


def looks_like_ucode(blob):
    """Identify the real blob rather than a coincidental 0x00002000.

    A big-endian 8192 occurs thousands of times in a 100 MB image, so counting
    printable bytes is not nearly specific enough.  Two properties of the real
    thing are:

      * the tail is padded with 0x1E000 -- the same fill IOS writes when the
        data runs out before the 8K-word store is full, which is a distinctive
        18-bit pattern rather than a byte value;
      * the payload is dense binary, not a string table.

    Requiring a run of trailing PAD instructions is what makes this reliable.
    """
    if len(blob) != INSNS * BITS // 8:
        return False
    if not all(unpack(blob, INSNS - 1 - k) == PAD for k in range(8)):
        return False
    printable = sum(1 for b in blob[:512] if 0x20 <= b < 0x7F)
    return printable < 400


def find_candidates(data):
    """Look for a big-endian count of 8192 followed by plausible microcode."""
    want = struct.pack('>I', INSNS)
    size = INSNS * BITS // 8
    out, pos = [], data.find(want)
    while pos >= 0:
        blob = data[pos + 4:pos + 4 + size]
        if len(blob) == size and looks_like_ucode(blob):
            out.append((pos + 4, blob))
        pos = data.find(want, pos + 1)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='IOS binary to extract from')
    ap.add_argument('-o', '--out', default='cd2481_ucode.bin')
    ap.add_argument('--at', help='file offset or 0x-prefixed VA to force')
    args = ap.parse_args()

    data = open(args.image, 'rb').read()
    size = INSNS * BITS // 8

    if args.at:
        n = int(args.at, 0)
        off = va_to_off(n) if n > 0x40000000 else n
        blob = data[off:off + size]
        print(f'forced offset 0x{off:x}')
    else:
        cands = find_candidates(data)
        if not cands:
            off = va_to_off(KNOWN_VA)
            blob = data[off:off + size]
            if not looks_like_ucode(blob):
                sys.exit('no microcode found; pass --at with an address')
            print(f'using the known 12.4(24)T3 address 0x{KNOWN_VA:08x}')
        else:
            off, blob = cands[0]
            print(f'found {len(cands)} candidate(s); using file offset 0x{off:x} '
                  f'(VA ~0x{off - LOAD + BASE:08x})')

    if len(blob) != size:
        sys.exit(f'short read: got {len(blob)} bytes, need {size}')

    first = ' '.join(f'{unpack(blob, i):05X}' for i in range(6))
    last = ' '.join(f'{unpack(blob, i):05X}' for i in range(INSNS - 3, INSNS))
    print(f'  {INSNS} instructions x {BITS} bits = {size} bytes')
    print(f'  first: {first}')
    print(f'  tail:  {last}   (0x1E000 is the fill IOS pads the store with)')

    open(args.out, 'wb').write(blob)
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
