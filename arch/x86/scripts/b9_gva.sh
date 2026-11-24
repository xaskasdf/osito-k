#!/bin/bash
# Query QEMU monitor for gva2gpa of the B9 fault pages + page-table dump
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 55555), timeout=5)
def rd():
    time.sleep(0.4); data=b""; s.settimeout(0.7)
    try:
        while True:
            c=s.recv(4096)
            if not c: break
            data+=c
    except: pass
    return data.decode("latin1","replace")
rd()
for cmd in ["info registers",
            "gva2gpa 0x10102000",
            "gva2gpa 0x10102e14",
            "gva2gpa 0x10295000",
            "gva2gpa 0x10100000",
            "gva2gpa 0x42930074",
            "gpa2hva 0x42930074"]:
    s.sendall((cmd+"\n").encode())
    print("### "+cmd)
    print(rd().strip())
    print()
s.close()
PY
