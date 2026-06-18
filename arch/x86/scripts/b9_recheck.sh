#!/bin/bash
L=/root/osito-run/serial.log
echo "=== context before first #PF (33840-33930) ==="
sed -n '33840,33930p' "$L" | grep -aiE 'throw|seh|catch|FuncInfo|handler|CxxThrow|AV|0x1014ADBC|4250A|wcscpy|EXCEPTION|RIP|CR2|FMW|pool'
echo ""
echo "=== gva2gpa of the not-present EH page (live kernel CR3) ==="
python3 - <<'PY'
import socket, time
try:
    s = socket.create_connection(("127.0.0.1", 55555), timeout=4)
    def rd():
        time.sleep(0.4); d=b""; s.settimeout(0.6)
        try:
            while True:
                c=s.recv(4096)
                if not c: break
                d+=c
        except: pass
        return d.decode('latin1','replace').strip()
    rd()
    for cmd in ["gva2gpa 0x10173000","gva2gpa 0x10173c1a","gva2gpa 0x10172000","gva2gpa 0x10174000","gva2gpa 0x10100000"]:
        s.sendall((cmd+"\n").encode()); print(cmd, "->", rd())
    s.close()
except Exception as e:
    print("monitor err:", e)
PY
