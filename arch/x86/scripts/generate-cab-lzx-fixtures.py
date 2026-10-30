"""Create data-only CAB oracles on Windows with the installed makecab.exe.

Usage: python generate-cab-lzx-fixtures.py OUTPUT_DIRECTORY
No DirectX payload is a build input. Original CABs are supplied separately.
"""
from pathlib import Path
import json, random, struct, subprocess, sys

out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = out / 'synthetic-source'
source.mkdir(exist_ok=True)
rng = random.Random(0x1a2b3c)
noise = rng.randbytes(1400000)
data = bytearray(noise + noise[:900000] + noise[300000:1200000] + b'xyzabc' * 2345)
for offset in range(17, len(data) - 4, 997):
    data[offset:offset + 5] = b'\xe8' + struct.pack('<i', (offset * 13) % 100000 - 50000)
(source / 'mixed.bin').write_bytes(data)
(source / 'noise.bin').write_bytes(rng.randbytes(100001))
(source / 'odd.txt').write_bytes(b'cabinet odd raw block ' + b'x' * 16)
manifest = []
for bits in range(15, 22):
    directory = out / f'generated-{bits}'
    directory.mkdir(exist_ok=True)
    directive = directory / 'make.ddf'
    lines = ['.OPTION EXPLICIT', '.Set Cabinet=ON', '.Set Compress=ON',
             '.Set CompressionType=LZX', f'.Set CompressionMemory={bits}',
             '.Set MaxDiskSize=0', '.Set MaxCabinetSize=0', '.Set FolderSizeThreshold=0',
             f'.Set CabinetNameTemplate=lzx{bits}.cab', f'.Set DiskDirectoryTemplate={directory}']
    for name in ('mixed.bin', 'noise.bin', 'odd.txt'):
        lines.append(f'"{source / name}" {name}')
    directive.write_text('\n'.join(lines) + '\n', encoding='ascii')
    result = subprocess.run(['makecab.exe', '/F', str(directive)], cwd=directory, capture_output=True)
    (directory / 'makecab.log').write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    manifest.append({'cab': str(directory / f'lzx{bits}.cab'), 'reference': str(source)})
(out / 'synthetic-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
