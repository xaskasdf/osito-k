#!/bin/bash
OUT=/mnt/c/Users/xasko/osito-k/arch/x86/out/windrv_resizeviewport.txt
echo "=== appFailAssert call sites with 8 lines of context before ==="
grep -nB8 'ds:0x1110c088' "$OUT" | grep -E 'push   0x1110|call|cmp|test|j(e|ne|z|nz)' | head -60
echo ""
echo "=== decode pushed assert strings (ASCII in .rdata) ==="
python3 - <<'PY'
import re, struct
data = open('/mnt/c/Users/xasko/osito-k/windrv.bin','rb').read()
out  = open('/mnt/c/Users/xasko/osito-k/arch/x86/out/windrv_resizeviewport.txt').read()
# capture push imm32 in the 6 instrs before each appFailAssert call
sites = [m.start() for m in re.finditer(r'call   DWORD PTR ds:0x1110c088', out)]
lines = out.split('\n')
idx = [i for i,l in enumerate(lines) if 'ds:0x1110c088' in l]
seen = set()
for i in idx:
    addr = lines[i].split(':')[0].strip()
    pushes = []
    for j in range(max(0,i-6), i):
        m = re.search(r'push   (0x1110[0-9a-f]{4})', lines[j])
        if m: pushes.append(m.group(1))
    strs = []
    for p in pushes:
        rva = int(p,16) - 0x11100000
        if 0xc000 <= rva < 0x14000:
            end = data.find(b'\0', rva)
            s = data[rva:end][:60].decode('latin1','replace')
            strs.append(s)
    print(f'assert @0x{addr}: ' + ' | '.join(strs))
PY
