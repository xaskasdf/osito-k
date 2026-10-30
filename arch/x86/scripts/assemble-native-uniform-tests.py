#!/usr/bin/env python3
"""Author shader fixtures for native GPU float-constant binding tests."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'shader_assembler', Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembler)
assembler.SOURCES = {
    'uniform-c0': 'ps_2_0\nmov oC0, c0\n',
    'uniform-c223': 'ps_3_0\nmov oC0, c223\n',
    'uniform-sparse': 'ps_3_0\nmov r0, c5\nadd oC0, r0, c223\n',
    'uniform-local': 'ps_3_0\ndef c0, 0.5, 0.25, 1, 0\nmov r0, c0\nmul oC0, r0, c1\n',
    'uniform-repeat': 'ps_3_0\nadd r0, c7, c7\nadd oC0, r0, c7\n',
    'uniform-swizzle': 'ps_3_0\nmov oC0, c10.wzyx\n',
    'uniform-mask': 'ps_3_0\nmov r0.xy, c5.yxzw\nmov r0.zw, c2\nmov oC0, r0\n',
}
for count in (128, 129):
    assembler.SOURCES[f'uniform-{count}'] = (
        'ps_3_0\nmov r0, c0\n' +
        ''.join(f'add r0, r0, c{i}\n' for i in range(1, count)) +
        'mov oC0, r0\n')
assembler.HLSL = {}
if __name__ == '__main__':
    assembler.main()
