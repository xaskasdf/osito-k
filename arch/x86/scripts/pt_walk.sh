#!/bin/bash
# Walk the kernel page table (CR3=0x01000000) for the unmapped Core.dll addrs,
# showing each level's entry, to locate the corruption (PD vs PT level).
python3 - <<'PY'
import socket, time
def mon():
    s = socket.create_connection(("127.0.0.1",55555), timeout=4)
    s.settimeout(0.7)
    def rd():
        time.sleep(0.25); b=b""
        try:
            while True:
                c=s.recv(4096)
                if not c: break
                b+=c
        except: pass
        return b.decode('latin1','replace')
    rd()
    def q(cmd):
        s.sendall((cmd+"\n").encode()); r=rd()
        # extract last "0x...: 0x...." hex value
        import re
        m=re.findall(r':\s*(0x[0-9a-fA-F]+)', r)
        return m[-1] if m else r.strip()[-40:]
    return s,q

s,q = mon()
CR3=0x01000000
def walk(va):
    print(f"--- VA 0x{va:08x} ---")
    pml4i=(va>>39)&0x1ff; pdpti=(va>>30)&0x1ff; pdi=(va>>21)&0x1ff; pti=(va>>12)&0x1ff
    e=q(f"xp/1gx 0x{CR3+pml4i*8:x}")
    print(f"PML4[{pml4i}] @0x{CR3+pml4i*8:x} = {e}")
    try: v=int(e,16)
    except: print("  (parse fail)"); return
    if not (v&1): print("  PML4 not present"); return
    pdpt=v & 0x000ffffffffff000
    e=q(f"xp/1gx 0x{pdpt+pdpti*8:x}")
    print(f"PDPT[{pdpti}] @0x{pdpt+pdpti*8:x} = {e}")
    v=int(e,16)
    if not (v&1): print("  PDPT not present"); return
    if v&0x80: print("  PDPT is 1GB large page"); return
    pd=v & 0x000ffffffffff000
    e=q(f"xp/1gx 0x{pd+pdi*8:x}")
    print(f"PD[{pdi}] @0x{pd+pdi*8:x} = {e}")
    v=int(e,16)
    if not (v&1): print("  *** PD ENTRY NOT PRESENT (PD-level corruption) ***"); return
    if v&0x80: print("  PD is 2MB large page (split lost!)"); return
    pt=v & 0x000ffffffffff000
    print(f"  PT base = 0x{pt:x}")
    e=q(f"xp/1gx 0x{pt+pti*8:x}")
    print(f"PT[{pti}] @0x{pt+pti*8:x} = {e}")
    v=int(e,16)
    print("  PTE present" if (v&1) else "  *** PTE NOT PRESENT (PT-level corruption) ***")

for va in (0x10173000, 0x1022f000, 0x10100000, 0x10101000):
    walk(va)
s.close()
PY
