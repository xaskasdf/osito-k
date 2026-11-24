#!/usr/bin/env python3
"""Host qualification for D3D9 -> NIR -> Intel machine code (no GPU execution)."""
from pathlib import Path
import hashlib
import json
import subprocess
import struct
import sys

out = Path(sys.argv[1] if len(sys.argv) > 1 else 'arch/x86/build/native-intel-compiler-host')
probe = Path(sys.argv[2]) if len(sys.argv) > 2 else out / 'native-compiler'
out.mkdir(parents=True, exist_ok=True)
fixtures = Path('arch/x86/build/native-compiler-fixtures')
manifest = json.loads((fixtures / 'manifest.json').read_text())
records = []
for case in manifest['fixtures']:
    source = fixtures / (case['name'] + '.bin')
    assert hashlib.sha256(source.read_bytes()).hexdigest() == case['sha256']
    machine = out / (case['name'] + '.eu')
    result = subprocess.run([str(probe), str(source), str(machine)] + case['expected_bits'],
                            capture_output=True, timeout=30)
    log = result.stdout + result.stderr
    (out / (case['name'] + '.log')).write_bytes(log)
    assert result.returncode == 0, (case['name'], result.returncode, log[-2000:])
    assert b'D3D9 constant result matches all four expected channels' in log
    assert b'validated=1' in log and b'scratch=0' in log and b'RT write' in log
    records.append({'name': case['name'], 'passed': True, 'sha256': hashlib.sha256(machine.read_bytes()).hexdigest(), 'bytes': machine.stat().st_size})
old = Path('arch/x86/build/native-shader-fixtures')
rejections = []
for case in json.loads((old / 'manifest.json').read_text())['fixtures']:
    if case['name'] == 'ps20-constants':
        continue
    machine = out / (case['name'] + '.rejected.eu')
    assert not machine.exists()
    result = subprocess.run([str(probe), str(old / (case['name'] + '.bin')), str(machine)],
                            capture_output=True, timeout=30)
    assert result.returncode == 4 and b'frontend status=2' in result.stderr and not machine.exists(), case['name']
    rejections.append({'name': case['name'], 'status': 'unsupported', 'passed': True})
base = (fixtures / 'ps_3_0-mov.bin').read_bytes()
words = list(struct.unpack('<' + 'I' * (len(base) // 4), base))
positions = []
pos = 1
while words[pos] != 0xffff:
    positions.append(pos)
    pos += 1 + ((words[pos] >> 24) & 15)
definitions = [p for p in positions if (words[p] & 0xffff) == 81]
last_mov = [p for p in positions if (words[p] & 0xffff) == 1][-2]
mutations = []
case = words.copy(); case[definitions[1] + 1] = case[definitions[0] + 1]
mutations.append(('duplicate-def', case, 1))
case = words.copy(); case[last_mov + 2] = 0x80e4001f
mutations.append(('undefined-temp', case, 1))
case = words.copy(); case[last_mov + 2] = 0xa0e400c8
mutations.append(('unbound-constant', case, 2))
case = words[:-1] + [0x05000051, 0xa00f00c8, 0, 0, 0, 0, 0xffff]
mutations.append(('late-def', case, 1))
case = words.copy(); case[positions[-1] + 1] &= ~0x00080000
mutations.append(('incomplete-color', case, 1))
for name, tokens, status in mutations:
    source = out / (name + '.bin'); machine = out / (name + '.rejected.eu')
    source.write_bytes(struct.pack('<' + 'I' * len(tokens), *tokens))
    assert not machine.exists()
    result = subprocess.run([str(probe), str(source), str(machine)],
                            capture_output=True, timeout=30)
    assert result.returncode == 4 and f'frontend status={status} '.encode() in result.stderr and not machine.exists(), (name, result.stderr)
    rejections.append({'name': name, 'status': status, 'passed': True})
(out / 'results.json').write_text(json.dumps({'passed': True, 'host_only': True, 'gpu_execution': False,
    'positive': records, 'rejections': rejections,
    'probe_sha256': hashlib.sha256(probe.read_bytes()).hexdigest()}, indent=2) + '\n')
print('Native Intel compiler:', len(records), 'semantic/native-code cases and', len(rejections), 'explicit rejections passed')
