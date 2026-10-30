#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/hwbp-trace-pe}
test ! -e "$out"
mkdir -p "$out"
cp arch/x86/test/hwbp_trace_pe.c arch/x86/test/hwbp-trace-kernel32.def "$out/"
for bits in 32 64; do
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/hwbp-trace-kernel32.def "$out/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/hwbp-trace-kernel32.def > "$out/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32.def" -l "$out/kernel32.lib"
    clang --target="$target" -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
        -fno-stack-protector -c arch/x86/test/hwbp_trace_pe.c -o "$out/trace$bits.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$out/trace$bits.exe" "/map:$out/trace$bits.map" "$out/trace$bits.obj" "$out/kernel32.lib"
done
python3 - "$out" <<'PY'
import hashlib, json, re, struct, sys
from pathlib import Path
out = Path(sys.argv[1])
records = []
for bits in (32, 64):
    data = (out / f'trace{bits}.exe').read_bytes()
    match = re.search(r'\bhwbp_trace_point\s+([0-9a-fA-F]+)',
                      (out / f'trace{bits}.map').read_text())
    assert match, 'missing assembly trace symbol'
    addr = int(match.group(1), 16)
    sample = re.search(r'\b_?sample\s+([0-9a-fA-F]+)',
                       (out / f'trace{bits}.map').read_text())
    assert sample, 'missing sample entry'
    sample_addr = int(sample.group(1), 16)
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    count = struct.unpack_from('<H', data, pe + 6)[0]
    base = struct.unpack_from('<Q' if bits == 64 else '<I', data,
                              pe + 24 + (24 if bits == 64 else 28))[0]
    for i in range(count):
        sec = pe + 24 + optional_size + i * 40
        virtual_size, rva, size, offset = struct.unpack_from('<4I', data, sec + 8)
        delta = addr - base - rva
        if 0 <= delta and delta + 16 <= size:
            assert struct.unpack_from('<I', data, sec + 36)[0] & 0x20000000
            signature = data[offset + delta:offset + delta + 16]
            assert signature == b'\x90' * 16
            sample_delta = sample_addr - base - rva
            prefix = data[offset + sample_delta:offset + delta]
            assert prefix[:1] == b'\x57', 'return offset requires the saved EDI/RDI only'
            assert len(prefix) == (21 if bits == 64 else 11)
            callers = []
            code = data[offset:offset + size]
            position = code.find(b'\xe8')
            while 0 <= position <= len(code) - 5:
                call = base + rva + position
                target = call + 5 + struct.unpack_from('<i', code, position + 1)[0]
                if target == sample_addr:
                    callers.append(hex(call + 5))
                position = code.find(b'\xe8', position + 1)
            assert callers, 'sample has no direct callers'
            records.append(dict(bits=bits, address=hex(addr), signature=signature.hex(),
                                sha256=hashlib.sha256(data).hexdigest(),
                                caller_offset=bits // 8, caller_returns=callers))
            break
    else:
        raise RuntimeError('trace instruction outside executable raw section')
(out / 'probes.json').write_text(json.dumps(records, indent=2) + '\n')
print(json.dumps(records))
PY
