#!/usr/bin/env python3
"""Generate host-only Intel XML packing and isolated ISL layout references.

The two ISL function bodies are copied verbatim, with their license, from the
pinned source. Tests supply only linear BGRA8, 2D, one layer, HALIGN/VALIGN 4.
This is an algorithm/encoding reference, not a simulated GPU.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
intel = root / 'arch/x86/lib/intel_d3d9/mesa-intel/src/intel'
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
with (out / 'gen9-texture-pack.h').open('w') as f:
    subprocess.run([sys.executable, str(intel / 'genxml/gen_pack_header.py'),
                    '--engines', 'render', '--include-symbols',
                    'RENDER_SURFACE_STATE,SAMPLER_STATE,SAMPLER_BORDER_COLOR_STATE,3DSTATE_SBE,3DSTATE_SBE_SWIZ,SF_OUTPUT_ATTRIBUTE_DETAIL,3DSTATE_URB_VS,3DSTATE_VERTEX_ELEMENTS,VERTEX_ELEMENT_STATE',
                    str(intel / 'genxml/gen90.xml')], stdout=f, check=True)
source_path = intel / 'isl/isl.c'
source = source_path.read_text()
# Ignore braces in comments while retaining source offsets.
scan = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: ' ' * len(m[0]), source, flags=re.S)
symbols = ['isl_calc_phys_slice0_extent_sa_gfx4_2d', 'get_image_offset_sa_gfx4_2d']
pieces = [source[:source.index('*/') + 2]]
for name in symbols:
    start = source.index('static void\n' + name + '(')
    brace = scan.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (scan[end] == '{') - (scan[end] == '}')
        end += 1
    pieces.append(source[start:end])
(out / 'isl-texture-reference.inc').write_text('\n\n'.join(pieces) + '\n')
(out / 'reference.json').write_text(json.dumps({
    'isl_source': str(source_path.relative_to(root)),
    'isl_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
    'symbols': symbols, 'scope': 'linear BGRA8, 2D, one layer, HALIGN/VALIGN 4',
    'gpu_execution': False,
}, indent=2) + '\n')
