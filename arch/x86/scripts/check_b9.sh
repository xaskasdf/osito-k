#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "PEFIX repaired   : $(grep -ac 'PEFIX' "$L")"
echo "exception handled: $(grep -ac 'exception handled by compat32' "$L")"
echo "Process crashed  : $(grep -ac 'Process crashed' "$L")"
echo "EXC32 vec=14     : $(grep -ac 'EXC32.*vec=14' "$L")"
echo "halt/hlt/HALT    : $(grep -aciE 'halt|hlt|spinning|deadlock|watchdog' "$L")"
echo "CR2 0x10295D30   : $(grep -ac '10295D30' "$L")"
echo "=== last PEFIX (up to 6) ==="
grep -anE 'PEFIX' "$L" | tail -6
echo "=== last 14 non-churn lines ==="
grep -avE 'CB32|INT2E callback|wcscpy' "$L" | grep -av '^$' | tail -14
