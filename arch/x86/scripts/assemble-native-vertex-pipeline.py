#!/usr/bin/env python3
from pathlib import Path
import ctypes as c
import hashlib,importlib.util,json,os,sys
spec=importlib.util.spec_from_file_location('assembler',Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler=importlib.util.module_from_spec(spec);spec.loader.exec_module(assembler)
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
compiler=Path(os.environ['SystemRoot'])/'System32/d3dcompiler_47.dll'
dll=c.WinDLL(str(compiler))
dll.D3DAssemble.argtypes=[c.c_void_p,c.c_size_t,c.c_char_p,c.c_void_p,c.c_void_p,c.c_uint,c.POINTER(c.c_void_p),c.POINTER(c.c_void_p)]
dll.D3DAssemble.restype=c.c_long
records=[]
for profile in (2,3):
 position,tex=('oPos','oT0') if profile==2 else ('o0','o7')
 source=f'vs_{profile}_0\ndcl_position v0\ndcl_texcoord0 v15\n'
 if profile==3:source+='dcl_position o0\ndcl_texcoord0 o7\n'
 source+=f'm4x4 {position}, v0, c0\nmov {tex}, v15\n'
 name=f'vs{profile}0-transform'
 data=assembler.assemble(dll,name,source)
 (out/(name+'.bin')).write_bytes(data)
 records.append(dict(name=name,source=source,sha256=hashlib.sha256(data).hexdigest()))
(out/'assembly.json').write_text(json.dumps(dict(compiler=str(compiler),compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),fixtures=records),indent=2)+'\n')
print('Assembled',len(records),'owned shaders; no runtime files copied')
