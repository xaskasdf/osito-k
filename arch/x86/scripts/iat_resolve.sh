#!/bin/bash
# Resolve UT.exe import names at given IAT slots (on-disk = hint/name RVAs)
UT=/mnt/c/Users/xasko/osito-k/UnrealTournament.exe
# .idata VMA 0x10958000 -> file offset 0x30000
for slot_rva in 0x58e44 0x58f28 0x58e40 0x58e48; do
  foff=$(( 0x30000 + slot_rva - 0x58000 ))
  hex=$(xxd -s $foff -l 4 -e "$UT" | awk '{print $2}')
  rva=$((16#$hex))
  printf "IAT 0x109%05x (file 0x%x) -> 0x%08x" $((slot_rva)) $foff $rva
  if [ $rva -gt $((0x58000)) ] && [ $rva -lt $((0x60000)) ]; then
    noff=$(( 0x30000 + rva - 0x58000 + 2 ))
    name=$(dd if="$UT" bs=1 skip=$noff count=40 2>/dev/null | tr -d '\0' | sed 's/[^[:print:]].*//')
    printf "  name=%s" "$name"
  fi
  echo
done
