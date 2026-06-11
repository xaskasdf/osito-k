#!/bin/bash
echo "=== all qemu procs (pid ppid args) ==="
ps -eo pid,ppid,etime,args | grep -i 'qemu-system-x86_64' | grep -v grep
echo "=== my run.out ==="
cat /root/run.out 2>/dev/null | tail -25
echo "=== serial mtime / size ==="
ls -la /root/osito-run/serial.log 2>/dev/null
