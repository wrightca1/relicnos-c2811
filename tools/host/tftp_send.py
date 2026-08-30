#!/usr/bin/env python3
"""Minimal TFTP *send* (read) server (RFC 1350 + 2347/2348/2349).

The companion to tftp_recv.py: that one accepts an upload (device -> us, for pulling
firmware off a box), this one serves a download (us -> device), which is what a
`rommon> boot tftp://<us>/<file>` needs.

Two modes:

  * **file** -- serve ONE local file whatever name the client asks for. ROMMON clients
    vary in how they mangle the path, and in the payload loop there is only ever one file
    we mean. The requested name is logged so a mismatch is still visible.
  * **directory** -- serve any file from a root directory, resolved by the requested name.
    Use this when a live target is pulling several different things (probe binaries,
    kernels) and restarting the server per file would be friction. Requests are confined
    to the root: any path component of ".." or a leading "/" is rejected.

By default it keeps serving until interrupted, because the payload loop is
"boot, watch it fail, rebuild, boot again" and restarting the server each round is friction.

Handles the 16-bit block-number rollover: a classic IOS image at the default 512-byte
blocksize passes 65535 blocks at 32 MB, so this is required, not theoretical.

Usage:
    sudo ./tftp_send.py payload.elf [bind_ip] [--once] [--port N] [--blksize N]

Port 69 needs root (or CAP_NET_BIND_SERVICE); --port is for testing against a normal
TFTP client without privileges.
"""
import os, socket, struct, sys, time

OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR, OP_OACK = 1, 2, 3, 4, 5, 6

ERR_NOTFOUND, ERR_ILLEGAL = 1, 4
NUL = b"\0"   # nested same-quote in an f-string needs py3.12; keep this portable


def log(*a): print("[tftp]", *a, flush=True)


def parse_opts(payload):
    # payload after opcode: filename\0 mode\0 [opt\0 val\0]...
    parts = payload.split(NUL)
    parts = parts[:-1] if parts and parts[-1] == b"" else parts
    fname = parts[0].decode("latin1") if parts else ""
    mode  = parts[1].decode("latin1").lower() if len(parts) > 1 else "octet"
    opts  = {}
    rest  = parts[2:]
    for i in range(0, len(rest) - 1, 2):
        opts[rest[i].decode("latin1").lower()] = rest[i+1].decode("latin1")
    return fname, mode, opts


def resolve(root, fname):
    """Map a requested name onto a file inside `root`, or None if it escapes.

    Clients mangle paths in creative ways, so rather than pattern-match for "..",
    resolve the candidate and require it to still be under root.
    """
    name = fname.replace("\\", "/").lstrip("/")
    if not name:
        return None
    root = os.path.realpath(root)
    cand = os.path.realpath(os.path.join(root, name))
    if cand != root and not cand.startswith(root + os.sep):
        return None
    return cand if os.path.isfile(cand) else None


def drain(srv):
    """Discard anything queued on the listening socket.

    Without this the server wedges.  When a transfer aborts, the client
    retransmits its RRQ several times; those queue on port 69.  The next call
    then picks up a STALE RRQ, sends its OACK to a client that has already given
    up, times out ("no ACK of OACK; aborting"), and leaves yet more queued -- a
    loop that never recovers, with the socket's receive queue growing steadily.
    Draining between transfers makes each one start from the client's current
    request instead of a backlog.
    """
    srv.setblocking(False)
    n = 0
    try:
        while True:
            srv.recvfrom(65536)
            n += 1
    except (BlockingIOError, OSError):
        pass
    finally:
        srv.setblocking(True)
    if n:
        log(f"drained {n} stale packet(s) from the listener")


def send_error(srv, client, code, msg):
    srv.sendto(struct.pack("!HH", OP_ERROR, code) + msg.encode() + b"\0", client)


def serve_one(srv, path, size, max_blksize, root=None):
    """Wait for an RRQ on srv, then serve a file over a fresh transfer socket.

    With `root` set, the file is chosen by the name the client asks for; otherwise
    `path` is served whatever was requested.
    """
    data, client = srv.recvfrom(65536)
    # A client that gave up mid-transfer will have retransmitted; take the most
    # recent request rather than the oldest.
    srv.setblocking(False)
    try:
        while True:
            d2, c2 = srv.recvfrom(65536)
            if struct.unpack("!H", d2[:2])[0] == OP_RRQ:
                data, client = d2, c2
    except (BlockingIOError, OSError):
        pass
    finally:
        srv.setblocking(True)

    op = struct.unpack("!H", data[:2])[0]
    if op != OP_RRQ:
        # A WRQ here is almost certainly the operator reaching for the wrong tool.
        log(f"ignoring opcode {op} from {client} (want RRQ=1; use tftp_recv.py for uploads)")
        return False
    fname, mode, opts = parse_opts(data[2:])
    log(f"RRQ from {client} file={fname!r} mode={mode} opts={opts}")
    if root is not None:
        path = resolve(root, fname)
        if path is None:
            log(f"  no such file under {root}: {fname!r}")
            send_error(srv, client, ERR_NOTFOUND, "file not found")
            return False
        size = os.path.getsize(path)
        log(f"  serving {path} ({size} bytes)")
    elif os.path.basename(fname) != os.path.basename(path):
        log(f"  note: client asked for {os.path.basename(fname)!r}, serving {os.path.basename(path)!r} anyway")

    xfer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    xfer.bind((srv.getsockname()[0], 0))
    xfer.settimeout(5)

    blksize, timeout = 512, None
    accepted = {}
    if "blksize" in opts:
        blksize = max(8, min(max_blksize, int(opts["blksize"])))
        accepted["blksize"] = str(blksize)
    if "tsize" in opts:
        accepted["tsize"] = str(size)          # client sends 0; we answer the real size
    if "timeout" in opts:
        timeout = opts["timeout"]; accepted["timeout"] = timeout

    f = open(path, "rb")
    t0 = time.time()
    sent = 0

    # With options, the client ACKs block 0 to accept the OACK before we send block 1.
    if accepted:
        oack = struct.pack("!H", OP_OACK) + b"".join(
            k.encode() + b"\0" + v.encode() + b"\0" for k, v in accepted.items())
        xfer.sendto(oack, client)
        deadline = time.time() + 30
        while True:
            try:
                pkt, _ = xfer.recvfrom(1024)
            except socket.timeout:
                if time.time() > deadline:
                    log("no ACK of OACK; aborting"); f.close(); xfer.close(); return False
                xfer.sendto(oack, client); continue
            op = struct.unpack("!H", pkt[:2])[0]
            if op == OP_ERROR:
                log(f"client rejected OACK: {pkt[4:].split(NUL)[0]!r}")

                f.close(); xfer.close(); return False
            if op == OP_ACK and struct.unpack("!H", pkt[2:4])[0] == 0:
                break

    blk = 1                       # absolute counter; wire value is blk & 0xFFFF
    while True:
        body = f.read(blksize)
        pkt = struct.pack("!HH", OP_DATA, blk & 0xFFFF) + body
        tries = 0
        while True:
            xfer.sendto(pkt, client)
            try:
                ack, _ = xfer.recvfrom(1024)
            except socket.timeout:
                tries += 1
                if tries > 10:
                    log(f"no ACK for block {blk} after {tries} tries; aborting")
                    f.close(); xfer.close(); return False
                continue
            aop = struct.unpack("!H", ack[:2])[0]
            if aop == OP_ERROR:
                code = struct.unpack("!H", ack[2:4])[0]
                log(f"client ERROR {code}: {ack[4:].split(NUL)[0]!r}")
                f.close(); xfer.close(); return False
            if aop == OP_ACK and struct.unpack("!H", ack[2:4])[0] == (blk & 0xFFFF):
                break
            # stale ACK for an earlier block -> just resend current
        sent += len(body)
        if len(body) < blksize:               # short block terminates the transfer
            break
        blk += 1

    f.close(); xfer.close()
    dt = time.time() - t0
    log(f"DONE {sent} bytes in {dt:.1f}s ({sent/max(dt,0.01)/1e6:.1f} MB/s) <- {path}")
    return True


def main():
    args = [a for a in sys.argv[1:]]
    once = "--once" in args
    if once: args.remove("--once")
    port, blksize_cap = 69, 65464
    for flag, cast in (("--port", int), ("--blksize", int)):
        if flag in args:
            i = args.index(flag)
            val = cast(args[i + 1]); del args[i:i + 2]
            if flag == "--port": port = val
            else: blksize_cap = val
    if not args:
        print(__doc__); return 2
    path = args[0]
    bind_ip = args[1] if len(args) > 1 else "0.0.0.0"

    root = None
    if os.path.isdir(path):
        root, size = os.path.realpath(path), 0
    elif not os.path.isfile(path):
        log(f"no such file or directory: {path}"); return 1
    else:
        size = os.path.getsize(path)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((bind_ip, port))
    except PermissionError:
        log(f"cannot bind {bind_ip}:{port} -- port <1024 needs root (sudo), or pass --port")
        return 1
    srv.settimeout(None)
    what = f"directory {root}" if root else f"{path} ({size} bytes)"
    log(f"serving {what} on {bind_ip}:{port}" + ("  [single-shot]" if once else "  [looping, ^C to stop]"))

    try:
        while True:
            serve_one(srv, path, size, blksize_cap, root)
            if once:
                break
            # Whatever happened -- success or abort -- leave nothing queued for
            # the next request to trip over.
            drain(srv)
    except KeyboardInterrupt:
        log("interrupted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
