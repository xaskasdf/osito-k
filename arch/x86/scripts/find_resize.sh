#!/bin/bash
W=/mnt/c/Users/xasko/osito-k/windrv.bin
# 1) find UTF-16LE "ResizeViewport" in .rdata (fileoff 0xc000 = VMA 0x1110c000)
echo "=== label string locations ==="
python3 - <<'PY'
data = open('/mnt/c/Users/xasko/osito-k/windrv.bin','rb').read()
needle = 'UWindowsViewport::ResizeViewport'.encode('utf-16-le')
i = 0
while True:
    i = data.find(needle, i)
    if i < 0: break
    # map fileoff to VA: .rdata fileoff 0xc000 VMA 0x1110c000 (delta +0x11100000 across all sections since foff==rva here)
    print(f'fileoff=0x{i:x} VA=0x{0x11100000 + i:x}')
    i += 1
PY
