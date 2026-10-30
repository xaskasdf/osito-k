#!/usr/bin/env python3
"""Malformed-file and failed-I/O bounds checks, independent of Windows bugs."""
import ctypes as c,random,struct,sys
from pathlib import Path
lib=c.CDLL(sys.argv[1]);lib.host_bytes.restype=c.c_void_p;lib.host_position.restype=c.c_ulonglong
lib.host_reset.argtypes=[c.c_void_p,c.c_uint,c.c_uint,c.c_uint]
lib.host_add.argtypes=[c.c_void_p,c.POINTER(c.c_uint),c.c_int]
lib.host_enum.argtypes=[c.c_uint,c.POINTER(c.c_uint),c.POINTER(c.c_uint),c.c_uint,c.c_int]
base=Path('arch/x86/build/msgwait-20261003/ipinterface-probe-r1/pe32/ipinterface32.exe').read_bytes()
pe=struct.unpack_from('<I',base,60)[0];security=pe+24+128
cert=c.create_string_buffer(struct.pack('<IHH',13,512,2)+b'abcde')
checks=0
def test(ok):
    global checks
    checks+=1
    assert ok,checks
rng=random.Random(0x4950484c)
for mode in (0,1):
    for trial in range(512):
        data=bytearray(base)
        if trial<256:
            struct.pack_into('<II',data,security,rng.choice([0,len(data)+1,0xfffffffc,security-1]),rng.randrange(1,0xffffffff))
        else:
            at=rng.randrange(64)
            data[at]^=rng.randrange(1,256)
        source=c.create_string_buffer(bytes(data));lib.host_reset(source,len(data),3,mode)
        count=(c.c_uint*3)(0x12345678,0x87654321,0x12345678)
        indices=(c.c_uint*6)(*[0xa5a5a5a5]*6)
        lib.host_enum(255,c.cast(c.byref(count,4),c.POINTER(c.c_uint)),indices,4,0)
        test(count[0]==0x12345678 and count[2]==0x12345678)
        test(indices[4]==0xa5a5a5a5 and indices[5]==0xa5a5a5a5)
        test(lib.host_position()==17)
        index=(c.c_uint*3)(0x12345678,0x87654321,0x12345678)
        lib.host_add(cert,c.cast(c.byref(index,4),c.POINTER(c.c_uint)),0)
        test(index[0]==0x12345678 and index[2]==0x12345678)
        test(lib.host_size()<=len(data)+16)
        lib.host_remove(0,0)
        test(lib.host_size()<=len(data)+16)
    for fail in range(1,32):
        source=c.create_string_buffer(base);lib.host_reset(source,len(base),3,mode)
        lib.host_fail_at(fail);index=c.c_uint(0x12345678)
        result=lib.host_add(cert,c.byref(index),0)
        test(lib.host_size()<=len(base)+16)
        test(lib.host_error()==(0 if result else 87))
        test(lib.host_position()==(lib.host_size() if result else 17))
print(f'IMAGEHLP BOUNDS checks={checks} failures=0')
