#!/usr/bin/env python3
"""
oftp-server — OsitoK File Transfer Protocol (UDP) server.

Listens on UDP, serves files in chunks to OsitoK clients (`kdownload`
shell command).  Designed to bypass the Mac→OsitoK→Mac reply-path bug
in the I211 driver: OsitoK initiates with a single UDP request, then
all subsequent packets flow Mac→OsitoK (the working direction).

Protocol:
  Request  (OsitoK → server, 64 bytes):
    [0..3]   magic "OFTQ"
    [4..63]  filename (NUL-padded, UTF-8)
  Chunk    (server → OsitoK, ≤ 1416 bytes):
    [0..3]   magic "OFTD"
    [4..7]   total_size  (uint32 big-endian)
    [8..11]  offset      (uint32 big-endian)
    [12..15] chunk_len   (uint32 big-endian, 0 = EOF marker)
    [16..]   chunk_len bytes of data
  Error response (server → OsitoK):
    [0..3]   magic "OFTE"
    [4..]    error message (ASCII)

Usage:
    ./oftp-server.py [bind-host] [port] [serve-root]
    Defaults: 0.0.0.0  7779  current dir
"""

import os
import socket
import struct
import sys
import time

CHUNK_SIZE   = 1400
MAGIC_REQ    = b"OFTQ"
MAGIC_DAT    = b"OFTD"
MAGIC_ERR    = b"OFTE"
PACE_SECONDS = 0.0005   # 500 µs between chunks — gentle for OsitoK drain


def serve(bind_host: str, port: int, root: str) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_host, port))
    print(f"oftp listening on {bind_host}:{port}  root={os.path.abspath(root)}")

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except KeyboardInterrupt:
            print("\nbye")
            return
        if len(data) < 4 or data[:4] != MAGIC_REQ:
            continue

        filename = data[4:].rstrip(b"\x00").decode("utf-8", errors="ignore")
        path     = os.path.join(root, filename)

        if "/" in filename or ".." in filename or not os.path.isfile(path):
            print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> not-found")
            sock.sendto(MAGIC_ERR + b"not found", addr)
            continue

        size  = os.path.getsize(path)
        print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> {size} bytes")
        sent  = 0
        start = time.monotonic()

        with open(path, "rb") as fh:
            offset = 0
            while offset < size:
                chunk = fh.read(CHUNK_SIZE)
                hdr   = MAGIC_DAT + struct.pack(">III", size, offset, len(chunk))
                sock.sendto(hdr + chunk, addr)
                offset += len(chunk)
                sent   += len(chunk)
                if PACE_SECONDS:
                    time.sleep(PACE_SECONDS)
        # EOF marker (chunk_len = 0, offset = size)
        sock.sendto(MAGIC_DAT + struct.pack(">III", size, size, 0), addr)

        elapsed = time.monotonic() - start
        rate    = sent / elapsed / 1024 if elapsed else 0
        print(f"  {addr[0]}:{addr[1]}  done {sent} B in {elapsed*1000:.0f} ms ({rate:.0f} KB/s)")


def main() -> None:
    bind_host = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0"
    port      = int(sys.argv[2]) if len(sys.argv) > 2 else 7779
    root      = sys.argv[3] if len(sys.argv) > 3 else "."
    if not os.path.isdir(root):
        print(f"error: serve root {root!r} not a directory")
        sys.exit(1)
    serve(bind_host, port, root)


if __name__ == "__main__":
    main()
