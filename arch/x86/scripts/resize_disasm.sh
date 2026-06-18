#!/bin/bash
W=/mnt/c/Users/xasko/osito-k/windrv.bin
OUT=/mnt/c/Users/xasko/osito-k/arch/x86/out/windrv_resizeviewport.txt
objdump -d -M intel --no-show-raw-insn --start-address=0x11109e00 --stop-address=0x1110ac40 "$W" > "$OUT" 2>/dev/null
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
echo "=== function prologues in range (push ebp; mov ebp,esp) ==="
grep -nB0 -A1 'push   ebp' "$OUT" | grep -E 'push   ebp' | head
echo "=== import calls (call DWORD PTR ds:...) in range ==="
grep -oE 'call   DWORD PTR ds:0x1110c[0-9a-f]{3}' "$OUT" | sort | uniq -c | sort -rn | head -20
echo "=== IAT names: resolve each ds:0x1110cxxx ==="
python3 - <<'PY'
import subprocess, struct
data = open('/mnt/c/Users/xasko/osito-k/windrv.bin','rb').read()
# .idata? windrv .rdata 0xc000.. IAT slots referenced as 0x1110c0xx..0x1110c3xx (fileoff == rva)
import re
out = open('/mnt/c/Users/xasko/osito-k/arch/x86/out/windrv_resizeviewport.txt').read()
slots = sorted(set(re.findall(r'call   DWORD PTR ds:(0x1110c[0-9a-f]{3})', out)))
for s in slots:
    rva = int(s,16) - 0x11100000
    val = struct.unpack('<I', data[rva:rva+4])[0]  # on-disk = hint/name RVA
    name = '?'
    if 0xc000 < val < 0x14000:
        end = data.find(b'\0', val+2)
        name = data[val+2:end].decode('latin1', 'replace')
    print(f'{s} -> {name}')
PY
