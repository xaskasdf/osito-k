"""Create tiny non-executed MZ files; file_unicode_pe.c verifies their markers."""
import hashlib
import json
from pathlib import Path
import sys

output = Path(sys.argv[1])
names = ['ascii.exe', 'caf\u00e9.exe', '\u65e5\u672c.exe', 'face-\U0001f642.exe',
         'ascii.exe\u0100.exe', 'e\u0301.exe', 'dir-\u65e5-\U0001f642/child.exe']
records = []
for marker, name in enumerate(names):
    path = output / 'file-unicode' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    data = bytes((ord('M'), ord('Z'), marker, 0xa5))
    path.write_bytes(data)
    records.append({'name': 'file-unicode/' + name, 'bytes': len(data),
                    'sha256': hashlib.sha256(data).hexdigest()})
(output / 'fixtures.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'files': len(records), 'output': str(output)}))
