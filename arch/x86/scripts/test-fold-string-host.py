"""Run allocation/concurrency safety tests plus an optional official UCD corpus.

Usage (Linux/WSL): test-fold-string-host.py OUT [NormalizationTest.txt]
The corpus is read from the caller's path; this test never downloads data.
"""
from pathlib import Path
import ctypes as C, hashlib, json, subprocess, sys
root=Path(__file__).resolve().parents[3]
out=Path(sys.argv[1]).resolve();out.mkdir(exist_ok=False,parents=True)
results={}
for compiler in ('gcc','clang'):
    exe=out/(compiler+'-fold-host')
    args=[compiler,'-std=c11','-O1','-g','-fsanitize=address,undefined',
          '-fno-omit-frame-pointer','-Wall','-Wextra','-Wno-misleading-indentation',
          '-pthread','-iquote','arch/x86/include','arch/x86/test/fold_string_host.c','-o',str(exe)]
    compiled=subprocess.run(args,cwd=root,capture_output=True)
    (out/(compiler+'-build.log')).write_bytes(compiled.stdout+compiled.stderr)
    assert compiled.returncode==0,compiled.stderr.decode(errors='replace')
    ran=subprocess.run([str(exe)],cwd=root,capture_output=True,timeout=90)
    (out/(compiler+'-run.log')).write_bytes(ran.stdout+ran.stderr)
    results[compiler]={'code':ran.returncode,'output':ran.stdout.decode(errors='replace')}
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(compiler,results[compiler],flush=True);assert ran.returncode==0
if len(sys.argv)>2:
    corpus=Path(sys.argv[2]).resolve()
    lib=out/'fold-host.so'
    subprocess.run(['gcc','-std=c11','-O2','-fPIC','-shared','-pthread','-iquote','arch/x86/include',
                    '-Wl,-Bsymbolic','arch/x86/test/fold_string_host.c','-o',str(lib)],cwd=root,check=True)
    call=C.CDLL(str(lib)).fold_host_call
    call.argtypes=[C.c_uint,C.c_void_p,C.c_int,C.c_void_p,C.c_int,C.POINTER(C.c_uint)]
    call.restype=C.c_int
    failures=[];checks=0;rows=0
    for line in corpus.read_text(encoding='utf-8').splitlines():
        line=line.split('#',1)[0].strip()
        if not line or line.startswith('@'):continue
        columns=[]
        for part in line.split(';')[:5]:
            raw=''.join(chr(int(p,16)) for p in part.split()).encode('utf-16-le')
            columns.append((raw,(C.c_ushort*(len(raw)//2)).from_buffer_copy(raw)))
        rows+=1
        for flags in [0x20,0x40,0x10,0x50]:
            for i,(raw,source) in enumerate(columns):
                expected=columns[(1 if i<3 else 3) if flags==0x20 else
                                 (2 if i<3 else 4) if flags==0x40 else
                                 3 if flags==0x10 else 4][0]
                target=(C.c_ushort*128)(*([90]*128));error=C.c_uint()
                count=call(flags,source,len(raw)//2,target,128,C.byref(error))
                checks+=1
                if count*2!=len(expected) or bytes(target)[:count*2]!=expected or error.value!=0x1234 or target[count]!=90:
                    if len(failures)<20:failures.append({'row':rows,'flags':flags,'column':i,'result':count,'error':error.value})
    results['unicode']={'corpus_sha256':hashlib.sha256(corpus.read_bytes()).hexdigest(),
                        'rows':rows,'checks':checks,'failures':failures}
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('unicode',results['unicode'],flush=True);assert not failures
