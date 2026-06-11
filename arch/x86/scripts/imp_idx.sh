#!/bin/bash
# Resolve Core.dll import symbol at a given IAT index in Engine.dll.
cd /c/Users/xasko/osito-k 2>/dev/null || cd "$(dirname "$0")/../../.."
DLL=arch/x86/build/Engine.dll
# llvm-readobj prints, per imported DLL, a list of "Symbol: name (ordinal/hint)".
# We want Core.dll's Nth symbol (0-based index from arg, default 334).
N=${1:-334}
llvm-readobj --coff-imports "$DLL" 2>/dev/null > /tmp/imp.txt
# Extract the Core.dll block: from its "Name: Core.dll" to the next "Import {" or EOF.
awk '
  /Symbol: |Name: / { line=$0 }
  /Name: Core\.dll/ { incore=1; next }
  /Name: MSVCRT\.dll/ { incore=0 }
  incore && /Symbol:/ { idx++; print (idx-1)": "$0 }
' /tmp/imp.txt > /tmp/core_syms.txt
echo "total Core.dll symbols: $(wc -l < /tmp/core_syms.txt)"
echo "=== around index $N ==="
sed -n "$((N-3)),$((N+3))p" /tmp/core_syms.txt
