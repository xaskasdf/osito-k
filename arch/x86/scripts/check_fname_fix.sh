#!/bin/bash
# Inspect the last UT99 serial.log for FName-fix regression markers.
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L")"
echo "--- counts ---"
printf 'Process crashed : %s\n' "$(grep -ac 'Process crashed' "$L")"
printf 'FNAME-NULL-FILL : %s\n' "$(grep -ac 'FNAME-NULL-FILL' "$L")"
printf 'FNAME-RESCUE    : %s\n' "$(grep -ac 'FNAME-RESCUE' "$L")"
printf 'src=L"0"        : %s\n' "$(grep -ac 'src=L.0.' "$L")"
printf 'Unhashed name   : %s\n' "$(grep -ac 'Unhashed name' "$L")"
printf 'find/Failed load: %s\n' "$(grep -aciE 'cant find|failed to load' "$L")"
printf 'throw1 0x10903EE4: %s\n' "$(grep -ac '0x10903EE4' "$L")"
echo "--- last 12 non-empty lines ---"
grep -av '^$' "$L" | tail -12
echo "--- title/menu markers ---"
grep -aiE 'RegisterClassEx|PeekMessage|GetMessage|InitEngine|Level is Level|UGameEngine|RegisterClass' "$L" | tail -8
