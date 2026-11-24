"""Differential PE32/PE64 argument parsing against installed MSVCRT on Windows."""
import itertools
import json
from pathlib import Path
import random
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
out = root / "arch/x86/build/crt-startup"
random.seed(0xC47)
cases = [
    'program.exe', 'program.exe first second',
    '"C:\\Program Files\\tool.exe" "a b c" d e',
    '"C:\\path with spaces\\" a',
    'ab"cd ef"gh.exe a\\\\\\b d"e f"g h',
    'program.exe a\\\\\\"b c d',
    'program.exe a\\\\\\\\"b c" d e',
    'program.exe "" "unterminated', 'program.exe "a""b"',
    'program.exe "a b"\t"c d"  ', 'program.exe ñ á Ω 🙂',
    ' program.exe', '\tprogram.exe first',
]
for size in range(1, 5):
    for chars in itertools.product('a \\"', repeat=size):
        cases.append('program.exe ' + ''.join(chars))
for _ in range(200):
    cases.append('program.exe ' + ''.join(random.choices('ab \t\\"', k=40)))
results = []
for bits in (32, 64):
    binary = out / f"crt-args{bits}.exe"
    failures = []
    for command in cases:
        p = subprocess.run(command, executable=str(binary), capture_output=True,
                           timeout=5)
        if p.returncode:
            failures.append({"command": command, "code": p.returncode,
                             "stdout": p.stdout.decode(errors="replace")})
    result = {"bits": bits, "cases": len(cases), "failures": failures}
    results.append(result)
    print(json.dumps({"bits": bits, "cases": len(cases),
                      "failures": len(failures), "first": failures[:3]}))
(out / "windows-args.json").write_text(json.dumps(results, indent=2) + "\n")
sys.exit(any(r["failures"] for r in results))
