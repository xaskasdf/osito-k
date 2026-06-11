#!/usr/bin/env python3
# Send arbitrary QEMU human-monitor commands. Usage:
#   qemu_mon.py <host> <port> "cmd1" "cmd2" ...
import socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
cmds = sys.argv[3:]
s = socket.create_connection((host, port), timeout=10)
s.settimeout(2)
def drain():
    b = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            b += d
    except socket.timeout:
        pass
    return b
drain()
for c in cmds:
    s.sendall((c + "\n").encode())
    time.sleep(0.4)
    drain()
s.close()
print("sent:", cmds)
