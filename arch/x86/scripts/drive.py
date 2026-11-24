#!/usr/bin/env python3
# Drive the OsitoK in-OS shell over the QEMU serial unix socket.
# Reads commands (one per line) from /tmp/cmds.txt and sends each
# char-by-char with ~25ms gaps (fast input garbles the line editor),
# waiting for the output to go quiet between commands.
#
# Usage: drive.py [sock] [quiet_secs] [cmd_timeout] [final_tail]
import socket, sys, time, os

SOCK   = sys.argv[1] if len(sys.argv) > 1 else "/tmp/osito-serial.sock"
QUIET  = float(sys.argv[2]) if len(sys.argv) > 2 else 4.0   # idle gap => prompt ready
CMD_TO = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0  # max wait per command
TAIL   = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0   # read this long after last cmd
CMDS   = "/tmp/cmds.txt"

# Connect (retry until qemu creates the socket)
s = None
for _ in range(120):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
        break
    except OSError:
        s = None
        time.sleep(0.5)
if s is None:
    print("!! could not connect to", SOCK); sys.exit(1)
s.setblocking(False)
print("== connected to", SOCK)

def drain_until_quiet(quiet, hard_timeout):
    """Read until no bytes arrive for `quiet` seconds (or hard_timeout total)."""
    last = time.time()
    start = time.time()
    got_any = False
    while True:
        try:
            data = s.recv(65536)
            if data:
                sys.stdout.buffer.write(data); sys.stdout.flush()
                last = time.time(); got_any = True
        except BlockingIOError:
            pass
        except OSError:
            break
        now = time.time()
        if now - last >= quiet:
            return got_any
        if now - start >= hard_timeout:
            return got_any
        time.sleep(0.05)

def send(line):
    for ch in line:
        s.sendall(ch.encode())
        time.sleep(0.025)
    s.sendall(b"\r")

print("== waiting for boot to settle ...")
drain_until_quiet(QUIET, 180.0)   # boot can take a while on first NVMe touch

cmds = []
if os.path.exists(CMDS):
    with open(CMDS) as f:
        cmds = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]

for c in cmds:
    print("\n== SEND:", repr(c))
    send(c)
    drain_until_quiet(QUIET, CMD_TO)

print("\n== final tail ...")
drain_until_quiet(TAIL, TAIL + 2)
print("\n== done")
