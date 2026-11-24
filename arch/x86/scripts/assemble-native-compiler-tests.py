#!/usr/bin/env python3
"""Generate authored native-compiler cases with the host's Microsoft assembler."""
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys

spec = importlib.util.spec_from_file_location('assembler', Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembler)
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
compiler_path = Path(os.environ['SystemRoot']) / 'System32/d3dcompiler_47.dll'
dll = c.WinDLL(str(compiler_path))
dll.D3DAssemble.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p,
                           c.c_void_p, c.c_uint, c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
dll.D3DAssemble.restype = c.c_long
cases = [
    ('mov', 'mov r0, c0.wzyx', [4,3,2,1]),
    ('add', 'add r0, c0, c1', [1.5,1,5,4]),
    ('sub', 'sub r0, c0, c1', [.5,3,1,4]),
    ('mul', 'mul r0, c0, c1', [.5,-2,6,0]),
    ('mad', 'mad r0, c0, c1, c2', [.75,-1.5,6.75,1]),
    ('min', 'min r0, c0, c1', [.5,-1,2,0]),
    ('max', 'max r0, c0, c1', [1,2,3,4]),
    ('abs', 'abs r0, c1', [.5,1,2,0]),
    ('frc', 'frc r0, c3', [.75,.5,.75,.5]),
    ('lrp', 'lrp r0, c1, c0, c2', [.625,-1,5.25,1]),
    ('dp3', 'dp3 r0, c0, c1', [4.5,4.5,4.5,4.5]),
    ('dp4', 'dp4 r0, c0, c1', [4.5,4.5,4.5,4.5]),
    ('mask', 'mov r0.xy, c0\nmov r0.zw, c1', [1,2,2,0]),
    ('partial-read', 'mov r0.xy, c0\nadd r0.xy, r0, c1\nmov r0.zw, c2', [1.5,1,.75,1]),
    ('self-swizzle', 'mov r0, c0\nmov r0.xy, r0.wzyx', [4,3,3,4]),
    ('saturate', 'mov_sat r0, c3', [0,.5,1,0]),
    ('negate', 'mov r0, -c0', [-1,-2,-3,-4]),
]
definitions = 'def c0, 1, 2, 3, 4\ndef c1, .5, -1, 2, 0\ndef c2, .25, .5, .75, 1\ndef c3, -.25, .5, 1.75, -2.5\n'
records = []
for profile in ('ps_2_0', 'ps_3_0'):
    for name, instructions, expected in cases:
        # PS2 has one constant-register read port per instruction. Materialize
        # the other operands just as an ordinary HLSL compiler would.
        body = instructions.replace('c1', 'r1').replace('c2', 'r2').replace('c3', 'r3')
        source = profile + '\n' + definitions + 'mov r1, c1\nmov r2, c2\nmov r3, c3\n' + body + '\nmov oC0, r0\n'
        name = profile + '-' + name
        data = assembler.assemble(dll, name, source)
        (out / (name + '.bin')).write_bytes(data)
        records.append({'name': name, 'source': source, 'bytes': len(data),
                        'sha256': hashlib.sha256(data).hexdigest(),
                        'expected_bits': [f'{struct.unpack("<I", struct.pack("<f", v))[0]:08x}' for v in expected]})
(out / 'manifest.json').write_text(json.dumps({'compiler': str(compiler_path),
    'compiler_sha256': hashlib.sha256(compiler_path.read_bytes()).hexdigest(), 'fixtures': records}, indent=2) + '\n')
print('Native compiler authored fixtures:', len(records))
