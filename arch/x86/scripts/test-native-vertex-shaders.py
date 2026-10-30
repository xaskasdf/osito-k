#!/usr/bin/env python3
"""Check VS compiler contracts, failures and optional cross-platform identity.

This verifies compilation and EU encoding, not execution of the vertex stage.
"""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import sys

fixtures, probe, out = map(Path, sys.argv[1:4])
reference = Path(sys.argv[4]) if len(sys.argv) > 4 else None
out.mkdir(parents=True, exist_ok=False)
records = []
for item in json.loads((fixtures / 'manifest.json').read_text())['fixtures']:
    name = item['name']; src = fixtures / (name + '.bin'); dst = out / (name + '.program')
    data = src.read_bytes(); assert hashlib.sha256(data).hexdigest() == item['sha256']
    p = subprocess.run([str(probe), str(src), str(dst)], capture_output=True, timeout=30)
    log = p.stdout + p.stderr; (out / (name + '.log')).write_bytes(log)
    status = item['status']
    assert p.returncode == (3 if status else 0), (name, p.returncode, log[-1500:])
    assert f'VERTEX status={status} '.encode() in log and b'failures=0' in log, (name, log[-1500:])
    digest = None
    if status:
        assert b'unchanged=1' in log and not dst.exists()
    else:
        program = dst.read_bytes(); h = struct.unpack('<64I', program[:256])
        assert h[0:2] == (4,256) and h[2] == len(program) and h[8:10] == (0,1)
        assert h[32] == item['inputs'] and h[33] == item['outputs'], (name, h[32:34], item)
        assert h[6] + h[7] == len(program) and h[6] % 64 == 0
        assert program[h[4]:h[4]+h[5]] == data
        used = [h[52+n] for n in range(12) if h[33] & (1<<n)]
        assert len(used) == len(set(used)) and max(used) < 4*h[34]
        for n in range(16):
            assert bool(h[32] & (1<<n)) == (h[36+n] != 0xffffffff)
        if b'urb MsgDesc:' in log:
            assert b'SIMD8 write' in log and b'EOT' in log and b'ERROR' not in log
            assert b'RT write' not in log and b'sampler MsgDesc:' not in log
        if reference:
            assert program == (reference / dst.name).read_bytes(), name
        digest = hashlib.sha256(program).hexdigest()
    records.append(dict(name=name, status=status, passed=True, program_sha256=digest))
    print(name, 'PASS', flush=True)
(out / 'results.json').write_text(json.dumps(dict(passed=True, gpu_execution=False,
    probe=str(probe), probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
    reference=str(reference) if reference else None, tests=records), indent=2)+'\n')
print('Vertex compiler qualification:', len(records), 'PASS; GPU execution not tested')
