#!/usr/bin/env python3
"""Exercise the production INF parser under Linux/WSL ASan and UBSan.

Pass infquery.inf created by build-setupapi-inf-query-test.sh's custom native
probe as --fixture. No installer or DLL is executed on the host.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fixture', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source_path = repo / 'arch/x86/win32/setupapi_inf.c'
source = source_path.read_text()
names = ['setup_inf_eq', 'setup_inf_trim', 'setup_inf_next_field', 'setup_inf_field',
         'setup_inf_find', 'setup_inf_directory', 'setup_inf_expand_text', 'setup_inf_expand',
         'setup_inf_parse', 'setup_inf_compact']
helpers = []
for name in ['INF_PATH', 'INF_TEXT', 'INF_QUERY_LIMIT', 'INF_MAX_LINES', 'INF_BAD_SECTION']:
    match = re.search(r'^#define ' + name + r' .+$', source, re.M)
    if not match:
        raise SystemExit('Production limit not found: ' + name)
    helpers.append(match.group())
for name in names:
    match = re.search(r'^static [^\n]*\b' + name + r'\b.*?^}', source, re.M | re.S)
    if not match:
        raise SystemExit('Production helper not found: ' + name)
    helpers.append(match.group())
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'setupapi_inf_parser.inc').write_text('\n\n'.join(helpers) + '\n')
command = ['clang', '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-I' + str(out),
           str(repo / 'arch/x86/test/setupapi_inf_host.c'), '-o', str(out / 'inf-host')]
subprocess.run(command, check=True)
result = subprocess.run([str(out / 'inf-host'), str(args.fixture.resolve())], capture_output=True)
(out / 'run.log').write_bytes(result.stdout + result.stderr)
record = {'command': command, 'code': result.returncode,
          'source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
          'fixture_sha256': hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
          'output': (result.stdout + result.stderr).decode(errors='replace')}
(out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
print(record['output'], end='')
raise SystemExit(result.returncode)
