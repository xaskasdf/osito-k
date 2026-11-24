#!/usr/bin/env python3
"""Authored VS2/VS3 cases assembled by the installed Microsoft assembler."""
from pathlib import Path
import ctypes as c
import hashlib
import importlib.util
import json
import os
import struct
import sys

spec = importlib.util.spec_from_file_location('assembler', Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembler)
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
compiler = Path(os.environ['SystemRoot']) / 'System32/d3dcompiler_47.dll'
dll = c.WinDLL(str(compiler))
dll.D3DAssemble.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p, c.c_void_p, c.c_uint, c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
dll.D3DAssemble.restype = c.c_long
records = []

def save(name, data, status, source=None, inputs=None, outputs=1):
    (out / (name + '.bin')).write_bytes(data)
    records.append(dict(name=name, source=source, status=status, inputs=inputs, outputs=outputs,
                        bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))

def assemble(name, source, status=0, inputs=None, outputs=1):
    data = assembler.assemble(dll, name, source)
    save(name, data, status, source, inputs, outputs)
    return data

ops = [('mov', 'mov r0, v0', 1), ('swizzle', 'mov r0, -v0.wzyx', 1),
       ('add', 'add r0, v0, c0', 1), ('sub', 'sub r0, v0, c0', 1),
       ('mul', 'mul r0, v0, c0', 1), ('mad', 'mov r1, c1\nmad r0, v0, c0, r1', 1),
       ('min', 'min r0, v0, c0', 1), ('max', 'max r0, v0, c0', 1),
       ('abs', 'abs r0, v0', 1), ('frc', 'frc r0, v0', 1),
       ('slt', 'slt r0, v0, c0', 1), ('sge', 'sge r0, v0, c0', 1),
       ('dp3', 'dp3 r0, v0, c0', 1), ('dp4', 'dp4 r0, v0, c0', 1),
       ('partial', 'mov r0.xy, v0\nmov r0.zw, c255', 1),
       ('high-constant', 'mov r0, c255', 0),
       ('lrp', 'mov r1, c1\nlrp r0, v0, c0, r1', 1),
       ('saturate', 'mov_sat r0, v0', 1),
       ('sparse', 'mov r0, v15', 32768),
       ('pair', 'mov r1, v15\nadd r0, v0, r1', 32769)]
for profile in (2, 3):
    prefix = f'vs_{profile}_0\ndcl_position v0\ndcl_texcoord7 v15\n'
    position = 'oPos' if profile == 2 else 'o7'
    declarations = '' if profile == 2 else 'dcl_position o7\n'
    for name, body, inputs in ops:
        if profile == 2 and name == 'saturate': continue
        assemble(f'vs{profile}0-{name}', prefix + declarations + body + f'\nmov {position}, r0\n', inputs=inputs)
    for opcode, mask in [('m4x4', ''), ('m4x3', '.xyz'), ('m3x4', ''), ('m3x3', '.xyz'), ('m3x2', '.xy')]:
        assemble(f'vs{profile}0-{opcode}', prefix + declarations + f'mov r0, c255\n{opcode} r0{mask}, v0, c0\nmov {position}, r0\n', inputs=1)
    assemble(f'vs{profile}0-local-constant', prefix + declarations + f'def c255, 1, 2, 3, 4\nmov {position}, c255\n', inputs=0)
    extra = '' if profile == 2 else 'dcl_texcoord0 o3\ndcl_color0 o11\ndcl_color1 o0\n'
    tex, color, specular = ('oT0', 'oD0', 'oD1') if profile == 2 else ('o3', 'o11', 'o0')
    assemble(f'vs{profile}0-outputs', prefix + declarations + extra + f'mov {position}, v0\nmov {tex}, v15\nmov {color}, c0\nmov {specular}, c1\n', inputs=32769, outputs=15)
    assemble(f'vs{profile}0-relative', prefix + declarations + f'mova a0.x, v0.x\nmov {position}, c0[a0.x]\n', status=2)
    assemble(f'vs{profile}0-flow', prefix + declarations + f'if b0\nmov {position}, v0\nelse\nmov {position}, c0\nendif\n', status=2)
    for count in (128, 129):
        body = 'mov r0, c0\n' + ''.join(f'add r0, r0, c{k}\n' for k in range(1,count))
        assemble(f'vs{profile}0-constants{count}', prefix + declarations + body + f'mov {position}, r0\n', status=0 if count == 128 else 2, inputs=0)

prefix = 'vs_3_0\n' + ''.join(f'dcl_texcoord{k} v{k}\n' for k in range(16)) + 'dcl_position o0\n'
body = 'mov r0, v0\n' + ''.join(f'mov r1, v{k}\nadd r0, r0, r1\n' for k in range(1,16)) + 'mov o0, r0\n'
assemble('vs30-all-inputs', prefix + body, inputs=65535)
declarations = 'dcl_position o0\ndcl_color0 o1\ndcl_color1 o2\n' + ''.join(f'dcl_texcoord{k} o{k+3}\n' for k in range(8))
body = ''.join(f'mov o{k}, v0\n' for k in range(11))
assemble('vs30-all-outputs', 'vs_3_0\ndcl_position v0\n' + declarations + body, inputs=1, outputs=2047)

data = (out / 'vs30-mov.bin').read_bytes()
w = list(struct.unpack('<' + 'I' * (len(data)//4), data))
# Discover declaration and arithmetic instruction boundaries from token lengths.
starts=[]; pos=1
while w[pos] != 0xffff:
    starts.append(pos); pos += 1 + ((w[pos] >> 24) & 15)
declarations = [p for p in starts if (w[p]&0xffff) == 31]
movs = [p for p in starts if (w[p]&0xffff) == 1]
mutations=[]
x=w.copy(); x[movs[0]+2]=(x[movs[0]+2]&~0x7ff)|14; mutations.append(('undeclared-input',x,1))
x=w.copy(); x[movs[0]+2]=0x80e40001; mutations.append(('undefined-temp',x,1))
x=w.copy(); x[movs[1]+1]=(x[movs[1]+1]&~0x7ff)|5; mutations.append(('undeclared-output',x,1))
x=w.copy(); x[movs[1]+1]&=~(8<<16); mutations.append(('incomplete-position',x,1))
p=declarations[0]; x=w[:p]+w[p:p+3]+w[p:]; mutations.append(('duplicate-input',x,1))
p=declarations[-1]; x=w[:p]+w[p:p+3]+w[p:]; mutations.append(('duplicate-output',x,1))
x=w.copy(); x[declarations[1]+1]=x[declarations[0]+1]; mutations.append(('duplicate-semantic',x,1))
x=w.copy(); x[declarations[0]+2]=(x[declarations[0]+2]&~0x7ff)|16; mutations.append(('input16',x,2))
x=w.copy(); x[movs[0]+1]=(x[movs[0]+1]&~0x7ff)|32; mutations.append(('temp32',x,2))
x=w.copy(); x[movs[0]+2]=0xa0e40100; mutations.append(('constant256',x,2))
x=w.copy(); x[0]=0xffff0300; mutations.append(('pixel-profile',x,2))
x=w[:-1]; mutations.append(('truncated-end',x,1))
x=w[:movs[0]]+[0x03000002, 0x800f0000, 0x90e40000, 0x90e4000f]+w[movs[1]:]; mutations.append(('two-input-ports',x,1))
x=w[:movs[0]]+[0x03000002, 0x800f0000, 0xa0e40000, 0xa0e40001]+w[movs[1]:]; mutations.append(('two-constant-ports',x,1))
v20 = (out/'vs20-mov.bin').read_bytes(); v20 = list(struct.unpack('<'+'I'*(len(v20)//4),v20))
p = next(k for k in range(len(v20)) if v20[k] == 0x800f0000)
x=v20.copy(); x[p]=(x[p]&~0x7ff)|12; mutations.append(('vs20-temp12',x,2))
x=v20.copy(); x[p]|=1<<20; mutations.append(('vs20-saturate',x,2))
for name, words, status in mutations:
    save('reject-'+name, struct.pack('<'+'I'*len(words), *words), status)
(out / 'manifest.json').write_text(json.dumps(dict(assembler=str(compiler), assembler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), fixtures=records), indent=2)+'\n')
print('Vertex shader fixtures:', len(records))
