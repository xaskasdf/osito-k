#!/usr/bin/env python3
from pathlib import Path
import hashlib
import json
import subprocess
import sys
base, probe = map(Path, sys.argv[1:3]);records=[]
for item in json.loads((base/'manifest.json').read_text())['fixtures']:
    src=base/(item['name']+'.bin');assert hashlib.sha256(src.read_bytes()).hexdigest()==item['sha256']
    p=subprocess.run([str(probe),str(src),*item['expected']],capture_output=True,timeout=30)
    (base/(item['name']+'.log')).write_bytes(p.stdout+p.stderr)
    assert p.returncode==0 and b'passed=1' in p.stdout,(item['name'],p.stdout,p.stderr)
    records.append(dict(name=item['name'],passed=True))
(base/'results.json').write_text(json.dumps(dict(passed=True,gpu_execution=False,tests=records),indent=2)+'\n')
print('Vertex arithmetic:',len(records),'PASS')
