#!/usr/bin/env python3
"""
vk-debug-server — OFTP fork tailored for Vulkan/Mesa/Zink/NVK iteration.

Same wire format as oftp-server (so the OsitoK kdownload/kupload commands
work unchanged), but:
  - Default port 7781 (oftp lives on 7779/7781 for the kexec workflow,
    we run alongside it on a separate listener for VK-only round trips).
  - Multiple serve roots concatenated (default: arch/x86/build +
    vulkan-tests/hello-gl-clear), so `kdownload kernel.elf` and
    `kdownload hello-gl-clear.elf` both resolve without changing dirs.
  - Upload side filters incoming logs and pretty-prints lines tagged
    [VK]/[MESA]/[ZINK]/[NVK]/[I211]/[ZINK]/[LOADER] in real time, so
    you see Vulkan progress without grepping the saved file.
  - Each upload saved to vk-logs/<timestamp>-<filename> for diffing
    across iterations.

Wire format (identical to oftp-server.py):
  OFTQ + filename(60)                                      → request
  OFTD + total(BE32) + offset(BE32) + chunk_len(BE32) + data → chunk
  OFTE + ascii                                             → error
  OFTN + offset(BE32)                                      → NAK
  OFTU + total(BE32) + filename                            → upload req
  OFTA + total(BE32)                                       → upload ack
  EOF marker = OFTD + total + total + 0
"""

import os
import re
import socket
import struct
import sys
import time

CHUNK_SIZE   = 1400
MAGIC_REQ    = b"OFTQ"
MAGIC_DAT    = b"OFTD"
MAGIC_ERR    = b"OFTE"
MAGIC_NAK    = b"OFTN"
MAGIC_PUT    = b"OFTU"
MAGIC_ACK    = b"OFTA"
PACE_SECONDS = 0.003
UPLOAD_DIR   = "vk-logs"

# Tags we surface in real time on the console as uploads stream in.
# Order matters only for color picking.
INTERESTING_TAGS = (
    "[VK]", "[ZINK]", "[MESA]", "[NVK]", "[NVK-RES]",
    "[LOADER]", "[I211]", "[NET]", "[GSP]", "[GMMU]",
    "[SASS]", "[GPU]", "[GPU_TENSOR]",
    "FAIL", "ERROR", "PANIC", "EXCEPTION",
)
TAG_RE = re.compile("|".join(re.escape(t) for t in INTERESTING_TAGS))


def serve(bind_host: str, port: int, roots: list) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_host, port))
    abs_roots = [os.path.abspath(r) for r in roots if os.path.isdir(r)]
    if not abs_roots:
        print("error: no valid serve root supplied")
        sys.exit(1)
    print(f"vk-debug-server on {bind_host}:{port}")
    for r in abs_roots:
        print(f"  serve root: {r}")
    upload_root = os.path.join(abs_roots[0], UPLOAD_DIR)
    os.makedirs(upload_root, exist_ok=True)
    print(f"  upload dir: {upload_root}")

    last_serve = {}                # peer → (path, size)
    uploads    = {}                # peer → upload session

    def find_file(name: str) -> str | None:
        """Resolve `name` in the first root that contains it."""
        if "/" in name or ".." in name:
            return None
        for r in abs_roots:
            cand = os.path.join(r, name)
            if os.path.isfile(cand):
                return cand
        return None

    def send_from(path: str, size: int, addr: tuple, start_offset: int) -> int:
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
        sock.sendto(MAGIC_DAT + struct.pack(">III", size, size, 0), addr)
        return sent

    def stream_log_lines(payload: bytes, peer: tuple) -> None:
        """Print VK-tagged lines from a freshly received upload chunk."""
        text = payload.decode("latin1", errors="replace")
        for line in text.splitlines():
            if not line:
                continue
            if TAG_RE.search(line):
                tag = f"{peer[0]}:{peer[1]}"
                print(f"  {tag}  {line}")

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except KeyboardInterrupt:
            print("\nbye")
            return
        if len(data) < 4:
            continue
        magic = data[:4]

        # Upload request
        if magic == MAGIC_PUT and len(data) >= 8:
            (total_size,) = struct.unpack(">I", data[4:8])
            raw_name = data[8:].rstrip(b"\x00").decode("utf-8", errors="ignore")
            base = os.path.basename(raw_name) or "unnamed.bin"
            if ".." in base or total_size > 256 * 1024 * 1024:
                print(f"  {addr[0]}:{addr[1]}  PUT {raw_name!r} -> rejected")
                sock.sendto(MAGIC_ERR + b"rejected", addr)
                continue
            ts = time.strftime("%Y%m%d-%H%M%S")
            dst_path = os.path.join(upload_root, f"{ts}-{base}")
            uploads[addr] = {
                "path":    dst_path,
                "total":   total_size,
                "buf":     bytearray(total_size),
                "high":    0,
                "started": time.monotonic(),
                "stream_pos": 0,
            }
            print(f"  {addr[0]}:{addr[1]}  PUT {base!r} -> {total_size} B")
            sock.sendto(MAGIC_ACK + struct.pack(">I", total_size), addr)
            continue

        # Upload data chunk
        if magic == MAGIC_DAT and len(data) >= 16 and addr in uploads:
            sess = uploads[addr]
            (total, offset, cklen) = struct.unpack(">III", data[4:16])
            if total != sess["total"]:
                continue
            if cklen == 0 and offset == total:
                with open(sess["path"], "wb") as fh:
                    fh.write(bytes(sess["buf"]))
                # Final pass of stream lines for anything between high-watermark
                # and the end (in case last chunks arrived out of order).
                tail = bytes(sess["buf"][sess["stream_pos"]:])
                stream_log_lines(tail, addr)
                elapsed = time.monotonic() - sess["started"]
                rate    = sess["high"] / elapsed / 1024 if elapsed else 0
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
            # Stream-print any complete-looking text since last cursor.
            if end > sess["stream_pos"] and sess["high"] >= sess["stream_pos"] + 256:
                window = bytes(sess["buf"][sess["stream_pos"]:sess["high"]])
                last_nl = window.rfind(b"\n")
                if last_nl >= 0:
                    stream_log_lines(window[:last_nl], addr)
                    sess["stream_pos"] += last_nl + 1
            continue

        # NAK retransmit
        if magic == MAGIC_NAK and len(data) >= 8:
            (resume_off,) = struct.unpack(">I", data[4:8])
            ctx = last_serve.get(addr)
            if not ctx:
                print(f"  {addr[0]}:{addr[1]}  NAK off={resume_off} but no session")
                continue
            path, size = ctx
            print(f"  {addr[0]}:{addr[1]}  NAK from off={resume_off} "
                  f"({size-resume_off} B remaining)")
            start = time.monotonic()
            sent  = send_from(path, size, addr, resume_off)
            elapsed = time.monotonic() - start
            rate    = sent / elapsed / 1024 if elapsed else 0
            print(f"  {addr[0]}:{addr[1]}  re-sent {sent} B in {elapsed*1000:.0f} ms ({rate:.0f} KB/s)")
            continue

        # Download request
        if magic != MAGIC_REQ:
            continue
        filename = data[4:].rstrip(b"\x00").decode("utf-8", errors="ignore")
        path = find_file(filename)
        if not path:
            print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> not-found")
            sock.sendto(MAGIC_ERR + b"not found", addr)
            continue
        size = os.path.getsize(path)
        print(f"  {addr[0]}:{addr[1]}  REQ {filename!r} -> {size} B  ({path})")
        last_serve[addr] = (path, size)
        start   = time.monotonic()
        sent    = send_from(path, size, addr, 0)
        elapsed = time.monotonic() - start
        rate    = sent / elapsed / 1024 if elapsed else 0
        print(f"  {addr[0]}:{addr[1]}  done {sent} B in {elapsed*1000:.0f} ms ({rate:.0f} KB/s)")


def main() -> None:
    bind_host = sys.argv[1] if len(sys.argv) > 1 else "0.0.0.0"
    port      = int(sys.argv[2]) if len(sys.argv) > 2 else 7781
    if len(sys.argv) > 3:
        roots = sys.argv[3:]
    else:
        home = os.path.expanduser("~")
        roots = [
            os.path.join(home, "osito-k/arch/x86/build"),
            os.path.join(home, "ok-ported/vulkan-tests/hello-gl-clear"),
        ]
    serve(bind_host, port, roots)


if __name__ == "__main__":
    main()
