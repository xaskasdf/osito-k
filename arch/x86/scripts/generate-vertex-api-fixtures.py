#!/usr/bin/env python3
from pathlib import Path
import json
import sys
base=Path(sys.argv[1]);fixtures=base/'fixtures';reference=base/'host-final'
assert json.loads((reference/'results.json').read_text())['passed']
header=['/* Authored Microsoft-assembled inputs; qualified compiler consistency outputs. */'];rows=[]
for n,item in enumerate(json.loads((fixtures/'manifest.json').read_text())['fixtures']):
    name=item['name'];data=(fixtures/(name+'.bin')).read_bytes()
    program=(reference/(name+'.program')).read_bytes() if not item['status'] else b''
    for prefix,contents in [('input',data),('reference',program)]:
        header.append(f'static const unsigned char vertex_{prefix}_{n}[]={{'+','.join(map(str,contents or b'\0'))+'};')
    rows.append(f'{{"{name}",vertex_input_{n},{len(data)},{item["status"]},vertex_reference_{n},{len(program)}}}')
header.append('static const struct vertex_fixture vertex_fixtures[]={'+','.join(rows)+'};')
(base/'vertex-fixtures.h').write_text('\n'.join(header)+'\n')
print('Vertex API embedded fixtures',len(rows))
