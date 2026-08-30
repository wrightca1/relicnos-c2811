#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dump memory off a Cisco 2811 through ROMMON's `dis` command.

Why `dis`: this ROMMON's `dump` is behind the undocumented `priv`, IOS 12.4(24)T3
has no `show memory <addr> <len>`, and `dnld -u` turned out to write *into* the
device ("timed out waiting for data"), not read out of it.  `dis` is the only
read primitive available unprivileged -- and it prints the raw hex word next to
each mnemonic, so the bytes are recoverable:

    0xbfc00000: 0bf002ad                j 0xbfc00ab4

Throughput is ~85 B/s at 9600 baud (each 4 bytes costs ~45 bytes of console),
so this is a background job: ~2.6 h for 768 KB.  Writes incrementally and
supports --resume, because a run this long should not have to start over.

    rommon_dump.py --start 0xBFC00000 --size 0xC0000 --out rommon.bin
"""
import argparse, os, re, select, sys, termios, time

BAUDS = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
         57600: termios.B57600, 115200: termios.B115200}
# `dis` form:   0xbfc00000: 0bf002ad                j 0xbfc00ab4
LINE = re.compile(rb"0x([0-9a-fA-F]{8}):\s+([0-9a-fA-F]{8})")
# `dump` form:  bfc00000  0bf0 02ad 0000 0000 0000 0000 0000 0000 ........
LINE_DUMP = re.compile(rb"([0-9a-fA-F]{8})\s+((?:[0-9a-fA-F]{4} ){7}[0-9a-fA-F]{4})")
PROMPT = re.compile(rb"rommon \d+ > ?$")


class Port:
    def __init__(self, dev, baud):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        cc = list(a[6]); cc[termios.VMIN] = 0; cc[termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0,
                           BAUDS[baud], BAUDS[baud], cc])
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def write(self, data):
        while data:
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                time.sleep(0.01); continue
            data = data[n:]

    def read_to_prompt(self, timeout):
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    c = os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if c:
                    buf += c
                    if PROMPT.search(buf.replace(b"\r", b"").rstrip(b" ") + b" "):
                        return buf
                    if PROMPT.search(buf[-40:]):
                        return buf
        return buf


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dev", default="/dev/ttyUSB2")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--start", required=True)
    p.add_argument("--size", required=True)
    p.add_argument("--chunk", default="0x400")
    p.add_argument("--out", required=True)
    p.add_argument("--via", choices=("dis", "dump"), default="dis",
                   help="dis = unprivileged, ~85 B/s; dump = needs `priv`, ~218 B/s "
                        "(4.4x console expansion instead of 11.4x)")
    p.add_argument("--resume", action="store_true")
    p.add_argument("--timeout", type=float, default=120.0)
    a = p.parse_args()

    start, size, chunk = int(a.start, 0), int(a.size, 0), int(a.chunk, 0)
    done = 0
    if a.resume and os.path.exists(a.out):
        done = os.path.getsize(a.out) & ~3
        print(f"[d] resuming at +0x{done:x}", flush=True)

    port = Port(a.dev, a.baud)
    port.write(b"\r")
    port.read_to_prompt(10)

    f = open(a.out, "r+b" if done else "wb")
    f.seek(done)
    t0 = time.time()
    while done < size:
        addr = start + done
        n = min(chunk, size - done)
        port.write(f"{a.via} 0x{addr:08X} 0x{n:X}\r".encode())
        buf = port.read_to_prompt(a.timeout)
        if a.via == "dump":
            pieces = [(int(ad, 16), bytes.fromhex(hx.replace(b" ", b"").decode()))
                      for ad, hx in LINE_DUMP.findall(buf)]
        else:
            pieces = [(int(ad, 16), int(w, 16).to_bytes(4, "big"))
                      for ad, w in LINE.findall(buf)]
        if not pieces:
            print(f"\n[d] no data at 0x{addr:08X}; stopping", file=sys.stderr)
            break
        got = 0
        for ad, blob in pieces:
            if ad != addr + got:
                print(f"\n[d] address gap: got 0x{ad:08X} want 0x{addr + got:08X}",
                      file=sys.stderr)
                break
            f.write(blob)
            got += len(blob)
        f.flush()
        if got == 0:
            break
        done += got
        el = time.time() - t0
        pct = 100.0 * done / size
        eta = (size - done) / max(done / max(el, 1), 1)
        print(f"\r[d] 0x{done:06x}/0x{size:06x} ({pct:5.1f}%)  "
              f"{done/max(el,1):5.1f} B/s  ETA {eta/60:5.1f} min", end="", flush=True)
    f.close()
    print(f"\n[d] wrote {done} bytes to {a.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
