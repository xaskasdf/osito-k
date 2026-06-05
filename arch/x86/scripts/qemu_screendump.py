#!/usr/bin/env python3
# Connect to the QEMU human monitor (TCP) and screendump the framebuffer.
# Usage: qemu_screendump.py <host> <port> <out.ppm>
import socket, sys, time

host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.create_connection((host, port), timeout=10)
s.settimeout(3)
def drain():
    buf = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            buf += d
    except socket.timeout:
        pass
    return buf

print("banner:", drain().decode(errors="replace").strip()[:120])
# QEMU resolves the screendump path relative to its own cwd (build/).
s.sendall(b"screendump out.ppm\n")
time.sleep(1.0)
print("resp:", drain().decode(errors="replace").strip()[:200])
s.close()
