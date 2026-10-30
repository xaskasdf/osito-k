"""Capture read-only Windows FoldStringW/GetShortPathNameW contracts."""
from pathlib import Path
import ctypes as C, itertools, json, random
root = Path(__file__).resolve().parents[3]
k = C.WinDLL('kernel32', use_last_error=True)
fold = k.FoldStringW
fold.argtypes = [C.c_uint, C.c_void_p, C.c_int, C.c_void_p, C.c_int]
fold.restype = C.c_int
short = k.GetShortPathNameW
short.argtypes = [C.c_void_p, C.c_void_p, C.c_uint]
short.restype = C.c_uint

def units(text):
    raw = text.encode('utf-16-le', 'surrogatepass')
    return [int.from_bytes(raw[i:i+2], 'little') for i in range(0, len(raw), 2)]

def capture(flags, text, length, capacity, kind):
    source = (C.c_ushort*64)(*(units(text)+[0])) if text is not None else None
    target = (C.c_ushort*64)(*([90]*64))
    output = [target, None, source][kind]
    C.set_last_error(0x1234)
    result = fold(flags, source, length, output, capacity)
    return [flags, text is None, length, capacity, kind, result,
            C.get_last_error(), units(text or '')+[0], list(target)[:48]]

rows = []
for flags, text, length, capacity, kind in itertools.product(
    [0,16,32,64,128,8192,48,80,96,8208,8224,8256,8320,0x80000000],
    [None,'','A\u0308\u00e6\u00df\u2460\uff11'], [-2,-1,0,3],
    [0,1,3,8,32], range(3)):
    if text is not None and length > len(text)+1: continue
    rows.append(capture(flags,text,length,capacity,kind))
flags_list = [16,32,64,128,8192,80,144,160,192,8208,8320,8336]
samples = ['A\u0308','\u00c4','\u00e6\u00df\ufb03',
           '\u2460\u00b2\uff11\u0663\U0001d7ce','\u212b','\uac01',
           '\u1100\u1161\u11a8','A\0B','\ud800','\udc00',
           'A\u0315\u0300','\U0001f600','\ufdd0','\U0010ffff',
           '\u32ff\ua7f2\U000105c9','\u0345\U0001e08f\u0334',
           'A\u0305\u0301','\u0301A\u0327\u0301','\u1100\u1161\u11a8\u11a8']
for text, flags, capacity in itertools.product(samples,flags_list,[0,1,2,3,4,8,32]):
    rows.append(capture(flags,text,len(units(text)),capacity,0))
random.seed(4710)
alphabet = list('abc\0\u00c5\u00e6\u00df\ufb03\u0308\u0315\u0300\u0334\u0345\u00b2\u0663\uff11\u2460\u212b\u1100\u1161\u11a8')+['\U0001d7ce','\U0001f600']
for _ in range(240):
    text=''.join(random.choices(alphabet,k=random.randrange(1,10)))
    flags=random.choice(flags_list)
    rows.append(capture(flags,text,len(units(text)),48,0))

paths=[]
for text,size,kind in itertools.product(
    [None,'','C:\\','C:\\Windows','C:\\WINDOWS','C:\\missing-osito-47e2.tmp'],
    [0,1,3,4,10,11,64],range(3)):
    source=(C.c_ushort*128)(*(units(text)+[0])) if text is not None else None
    target=(C.c_ushort*128)(*([90]*128))
    output=[target,None,source][kind]
    C.set_last_error(0x1234);result=short(source,output,size);error=C.get_last_error()
    paths.append([text is None,size,kind,result,error,units(text or '')+[0],list(target)[:48]])

def numbers(values): return '{'+','.join(str(n) for n in values)+'}'
lines=['/* Native Windows contracts; regenerate with scripts/generate-fold-cases.py. */',
       'typedef struct { DWORD flags; int null_source,length,capacity,kind,result; DWORD error; WCHAR input[32],output[48]; } FOLD_CASE;',
       'static const FOLD_CASE fold_cases[] = {']
for row in rows:
    assert len(row[7]) <= 32
    lines.append('    {'+','.join(str(int(n))+'U' if i==0 else str(int(n)) for i,n in enumerate(row[:7]))+','+numbers(row[7])+','+numbers(row[8])+'},')
lines += ['};','typedef struct { int null_source; DWORD capacity; int kind; DWORD result,error; WCHAR input[64],output[48]; } SHORT_CASE;',
          'static const SHORT_CASE short_cases[] = {']
for row in paths: lines.append('    {'+','.join(str(int(n)) for n in row[:5])+','+numbers(row[5])+','+numbers(row[6])+'},')
lines.append('};')
(root/'arch/x86/test/fold_string_cases.h').write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
print(json.dumps({'fold_cases':len(rows),'short_cases':len(paths)}))
