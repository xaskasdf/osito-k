#!/usr/bin/env python3
"""Verify authored binding expectations and actual sampler messages; no rendering."""
from pathlib import Path
import hashlib,json,re,struct,subprocess,sys

base=Path(sys.argv[1]);probe=Path(sys.argv[2]);out=base/'host-qualified';out.mkdir(exist_ok=False)
expected={
 'ps20-sample':(1,1,0),'ps30-sample':(1,1,0),'ps30-sparse':(0x8000,0x80,0),
 'ps30-pair':(0x8001,0x81,0),'ps30-dependent':(3,1,0),'ps30-tint':(1,1,4),
 'ps30-coordinate-swizzle':(8,4,0),'ps30-constant-coordinate':(1,0,0),
 'ps30-input-color':(0,0x10,0),'ps30-unused-declaration':(0,0,4),'ps30-dead-sample':(0,0,4),
 'ps20-hlsl-sample':(1,1,4),'ps30-hlsl-pair':(9,4,0),
}
cases=[]
for item in json.loads((base/'fixtures/manifest.json').read_text())['fixtures']:
 name=item['name'];data=(base/'fixtures'/f'{name}.bin').read_bytes()
 assert hashlib.sha256(data).hexdigest()==item['sha256']
 cases.append((name,data,2 if name.startswith('reject-') else 0,expected.get(name,(0,0,0))))
words=list(struct.unpack('<15I',(base/'fixtures/ps30-sample.bin').read_bytes()))
assert words[1]&0xffff==31 and words[4]&0xffff==31 and words[7]&0xffff==66
mutations=[]
w=words.copy();w[3]&=~(2<<16);mutations.append(('invalid-missing-y',w,1))
w=words.copy();w[10]=(w[10]&~0x7ff)|1;mutations.append(('invalid-undeclared-sampler',w,1))
w=words.copy();w[9]=(w[9]&~0x7ff)|1;mutations.append(('invalid-undeclared-input',w,1))
w=words[:4]+words[1:4]+words[4:];mutations.append(('invalid-duplicate-input',w,1))
w=words[:7]+words[4:7]+words[7:];mutations.append(('invalid-duplicate-sampler',w,1))
w=words[:4]+words[7:11]+words[4:7]+words[11:];mutations.append(('invalid-late-sampler',w,1))
w=words.copy();w[6]=(w[6]&~0x7ff)|16;mutations.append(('unsupported-sampler16',w,2))
w=words.copy();w[10]=(w[10]&~0x00ff0000)|0x001b0000;mutations.append(('unsupported-sampler-swizzle',w,2))
w=words.copy();w[9]|=1<<24;mutations.append(('unsupported-coordinate-negate',w,2))
for name,w,status in mutations:cases.append((name,struct.pack('<'+'I'*len(w),*w),status,(0,0,0)))
records=[];header=['/* Authored inputs, independent Microsoft assembly, compiler-output consistency references. */'];rows=[]
for index,(name,data,status,bindings) in enumerate(cases):
 source=out/f'{name}.bin';destination=out/f'{name}.program';source.write_bytes(data)
 p=subprocess.run([str(probe),str(source),str(destination)],capture_output=True,timeout=30)
 log=p.stdout+p.stderr;(out/f'{name}.log').write_bytes(log)
 assert p.returncode==(3 if status else 0),(name,p.returncode,log[-1500:])
 assert f'SAMPLER status={status} '.encode() in log,(name,log[:300])
 reference=b''
 if status:
  assert b'unchanged=1' in log and not destination.exists()
 else:
  reference=destination.read_bytes();h=struct.unpack('<32I',reference[:128]);samplers,coords,params=bindings
  assert h[0:2]==(3,128) and h[2]==len(reference) and h[11]==0
  assert h[27:30]==(samplers,samplers,coords) and h[24]==params and h[31]==0,(name,h)
  assert h[23]==(1 if coords else 0),(name,h[23])
  slots=[]
  for semantic in range(8):
   slot=(h[30]>>(semantic*4))&15
   assert (slot!=15)==bool(coords&(1<<semantic))
   if slot!=15:assert slot<h[22] and slot not in slots;slots.append(slot)
  if h[25]!=0xffffffff:assert h[25]<h[22] and h[25] not in slots;slots.append(h[25])
  assert len(slots)==h[22]
  actual=0
  for surface,sampler in re.findall(rb'sampler MsgDesc: sample SIMD(?:8|16|32) Surface = (\d+) Sampler = (\d+)',log):
   surface,sampler=int(surface),int(sampler);assert 0<=sampler<16 and surface==1+sampler
   actual|=1<<sampler
  assert actual==samplers,(name,actual,samplers)
  assert b'RT write' in log and b'ERROR' not in log
  assert reference[h[4]:h[4]+h[5]]==data
  for scalar in struct.unpack('<'+'I'*params,reference[h[26]:h[26]+params*4]):assert scalar<896
 header.append(f'static const unsigned char sampler_input_{index}[]={{'+','.join(str(v) for v in data)+'};')
 header.append(f'static const unsigned char sampler_reference_{index}[]={{'+','.join(str(v) for v in reference or b'\0')+'};')
 rows.append(f'{{"{name}",sampler_input_{index},{len(data)},{status},{bindings[0]},{bindings[1]},{bindings[2]},sampler_reference_{index},{len(reference)}}}')
 records.append(dict(name=name,status=status,passed=True,input_sha256=hashlib.sha256(data).hexdigest(),program_sha256=hashlib.sha256(reference).hexdigest() if reference else None))
 print(name,'PASS',flush=True)
header.append('static const struct sampler_fixture sampler_fixtures[]={'+','.join(rows)+'};')
(base/'sampler-fixtures.h').write_text('\n'.join(header)+'\n')
(out/'results.json').write_text(json.dumps(dict(passed=True,gpu_execution=False,tests=records),indent=2)+'\n')
