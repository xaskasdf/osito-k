#!/usr/bin/env python3
"""Audit the freestanding numeric objects, not the host test executables."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path)
args = parser.parse_args()
arch = Path(__file__).resolve().parents[1]
build = (args.build or arch / 'build').resolve()
sources = [arch / 'dos/dos_fpu_softfloat.cc', arch / 'dos/dos_fpu_state.cc']
for relative in ('lib/softfloat3e', 'lib/softfloat3e/8086-SSE', 'lib/bochs-transcendental'):
    sources.extend(sorted((arch / relative).glob('*.cc')))
objects = [build / source.relative_to(arch).with_suffix('.o') for source in sources]
undefined, defined = set(), set()
instructions = 0
for source, path in zip(sources, objects):
    if not path.is_file() or path.stat().st_mtime_ns < source.stat().st_mtime_ns:
        raise SystemExit(f'Missing or stale object: {path}')
    listing = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', str(path)], text=True)
    for line in listing.splitlines():
        match = re.match(r'\s*[0-9a-f]+:\s+(\S+)(.*)', line)
        if not match:
            continue
        mnemonic, operands = match.groups()
        if mnemonic.startswith('f') or re.search(r'%(?:[xyz]mm\d+|mm\d+|st\b)', operands):
            raise SystemExit(f'FP/SIMD in {path}: {line}')
        instructions += 1
    symbols = subprocess.check_output(['nm', '--format=posix', str(path)], text=True)
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        if fields[1] == 'U':
            undefined.add(fields[0])
        elif fields[1].isupper():
            defined.add(fields[0])
    sections = subprocess.check_output(['readelf', '-SW', str(path)], text=True)
    if re.search(r'\.(?:init_array|fini_array|ctors|dtors|gcc_except_table)\b', sections):
        raise SystemExit(f'Runtime initializer/destructor/exception section in {path}')
if undefined - defined:
    raise SystemExit(f'External numeric dependencies: {sorted(undefined - defined)}')
kernel = build / 'kernel.elf'
if not kernel.is_file() or any(kernel.stat().st_mtime_ns < p.stat().st_mtime_ns for p in objects):
    raise SystemExit('Missing or stale linked kernel')
symbols = subprocess.check_output(['nm', '--format=posix', str(kernel)], text=True)
if re.search(r'^dos_fpu_(?:host_step|register_ops|memory_ops|noop)\b', symbols, re.M):
    raise SystemExit('Native instruction fallback still linked into the kernel')
print(f'PASS numeric objects={len(objects)} instructions={instructions} '
      'external-symbols=0 constructors=0 exception-tables=0 FP/SIMD=0 native-fallback=0')
print(f'Kernel bytes={kernel.stat().st_size} sha256={hashlib.sha256(kernel.read_bytes()).hexdigest()}')
