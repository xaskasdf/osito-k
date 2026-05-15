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
    Defaults: 0.0.0.0  7781  current dir
"""

import os
import socket
import struct
import sys
import time

CHUNK_SIZE   = 1400
MAGIC_REQ    = b"OFTQ"   # download request   (client → server)
MAGIC_DAT    = b"OFTD"   # data chunk         (both directions)
MAGIC_ERR    = b"OFTE"   # error              (server → client)
MAGIC_NAK    = b"OFTN"   # download NAK       (client → server, "resend from offset X")
MAGIC_PUT    = b"OFTU"   # upload request     (client → server)
MAGIC_ACK    = b"OFTA"   # upload acknowledge (server → client, "ready, send chunks")
UPLOAD_DIR   = "uploads"  # subdir under serve-root for incoming files
# 3 ms ≈ 333 chunks/s ≈ 460 KB/s. Slow on purpose: previous 0.5 ms left
# OsitoK losing the tail packets of a 1.2 MB transfer (kdownload stalled
# at 1237600/1240728). Drain through net_poll on the i211 polling loop is
# the bottleneck. The NAK retransmit handler below covers leftover loss.
PACE_SECONDS = 0.003


def serve(bind_host: str, port: int, root: str) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_host, port))
    print(f"oftp listening on {bind_host}:{port}  root={os.path.abspath(root)}")

    # peer (addr, path, size) of the file currently being served — kept
    # so a NAK from the same peer can re-stream from a given offset.
    last_serve = {}

    # peer → upload session: {path, total_size, buffer (bytearray), high_watermark}
    uploads = {}

    # Ensure upload dir exists under root
    upload_root = os.path.join(root, UPLOAD_DIR)
    os.makedirs(upload_root, exist_ok=True)

    def send_from(path: str, size: int, addr: tuple, start_offset: int) -> int:
        """Stream from start_offset to EOF. Returns bytes sent (excl. EOF marker)."""
        sent = 0
        with open(path, "rb") as fh:
            fh.seek(start_offset)
            offset = start_offset
            while offset < size:
                chunk = fh.read(CHUNK_SIZE)
                hdr   = MAGIC_DAT + struct.pack(">III", size, offset, len(chunk))
                sock.sendto(hdr + chunk, addr)
                offset += len(chunk)
                sent   += len(chunk)
                if PACE_SECONDS:
                    time.sleep(PACE_SECONDS)
        # EOF marker so receiver knows we're done with this batch.
        sock.sendto(MAGIC_DAT + struct.pack(">III", size, size, 0), addr)
        return sent

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except KeyboardInterrupt:
            print("\nbye")
            return
        if len(data) < 4:
            continue

        magic = data[:4]

        # ── Upload request: client wants to push a file to us ───────
        if magic == MAGIC_PUT and len(data) >= 8:
            (total_size,) = struct.unpack(">I", data[4:8])
            filename = data[8:].rstrip(b"\x00").decode("utf-8", errors="ignore")
            # Sanitize filename: strip path components, refuse traversal
            base = os.path.basename(filename) or "unnamed.bin"
            if ".." in base or total_size > 256 * 1024 * 1024:
                print(f"  {addr[0]}:{addr[1]}  PUT {filename!r} -> rejected")
                sock.sendto(MAGIC_ERR + b"rejected", addr)
                continue
            dst_path = os.path.join(upload_root, base)
            uploads[addr] = {
                "path": dst_path,
                "total": total_size,
                "buf": bytearray(total_size),
                "high": 0,
                "started": time.monotonic(),
            }
            print(f"  {addr[0]}:{addr[1]}  PUT {base!r} -> {total_size} bytes (recv)")
            sock.sendto(MAGIC_ACK + struct.pack(">I", total_size), addr)
            continue

        # ── Upload data chunk: belongs to an active upload session ──
        if magic == MAGIC_DAT and len(data) >= 16 and addr in uploads:
            sess = uploads[addr]
            (total, offset, cklen) = struct.unpack(">III", data[4:16])
            if total != sess["total"]:
                continue  # stale or mismatched
            # EOF marker
            if cklen == 0 and offset == total:
                bytes_written = sum(1 for b in sess["buf"] if b is not None) if False else len(sess["buf"])
                with open(sess["path"], "wb") as fh:
                    fh.write(bytes(sess["buf"]))
                elapsed = time.monotonic() - sess["started"]
                rate = sess["high"] / elapsed / 1024 if elapsed else 0
                print(f"  {addr[0]}:{addr[1]}  PUT done {sess['high']}/{total} B "
                      f"in {elapsed*1000:.0f} ms ({rate:.0f} KB/s) -> {sess['path']}")
                del uploads[addr]
                continue
            if offset + cklen > total or 16 + cklen > len(data):
                continue
            sess["buf"][offset:offset+cklen] = data[16:16+cklen]
            end = offset + cklen
            if end > sess["high"]:
                sess["high"] = end
            continue

        # NAK retransmit: client lost a stretch starting at <offset>.
        if magic == MAGIC_NAK and len(data) >= 8:
            (resume_off,) = struct.unpack(">I", data[4:8])
            ctx = last_serve.get(addr)
            if not ctx:
                print(f"  {addr[0]}:{addr[1]}  NAK off={resume_off} but no active session")
                continue
            path, size = ctx
            print(f"  {addr[0]}:{addr[1]}  NAK from off={resume_off} ({size-resume_off} B remaining)")
            start = time.monotonic()
            sent  = send_from(path, size, addr, resume_off)
            elapsed = time.monotonic() - start
            rate    = sent / elapsed / 1024 if elapsed else 0
            print(f"  {addr[0]}:{addr[1]}  re-sent {sent} B in {elapsed*1000:.0f} ms ({rate:.0f} KB/s)")
            continue

        if magic != MAGIC_REQ:
            continue

        filename = data[4:].rstrip(b"\x00").decode("utf-8", errors="ignore")
        path     = os.path.join(root, filename)

        if "/" in filename or ".." in filename or not os.path.isfile(path):
            print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> not-found")
            sock.sendto(MAGIC_ERR + b"not found", addr)
            continue

        size = os.path.getsize(path)
        print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> {size} bytes")
        last_serve[addr] = (path, size)

        start   = time.monotonic()
        sent    = send_from(path, size, addr, 0)
        elapsed = time.monotonic() - start
        rate    = sent / elapsed / 1024 if elapsed else 0
        print(f"  {addr[0]}:{addr[1]}  done {sent} B in {elapsed*1000:.0f} ms ({rate:.0f} KB/s)")


def main() -> None:
    bind_host = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0"
    port      = int(sys.argv[2]) if len(sys.argv) > 2 else 7781
    root      = sys.argv[3] if len(sys.argv) > 3 else "."
    if not os.path.isdir(root):
        print(f"error: serve root {root!r} not a directory")
        sys.exit(1)
    serve(bind_host, port, root)


if __name__ == "__main__":
    main()
