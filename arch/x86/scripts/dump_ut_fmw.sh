#!/bin/bash
# Disasm UT.exe FMallocWindows pool manager (the NULL-write site FMW-POOL-SKIP masks)
UT=/mnt/c/Users/xasko/osito-k/UnrealTournament.exe
OUT=/mnt/c/Users/xasko/osito-k/arch/x86/out
OBJ=/usr/bin/objdump
DA="$OBJ -d -M intel --no-show-raw-insn"
echo "### UT.exe headers"; $OBJ -p "$UT" 2>/dev/null | grep -iE 'ImageBase|SizeOfImage' | head
echo "### sections"; $OBJ -h "$UT" 2>/dev/null | sed -n '1,20p'
{
  echo "### FMallocWindows pool manager region 0x10902000-0x10903400"
  echo "### (the NULL-target *PrevLink/*FirstMem writes @0x10902AF2 / 0x10902B2D that FMW-POOL-SKIP masks)"
  $DA --start-address=0x10902000 --stop-address=0x10903400 "$UT" 2>/dev/null
} > "$OUT/ut_fmw_pool.txt"
echo "wrote $OUT/ut_fmw_pool.txt ($(wc -l < "$OUT/ut_fmw_pool.txt") lines)"
echo "### focus: around 0x10902AF2 and 0x10902B2D (the NULL writes)"
$DA --start-address=0x10902a90 --stop-address=0x10902b80 "$UT" 2>/dev/null
