"""Append a valid zero-fill section exceeding the 2 GiB guest's RAM.

The output remains a tiny file. Only the explicitly selected guest probe loads
it; the native reference uses normal-sized DLLs and failed attach callbacks.
"""
from pathlib import Path
import struct,sys

root=Path(sys.argv[1])
for bits in (32,64):
    directory=root/f'pe{bits}'
    data=bytearray((directory/'lifecycle-good.dll').read_bytes())
    pe=struct.unpack_from('<I',data,60)[0]
    count=struct.unpack_from('<H',data,pe+6)[0]
    optional=pe+24
    table=optional+struct.unpack_from('<H',data,pe+20)[0]
    headers=struct.unpack_from('<I',data,optional+60)[0]
    offset=table+count*40
    assert offset+40<=headers
    image_size=struct.unpack_from('<I',data,optional+56)[0]
    assert image_size%4096==0
    zeros=0x90000000
    struct.pack_into('<8sIIIIIIHHI',data,offset,b'.oom\0\0\0\0',zeros,image_size,0,0,0,0,0,0,0xc0000080)
    struct.pack_into('<H',data,pe+6,count+1)
    struct.pack_into('<I',data,optional+56,image_size+zeros)
    struct.pack_into('<I',data,optional+12,struct.unpack_from('<I',data,optional+12)[0]+zeros)
    assert len(data)<16384
    (directory/'dependency-huge.dll').write_bytes(data)
