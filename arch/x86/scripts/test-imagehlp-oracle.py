#!/usr/bin/env python3
"""Compare production ImageHlp file behavior with captured native Windows runs."""
import ctypes as c,hashlib,json,struct,sys
from pathlib import Path
root=Path('arch/x86/build/msgwait-20261003/imagehlp-research')
lib=c.CDLL(sys.argv[1]);lib.host_bytes.restype=c.c_void_p;lib.host_position.restype=c.c_ulonglong
lib.host_reset.argtypes=[c.c_void_p,c.c_uint,c.c_uint,c.c_uint]
lib.host_add.argtypes=[c.c_void_p,c.POINTER(c.c_uint),c.c_int]
lib.host_enum.argtypes=[c.c_uint,c.POINTER(c.c_uint),c.POINTER(c.c_uint),c.c_uint,c.c_int]
checks=failures=0
base=Path('arch/x86/build/msgwait-20261003/ipinterface-probe-r1/pe32/ipinterface32.exe').read_bytes()
pe=struct.unpack_from('<I',base,60)[0];checksum=pe+24+64;security=pe+24+128
def check(actual,expected,label):
    global checks,failures
    checks+=1
    if actual!=expected:
        failures+=1;print('IMAGEHLP FAIL',label,repr(actual),repr(expected))
for mode in (0,1):
    for case in ('normal','overlay','readonly','writeonly','nonpe','truncated','null-index','null-certificate','enum-null-count','length-0','length-7','length-8','length-9','length-16'):
        reference=json.loads((root/(case+'.json')).read_text())
        data=base+(b'OVERLAY-BEFORE-CERT' if case=='overlay' else b'')
        if case=='nonpe':data=b'not a PE file'*10
        if case=='truncated':data=data[:300]
        check(hashlib.sha256(data).hexdigest(),reference['original_sha256'],case+' fixture hash')
        source=c.create_string_buffer(data);lib.host_reset(source,len(data),1 if case=='readonly' else 2 if case=='writeonly' else 3,mode)
        number=0
        for step,expected in enumerate(reference['actions']):
            label=f'{mode}/{case}/{step}/{expected["label"]}'
            count=c.c_uint(0xdeadbeef);index=c.c_uint(0xdeadbeef);indices=(c.c_uint*10)(*[0xa5a5a5a5]*10)
            lib.host_error_set(0x12345678)
            if expected['label']=='enum':
                result=lib.host_enum(expected['filter'],None if case=='enum-null-count' and step else c.byref(count),indices if expected['present'] else None,expected['capacity'],0)
                check(count.value,expected['count'],label+' count');check(list(indices),expected['indices'],label+' indices')
            elif expected['label']=='add':
                cert=bytearray(512);struct.pack_into('<IHH',cert,0,expected['length'],0x200,[2,1,2][number%3])
                for i in range(8,512):cert[i]=(i+number)&255
                number+=1;certificate=c.create_string_buffer(bytes(cert))
                result=lib.host_add(None if case=='null-certificate' else certificate,None if case=='null-index' else c.byref(index),0)
                check(index.value,expected['index'],label+' index')
            else:result=lib.host_remove(expected['index'],0)
            check(bool(result),expected['result'],label+' result');check(lib.host_error(),expected['error'],label+' error')
            check(lib.host_position(),expected['position'],label+' cursor');check(lib.host_size(),expected['size'],label+' size')
            actual=c.string_at(lib.host_bytes(),lib.host_size())
            entry=list(struct.unpack_from('<II',actual,security)) if len(actual)>=security+8 else [0,0]
            check(entry,expected['security'],label+' security')
            check(struct.unpack_from('<I',actual,checksum)[0] if len(actual)>=checksum+4 else None,expected['checksum'],label+' checksum')
            offset,length=entry;check(actual[offset:offset+min(length,192)].hex(),expected['certificate_bytes'],label+' certificate bytes')
        check(hashlib.sha256(actual).hexdigest(),reference['final_sha256'],case+' final file hash')
print(f'IMAGEHLP ORACLE checks={checks} failures={failures}')
sys.exit(bool(failures))
