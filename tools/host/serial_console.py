#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dependency-free serial console driver (termios only).

Written for the Cisco 2811 console, where
neither pyserial nor pexpect is installed.  Runs *on that host*; drive it over
ssh.

  probe    -- send a byte or two, read whatever comes back
  capture  -- passive read for N seconds (boot logs)
  run      -- send commands, wait for a prompt regex, print the transcript
  send     -- raw send, no expect (break-ins, control chars)

Every byte read is appended verbatim to --log if given, so a session is always
recoverable even if an expect times out.
"""
import argparse, os, re, select, sys, termios, time

BAUDS = {1200: termios.B1200, 2400: termios.B2400, 4800: termios.B4800,
         9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
         57600: termios.B57600, 115200: termios.B115200}


class Port:
    def __init__(self, dev, baud=9600, log=None):
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.log = open(log, "ab", buffering=0) if log else None
        a = termios.tcgetattr(self.fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = a
        # raw 8N1, no flow control, ignore modem lines (CLOCAL), reader enabled
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = termios.CS8 | termios.CREAD | termios.CLOCAL
        ispeed = ospeed = BAUDS[baud]
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def write(self, data: bytes):
        # Dribble in small chunks.  The 2811 runs its 16550 in POLLED mode
        # (serial8250 .irq = 0, because the MV64340 irqchip is not wired up
        # yet), so the receiver only drains the 16-byte FIFO once per poll
        # tick.  At 9600 baud a 16-byte FIFO fills in ~17 ms, so anything
        # faster than this silently loses characters mid-command --
        # "ttyS0: input overrun(s)", and the command arrives mangled.
        while data:
            n = os.write(self.fd, data[:8])
            data = data[n:]
            time.sleep(0.03)

    def read_until(self, pattern=None, timeout=10.0, quiet=None):
        """Read until `pattern` (compiled regex on bytes) matches, or timeout.

        `quiet` -- if set, also stop once no byte has arrived for that long.
        Returns (buffer, matched_bool).
        """
        buf = b""
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.fd, 4096)
                except BlockingIOError:
                    # select() can report a spurious readiness on a nonblocking
                    # tty; there is simply nothing there yet.
                    continue
                if chunk:
                    buf += chunk
                    last = time.time()
                    if self.log:
                        self.log.write(chunk)
                    if pattern and pattern.search(buf):
                        return buf, True
            elif quiet is not None and time.time() - last >= quiet and buf:
                return buf, False
        return buf, False

    def close(self):
        os.close(self.fd)
        if self.log:
            self.log.close()


def unescape(s: str) -> bytes:
    """Turn a shell-passed string into bytes, honouring \\r \\n \\x03 etc."""
    return s.encode("latin-1", "backslashreplace").decode("unicode_escape").encode("latin-1")


def show(buf: bytes):
    sys.stdout.write(buf.decode("utf-8", "replace"))
    sys.stdout.flush()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dev", default="/dev/ttyUSB1")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--log")
    sub = p.add_subparsers(dest="cmd", required=True)

    q = sub.add_parser("probe")
    q.add_argument("--poke", default="\\r", help="bytes to send first (default CR)")
    q.add_argument("--seconds", type=float, default=6.0)

    c = sub.add_parser("capture")
    c.add_argument("--seconds", type=float, default=60.0)
    c.add_argument("--until", help="regex that ends the capture early")

    r = sub.add_parser("run")
    r.add_argument("cmds", nargs="+")
    r.add_argument("--expect", default=r"[\r\n][^\r\n]*[>#]\s?$", help="prompt regex")
    r.add_argument("--timeout", type=float, default=20.0)

    s = sub.add_parser("send")
    s.add_argument("data")
    s.add_argument("--seconds", type=float, default=3.0)

    a = p.parse_args()
    port = Port(a.dev, a.baud, a.log)
    try:
        if a.cmd == "probe":
            port.write(unescape(a.poke))
            buf, _ = port.read_until(None, a.seconds)
            show(buf)
            print(f"\n--- {len(buf)} bytes ---", file=sys.stderr)
        elif a.cmd == "capture":
            pat = re.compile(a.until.encode()) if a.until else None
            buf, hit = port.read_until(pat, a.seconds)
            show(buf)
            print(f"\n--- {len(buf)} bytes, matched={hit} ---", file=sys.stderr)
        elif a.cmd == "send":
            port.write(unescape(a.data))
            buf, _ = port.read_until(None, a.seconds)
            show(buf)
        elif a.cmd == "run":
            pat = re.compile(a.expect.encode(), re.M)
            for cmd in a.cmds:
                port.write(unescape(cmd) + b"\r")
                buf, hit = port.read_until(pat, a.timeout, quiet=2.0)
                show(buf)
                if not hit:
                    print(f"\n[!] no prompt after {cmd!r}", file=sys.stderr)
    finally:
        port.close()


if __name__ == "__main__":
    main()
