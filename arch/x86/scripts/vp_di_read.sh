#!/bin/bash
python3 - <<'PY'
import socket, time, re
s = socket.create_connection(("127.0.0.1", 55555), timeout=5)
def rd():
    time.sleep(0.35); b=b""; s.settimeout(0.6)
    try:
        while True:
            c=s.recv(4096)
            if not c: break
            b+=c
    except: pass
    return b.decode("latin1","replace")
rd()
def xp(addr):
    s.sendall((f"xp/1wx 0x{addr:x}\n").encode())
    r = rd()
    m = re.findall(r":\s*(0x[0-9a-f]+)", r)
    return m[-1] if m else "?"
vp = 0x423D8E80
# guest VA == phys (identity, win32 CR3 maps VA->phys 1:1? VirtualAlloc VAs are mapped, not identity)
# use gva2gpa first
def gva(addr):
    s.sendall((f"gva2gpa 0x{addr:x}\n").encode())
    r = rd()
    m = re.search(r"gpa:\s*(0x[0-9a-f]+)", r)
    return int(m.group(1),16) if m else None
for off,label in ((0x1bc,"DI_active"),(0x18c,"DI_device"),(0x1c4,"UseJoystick?"),(0x38,"bools")):
    g = gva(vp+off)
    if g is None:
        print(f"+{off:#x} {label}: unmapped")
    else:
        print(f"+{off:#x} {label}: {xp(g)}")
s.close()
PY
