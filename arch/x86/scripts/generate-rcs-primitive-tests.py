"""Authored geometry and an integer edge-equation coverage oracle for Gen9 tests."""
from pathlib import Path
import hashlib,json,struct,sys

root=Path(__file__).resolve().parents[3]
old=root/'arch/x86/build/native-rcs-program'
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
fixtures=json.loads((old/'fixtures.json').read_text())['cases']
# Coordinates are in quarter-pixel units; every value is exactly binary32.
# This oracle is test code only. The driver uploads the original vertices and
# hardware performs rasterization; no CPU coverage code enters that path.
shapes=[
 ('right',4,[(32,32),(192,32),(32,160)]),
 ('right-reversed',4,[(32,160),(192,32),(32,32)]),
 ('oblique',4,[(51,17),(279,191),(29,229)]),
 ('negative',4,[(-40,32),(160,24),(32,200)]),
 ('outside',4,[(-160,-160),(-80,-40),(-8,-8)]),
 ('cover',4,[(-800,-800),(2400,-800),(-800,2400)]),
 ('collinear',4,[(20,20),(80,80),(140,140)]),
 ('duplicate',4,[(20,20),(20,20),(140,140)]),
 ('thin',4,[(41,21),(47,199),(51,21)]),
 ('two-triangles',4,[(40,40),(200,40),(40,180),(200,40),(200,180),(40,180)]),
 ('strip-quad',5,[(40,40),(40,180),(200,40),(200,180)]),
 ('strip-zigzag',5,[(12,16),(20,240),(76,28),(116,216),(196,32),(232,248)]),
 ('fan-quad',6,[(40,40),(200,40),(200,180),(40,180)]),
 ('fan-polygon',6,[(160,140),(44,60),(204,20),(320,124),(256,240),(80,220),(44,60)]),
 ('edge-origin',4,[(0,0),(80,0),(0,80)]),
 ('edge-bottom-right',4,[(304,208),(384,208),(384,288)]),
]
records=[];body=bytearray();total_pixels=0
for index,(name,topology,vertices) in enumerate(shapes):
 for clipped in (False,True):
  scissor=(17,13,47,39) if clipped else (0,0,96,72)
  triangles=[]
  if topology==4:triangles=[vertices[i:i+3] for i in range(0,len(vertices),3)]
  elif topology==5:triangles=[vertices[i:i+3] for i in range(len(vertices)-2)]
  else:triangles=[[vertices[0],vertices[i],vertices[i+1]] for i in range(1,len(vertices)-1)]
  coverage=bytearray(96*72//8)
  for triangle in triangles:
   a,b,c=triangle
   area=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])
   if not area:continue
   if area<0:b,c=c,b
   for y in range(scissor[1],scissor[1]+scissor[3]):
    for x in range(scissor[0],scissor[0]+scissor[2]):
     inside=True
     for u,v in ((a,b),(b,c),(c,a)):
      dx,dy=v[0]-u[0],v[1]-u[1]
      edge=dx*(y*4-u[1])-dy*(x*4-u[0])
      if edge<0 or (edge==0 and not (dy<0 or (dy==0 and dx>0))):inside=False;break
     if inside:coverage[(y*96+x)//8]|=1<<((y*96+x)%8)
  pixels=sum(n.bit_count() for n in coverage)
  raw_vertices=b''.join(struct.pack('<4f',x/4,y/4,0.0,1.0) for x,y in vertices)
  geometry=struct.pack('<10I',topology,len(vertices),*scissor,len(raw_vertices),len(coverage),0,0)+raw_vertices+coverage
  source=fixtures[(index*3)%len(fixtures)]
  program=(old/'programs'/(source['name']+'.program')).read_bytes()
  assert hashlib.sha256(program).hexdigest()==source['program_sha256']
  for mask in (1,2,3):
   body+=struct.pack('<4I',len(program),int(source['expected_bgra'],16),mask,len(geometry))+program+geometry
   records.append({'name':name+('-scissor' if clipped else ''),'mask':mask,
     'expected_bgra':source['expected_bgra'],'program_sha256':source['program_sha256'],
     'topology':topology,'vertices':len(vertices),'pixels':pixels,
     'coverage_sha256':hashlib.sha256(coverage).hexdigest()})
   total_pixels+=pixels
bundle=struct.pack('<4I',0x53504749,2,len(records),16+len(body))+body
(out/'compiled-fixtures.bin').write_bytes(bundle)
receipt={'cases':records,'bytes':len(bundle),'sha256':hashlib.sha256(bundle).hexdigest(),
         'pixels':total_pixels,'oracle':'Exact integer edge equations at D3D9 integer pixel centers; top-left edges included.',
         'source_programs':'arch/x86/build/native-rcs-program/programs'}
(out/'fixtures.json').write_text(json.dumps(receipt,indent=2)+'\n')
print('Geometry fixtures:',len(records),'draws',total_pixels,'pixels',len(bundle),'bytes',flush=True)
