#!/usr/bin/env python3
"""Compare INF navigation/text probe output from native Windows and Osito-K."""
import argparse
import difflib
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--native', type=Path, required=True)
parser.add_argument('--guest', type=Path, required=True)
args = parser.parse_args()
pattern = rb'INFQUERY (?:FIND|TEXT|NEXT|INVALID NEXT) [^\r\n]*'
expected_log, actual_log = args.native.read_bytes(), args.guest.read_bytes()
expected, actual = re.findall(pattern, expected_log), re.findall(pattern, actual_log)
# Directory spelling follows each system's Windows directory. Only this row
# compares paths without case; ordinary quoted text remains case-sensitive.
expected = [row.lower() if row.endswith(b'%99999%') else row for row in expected]
actual = [row.lower() if row.endswith(b'%99999%') else row for row in actual]
complete = all(b'INFQUERY DONE' in data and b'INFQUERY FAIL' not in data
               for data in (expected_log, actual_log))
if not complete or not expected or expected != actual:
    print(''.join(difflib.unified_diff([row.decode() + '\n' for row in expected],
                                     [row.decode() + '\n' for row in actual],
                                     fromfile='native', tofile='guest')), end='')
    raise SystemExit('INF query comparison failed or incomplete')
print(f'INF QUERY PASS {len(expected)} exact native/guest rows')
