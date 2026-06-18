#!/bin/bash
L=/root/osito-run/serial.log
ln=$(grep -anE 'ExitProcess called' "$L" | tail -1 | cut -d: -f1)
echo "exit at line $ln"
echo "=== 120 lines before exit (what the engine did after the depth change) ==="
sed -n "$((ln-120)),$((ln))p" "$L" | grep -avE 'INT2E. callback return|CB32. longjmp' | tail -70
