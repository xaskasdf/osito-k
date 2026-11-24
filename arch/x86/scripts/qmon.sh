#!/bin/bash
# Query the QEMU monitor (tcp 127.0.0.1:55555) for status + CPU registers.
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 55555), timeout=5)
def rd():
    time.sleep(0.4); data=b""
    s.settimeout(0.6)
    try:
        while True:
            c=s.recv(4096)
            if not c: break
            data+=c
    except: pass
    return data.decode("latin1","replace")
rd()
for cmd in ["info status","info registers","info registers"]:
    s.sendall((cmd+"\n").encode())
    print("### "+cmd)
    print(rd())
s.close()
PY
