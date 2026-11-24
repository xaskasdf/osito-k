#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L")"
echo "=== context around CTF-Orbital / LoadMap / LoadPackage ==="
grep -anE 'Orbital|LoadMap|LoadPackage|GetPackageLinker|Browse|InitGame' "$L" | tail -30
echo "=== Critical / appError / Cant find / package ==="
grep -anE 'Critical|appError|Can.t find|Cannot find|find file|find package' "$L" | tail -30
echo "=== _CxxThrowException context (with surrounding) ==="
grep -anF '_CxxThrowException' "$L" | tail -6
echo "=== CXX / THROWMSG / wcscpy 0 ==="
grep -anE 'CXX-F[0-9]|THROWMSG|CXX-OBJ|src=L"0"' "$L" | tail -20
echo "=== last 30 lines ==="
tail -30 "$L"
