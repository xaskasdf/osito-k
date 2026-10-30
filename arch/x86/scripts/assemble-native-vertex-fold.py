#!/usr/bin/env python3
"""Generate constant arithmetic checks with independently stated results."""
from pathlib import Path
import ctypes as c
import hashlib
import importlib.util
import json
import os
import struct
import sys

spec = importlib.util.spec_from_file_location('assembler', Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler = importlib.util.module_from_spec(spec); spec.loader.exec_module(assembler)
dll = c.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32/d3dcompiler_47.dll'))
dll.D3DAssemble.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p, c.c_void_p, c.c_uint, c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
dll.D3DAssemble.restype = c.c_long
out = Path(sys.argv[1]); out.mkdir(exist_ok=True)
cases = [('mov', 'mov r0, r1.wzyx', [4,3,2,1]),
 ('add', 'add r0, r1, r2', [1.5,1,5,4]), ('sub', 'sub r0, r1, r2', [.5,3,1,4]),
 ('mul', 'mul r0, r1, r2', [.5,-2,6,0]), ('mad', 'mad r0, r1, r2, r3', [.75,-1.5,6.75,1]),
 ('min', 'min r0, r1, r2', [.5,-1,2,0]), ('max', 'max r0, r1, r2', [1,2,3,4]),
 ('abs', 'abs r0, r2', [.5,1,2,0]), ('frc', 'frc r0, c3', [.75,.5,.75,.5]),
 ('lrp', 'lrp r0, r2, r1, r3', [.625,-1,5.25,1]),
 ('slt', 'slt r0, r1, r2', [0,0,0,0]), ('sge', 'sge r0, r1, r2', [1,1,1,1]),
 ('dp3', 'dp3 r0, r1, r2', [4.5]*4), ('dp4', 'dp4 r0, r1, r2', [4.5]*4),
 ('partial', 'mov r0.xy, r1\nmov r0.zw, r2', [1,2,2,0]),
 ('partial-read', 'mov r0.xy, r1\nadd r0.xy, r0, r2\nmov r0.zw, r3', [1.5,1,.75,1]),
 ('self-swizzle', 'mov r0, r1\nmov r0.xy, r0.wzyx', [4,3,3,4]),
 ('negate', 'mov r0, -r1', [-1,-2,-3,-4]),
 ('m4x4', 'm4x4 r0, r1, c4', [1,4,9,144]),
 ('m4x3', 'm4x3 r0.xyz, r1, c4', [1,4,9,12]),
 ('m3x4', 'm3x4 r0, r1, c4', [1,4,9,140]),
 ('m3x3', 'm3x3 r0.xyz, r1, c4', [1,4,9,12]),
 ('m3x2', 'm3x2 r0.xy, r1, c4', [1,4,11,12])]
definitions = ('def c0,1,2,3,4\ndef c1,.5,-1,2,0\ndef c2,.25,.5,.75,1\n'
 'def c3,-.25,.5,1.75,-2.5\ndef c4,1,0,0,0\ndef c5,0,2,0,0\n'
 'def c6,0,0,3,0\ndef c7,10,20,30,1\ndef c255,7,8,11,12\n')
records=[]
for profile in (2,3):
    for name, body, expected in cases:
        position = 'oPos' if profile == 2 else 'o0'
        source = f'vs_{profile}_0\n' + ('dcl_position o0\n' if profile == 3 else '') + definitions
        source += 'mov r0,c255\nmov r1,c0\nmov r2,c1\nmov r3,c2\n' + body + f'\nmov {position},r0\n'
        name=f'vs{profile}0-{name}';data=assembler.assemble(dll,name,source)
        (out/(name+'.bin')).write_bytes(data)
        records.append(dict(name=name,source=source,sha256=hashlib.sha256(data).hexdigest(),
            expected=[f'{struct.unpack("<I",struct.pack("<f",value))[0]:08x}' for value in expected]))
(out/'manifest.json').write_text(json.dumps(dict(fixtures=records),indent=2)+'\n')
print('Vertex arithmetic fixtures:',len(records))
