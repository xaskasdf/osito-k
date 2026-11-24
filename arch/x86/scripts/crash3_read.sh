#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L" 2>/dev/null)"
echo "=== the guest #PF at 0 (context) ==="
ln=$(grep -anE 'EXCEPTION: #PF' "$L" | head -1 | cut -d: -f1)
echo "first #PF at line $ln"
sed -n "$((ln-30)),$((ln+25))p" "$L" | grep -avE 'INT2E. callback return|CB32. longjmp' | tail -45
echo ""
echo "=== the kernel #GP + halt ==="
grep -anE '#GP|General Protection|SYSTEM HALTED|HALT' "$L" | tail -6
ln2=$(grep -anE 'General Protection' "$L" | tail -1 | cut -d: -f1)
if [ -n "$ln2" ]; then sed -n "$((ln2-20)),$((ln2+15))p" "$L" | tail -35; fi
