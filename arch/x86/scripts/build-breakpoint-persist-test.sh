#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/breakpoint_persist_pe.c -o "$dir/probe.obj"
    lld-link /entry:breakpoint_persist_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/bppersist$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
dir="$out/pe64"
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -Wall -Wextra -Werror -DLOOKUP_CHECKPOINT_PROBE=1 \
    -c arch/x86/test/breakpoint_persist_pe.c -o "$dir/lookup.obj"
lld-link /entry:breakpoint_persist_entry /subsystem:console /nodefaultlib /machine:x64 \
    /fixed /base:0x183b40000 /filealign:4096 \
    "/out:$dir/lookuppersist64.exe" "$dir/lookup.obj" "$dir/kernel32.lib"
python3 - "$dir/lookuppersist64.exe" <<'PY'
import struct, sys
from pathlib import Path
path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
pe, = struct.unpack_from('<I', data, 0x3c)
count, = struct.unpack_from('<H', data, pe + 6)
optional_size, = struct.unpack_from('<H', data, pe + 20)
optional = pe + 24
assert struct.unpack_from('<H', data, optional)[0] == 0x20b
assert struct.unpack_from('<Q', data, optional + 24)[0] == 0x183b40000
assert struct.unpack_from('<I', data, optional + 36)[0] == 4096
assert struct.unpack_from('<I', data, optional + 56)[0] <= 0xe000
header = optional + optional_size + count * 40
assert header + 40 <= struct.unpack_from('<I', data, optional + 60)[0]
assert not any(data[header:header + 40])
raw = (len(data) + 4095) & ~4095
data.extend(bytes(raw + 4096 - len(data)))
struct.pack_into('<8sIIIIIIHHI', data, header, b'.bpstub\0',
                 4096, 0xe000, 4096, raw, 0, 0, 0, 0, 0xe0000020)
struct.pack_into('<H', data, pe + 6, count + 1)
struct.pack_into('<I', data, optional + 56, 0xf000)
code_size, = struct.unpack_from('<I', data, optional + 4)
struct.pack_into('<I', data, optional + 4, code_size + 4096)
path.write_bytes(data)
PY
