#!/bin/bash
# Pre-extract disasm regions for the B8 / B1B2 parallel workflow agents.
set -e
R=/mnt/c/Users/xasko/osito-k
OUT=$R/arch/x86/out
mkdir -p "$OUT"
WINDRV=$R/windrv.bin     # WinDrv.dll  base 0x11100000
CORE=$R/Core.dll         # Core.dll    base 0x10100000
OBJ=/usr/bin/objdump
DA="$OBJ -d -M intel --no-show-raw-insn"

echo "[1/4] WinDrv ViewportWndProc + window/input regions"
{
  echo "### WinDrv.dll base 0x11100000  (file windrv.bin)"
  echo "### section headers"
  $OBJ -h "$WINDRV" 2>/dev/null | sed -n '1,30p'
  echo
  echo "### candidate ViewportWndProc @ 0x11107030 (RVA 0x7030) — wide net 0x6f00-0x7400"
  $DA --start-address=0x11106f00 --stop-address=0x11107400 "$WINDRV" 2>/dev/null
} > "$OUT/windrv_viewportwndproc.txt"

echo "[2/4] WinDrv CauseInputEvent 0x6560 + SetMouseCapture 0x6610"
{
  echo "### CauseInputEvent @ 0x11106560  +  SetMouseCapture/recenter @ 0x11106610"
  $DA --start-address=0x11106500 --stop-address=0x111067a0 "$WINDRV" 2>/dev/null
} > "$OUT/windrv_input.txt"

echo "[3/4] WinDrv import table (what message-handling fns it calls in user32/etc)"
{
  echo "### WinDrv imports"
  $OBJ -p "$WINDRV" 2>/dev/null | sed -n '/The Import Tables/,/^$/p' | head -250
} > "$OUT/windrv_imports.txt"

echo "[4/4] Core.dll B8 sites: AV 0x1014ADBC + FString-build 0x10107900"
{
  echo "### Core.dll base 0x10100000  (file Core.dll)"
  echo "### B8 write-AV site @ 0x1014ADBC (mov word [eax],0) — context 0x1014AD60-0x1014AE40"
  $DA --start-address=0x1014ad60 --stop-address=0x1014ae40 "$CORE" 2>/dev/null
  echo
  echo "### FString-build / wcscpy(L\"0\") caller @ 0x10107900-0x10107a20 (outer_eip 0x1010796d)"
  $DA --start-address=0x10107880 --stop-address=0x10107a20 "$CORE" 2>/dev/null
} > "$OUT/core_b8.txt"

echo "done. files:"
ls -la "$OUT"/windrv_*.txt "$OUT"/core_b8.txt
echo "=== serial excerpt (B8/B9 + input/viewport lines) ==="
L=/root/osito-run/serial.log
{
  echo "### last 120 lines"
  tail -120 "$L"
  echo
  echo "### viewport / mouse / input / WM_ lines (last 60)"
  grep -anE -i 'viewport|WM_KEY|WM_MOUSE|CauseInput|SetCapture|mouse|MapView|InputEvent|UWindowsClient' "$L" | tail -60
} > "$OUT/serial_b8b9.txt"
wc -l "$OUT/serial_b8b9.txt"
