"""Embed authored bytecode and previously qualified native output in API tests."""
from pathlib import Path
import hashlib, json, sys

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
base = Path('arch/x86/build')
reference = base / 'native-intel-compiler-pe64'
result = json.loads((reference / 'results.json').read_text())
assert result['passed'] and len(result['positive']) == 34 and len(result['rejections']) == 20
cases = []
for case in result['positive']:
    code = (base / 'native-compiler-fixtures' / (case['name'] + '.bin')).read_bytes()
    native = (reference / (case['name'] + '.eu')).read_bytes()
    assert hashlib.sha256(native).hexdigest() == case['sha256']
    cases.append((case['name'], code, native, 0))
for case in result['rejections']:
    folder = base / 'native-shader-fixtures' if case['status'] == 'unsupported' else reference
    cases.append((case['name'], (folder / (case['name'] + '.bin')).read_bytes(), b'',
                  2 if case['status'] == 'unsupported' else case['status']))
lines = ['/* Generated from authored fixtures and qualified Intel output. */']
for i, (_, code, native, _) in enumerate(cases):
    lines.append(f'static const unsigned char api_input_{i}[] = {{' + ','.join(map(str, code)) + '};')
    if native: lines.append(f'static const unsigned char api_native_{i}[] = {{' + ','.join(map(str, native)) + '};')
lines.append('static const struct compile_fixture api_fixtures[] = {')
for i, (name, code, native, status) in enumerate(cases):
    lines.append('{' + f'{json.dumps(name)},api_input_{i},{len(code)},{status},' +
                 (f'api_native_{i}' if native else 'NULL') + f',{len(native)}' + '},')
lines.append('};')
(out / 'compiler-api-fixtures.h').write_text('\n'.join(lines) + '\n')
