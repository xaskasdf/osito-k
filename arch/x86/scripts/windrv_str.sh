#!/bin/bash
W=/mnt/c/Users/xasko/osito-k/windrv.bin
echo "=== sections ==="
objdump -h "$W" | sed -n '1,14p'
# Dump strings at the addresses used by the throw site
for va in 0x1110d154 0x1110d8c8 0x1110d140 0x1110d170; do
  rva=$(( va - 0x11100000 ))
  echo "--- VA $va (RVA $(printf 0x%x $rva)) ---"
  # find file offset: try .rdata mapping (read section table values)
  # generic: for each section, if rva in [vma, vma+size): foff = fileoff + (rva - vma)
  objdump -h "$W" | awk -v rva=$rva '
    /^ +[0-9]+ /{
      name=$2; size=strtonum("0x" $3); vma=strtonum("0x" $4); foff=strtonum("0x" $6);
      base=vma-0x11100000;
      if (rva>=base && rva<base+size) printf "%s off=0x%x\n", name, foff+(rva-base);
    }' | while read sec off; do
      o=${off#off=}
      dd if="$W" bs=1 skip=$((o)) count=96 2>/dev/null | iconv -f UTF-16LE -t UTF-8 2>/dev/null | head -c 80; echo
    done
done
