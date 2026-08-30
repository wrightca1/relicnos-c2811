#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cross-reference strings to the code that uses them, in the decompressed IOS image.

`ANALYSIS_METHOD.md` records that absolute-address cross-referencing "does not work" on
this image.  That was half right: it fails if you use the ELF's own vaddr.  The image is
an ELF with one load segment at 0x8000f000, but the CODE addresses its data through the
0x4xxxxxxx alias -- the same memory IOS reports as `main` in `show region`, where
`main:text` was seen at 0x400176C0.

The real link base is 0x4000f000, derived by measurement rather than assumption: take
every lui/addiu-computed address in the code, then pick the base under which most of them
land on the first byte of a printable C string.  0x4000f000 scores 2441 against 463 for
the runner-up.

    ios_xref.py <image> "some string"        # find the code that references it
    ios_xref.py <image> --at 0x40123456      # disassemble around an address
"""
import struct, subprocess, sys, tempfile, os

LOAD_OFF = 0x60          # single PT_LOAD file offset
BASE     = 0x4000f000    # link base as the code addresses itself
CODE_END = 0x4161060     # end of the text section, from the section table


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        n = (CODE_END - LOAD_OFF) // 4
        self.W = struct.unpack(f'>{n}I', self.d[LOAD_OFF:LOAD_OFF + n * 4])
        self.n = n

    def off2va(self, off):
        return off - LOAD_OFF + BASE

    def va2off(self, va):
        return va - BASE + LOAD_OFF

    def cstr_start(self, off):
        while off > 0 and self.d[off - 1] != 0:
            off -= 1
        return off

    def find_refs(self, va, limit=24):
        """lui rX,hi ; ... addiu rX,rX,lo -- hi adjusted for lo's sign extension."""
        lo = va & 0xFFFF
        hi = ((va >> 16) + (1 if lo >= 0x8000 else 0)) & 0xFFFF
        lo_s = lo if lo < 0x8000 else lo - 0x10000
        out = []
        for i in range(self.n):
            w = self.W[i]
            if (w >> 26) != 0x0F or (w & 0xFFFF) != hi:
                continue
            rt = (w >> 16) & 0x1F
            for j in range(i + 1, min(i + 12, self.n)):
                w2 = self.W[j]
                op = w2 >> 26
                if op == 0x09 and ((w2 >> 21) & 0x1F) == rt:
                    o2 = w2 & 0xFFFF
                    o2 = o2 if o2 < 0x8000 else o2 - 0x10000
                    if o2 == lo_s:
                        out.append(BASE + i * 4)
                    break
                if op == 0x0F and ((w2 >> 16) & 0x1F) == rt:
                    break
            if len(out) >= limit:
                break
        return out

    def disasm(self, va, count=40, before=8):
        start = va - before * 4
        off = self.va2off(start)
        blob = self.d[off:off + count * 4]
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
            f.write(blob); tmp = f.name
        try:
            out = subprocess.run(
                ['mips-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'mips', '-EB',
                 f'--adjust-vma=0x{start:08x}', tmp],
                capture_output=True, text=True).stdout
            return '\n'.join(out.split('\n')[7:])
        finally:
            os.unlink(tmp)


def main():
    img = Image(sys.argv[1])
    if sys.argv[2] == '--at':
        print(img.disasm(int(sys.argv[3], 16), count=int(sys.argv[4]) if len(sys.argv) > 4 else 40))
        return
    for pat in sys.argv[2:]:
        off = img.d.find(pat.encode())
        if off < 0:
            print(f'\n=== {pat!r}: string not found'); continue
        st = img.cstr_start(off)
        va = img.off2va(st)
        refs = img.find_refs(va)
        print(f'\n=== {pat!r}')
        print(f'    string va 0x{va:08x}: {img.d[st:st+64]!r}')
        print(f'    referenced from: {[hex(r) for r in refs] or "none"}')


if __name__ == '__main__':
    main()
