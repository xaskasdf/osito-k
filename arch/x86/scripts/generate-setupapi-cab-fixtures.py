from pathlib import Path
import struct,zlib,shutil,sys
# Usage: generate-setupapi-cab-fixtures.py OUTPUT ORIGINAL_DXUPDATE_CAB
# The Microsoft CAB remains external test data, never an OS build input.
root=Path(sys.argv[1])
original=Path(sys.argv[2])
assert original.is_file()
for bits in (32,64):(root/f'pe{bits}').mkdir(parents=True,exist_ok=True)
files=[(b'alpha.txt',b'hello cabinet\n',0x20),(b'empty.bin',b'',0x20),(b'dir\\beta.bin',bytes((i*17+3)&255 for i in range(70000)),0x20)]
entries=bytearray();payload=b''
for name,data,attr in files:
 entries+=struct.pack('<IIHHHH',len(data),len(payload),0,0x5421,0x645c,attr)+name+b'\0';payload+=data
for method in (0,1):
 blocks=bytearray();count=0
 for start in range(0,len(payload),32768):
  chunk=payload[start:start+32768]
  if method:
   args=dict(level=9,wbits=-15)
   if start:args['zdict']=payload[max(0,start-32768):start]
   comp=zlib.compressobj(**args);data=b'CK'+comp.compress(chunk)+comp.flush()
  else:data=chunk
  blocks+=struct.pack('<IHH',0,len(data),len(chunk))+data;count+=1
 offset=44+len(entries);size=offset+len(blocks)
 cabinet=struct.pack('<4sIIIIIBBHHHHH',b'MSCF',0,size,0,44,0,3,1,1,len(files),0,0x1234,2)+struct.pack('<IHH',offset,count,method)+entries+blocks
 name='mszip.cab' if method else 'stored.cab'
 for bits in (32,64):
  dest=root/f'pe{bits}';(dest/name).write_bytes(cabinet)
  if method:
   b=bytearray(cabinet);struct.pack_into('<I',b,offset,0x12345678);(dest/'bad-checksum.cab').write_bytes(b)
  else:(dest/'truncated.cab').write_bytes(cabinet[:-10])
for bits in (32,64):shutil.copyfile(original,root/f'pe{bits}/dxupdate.cab')
