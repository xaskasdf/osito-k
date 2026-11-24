#!/usr/bin/env python3
"""Generate version-matched Intel compiler metadata; requires Mako."""
from pathlib import Path
import hashlib, json, re, subprocess, sys

src = Path('arch/x86/lib/intel_d3d9/mesa-intel/src/intel')
mesa = Path('arch/x86/lib/opengl/mesa')
out = Path(sys.argv[1] if len(sys.argv) > 1 else 'arch/x86/build/native-intel-compiler-host')
root = src.parent.parent
assert (root / 'VERSION').read_text().strip() == (mesa / 'VERSION').read_text().strip() == '25.0.0'
for relative, expected in json.loads((root / 'source.json').read_text())['files'].items():
    assert hashlib.sha256((root / relative).read_bytes()).hexdigest() == expected, relative
gen = out / 'generated/intel'
for child in ('compiler', 'dev', 'isl'):
    (gen / child).mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(src / 'dev/intel_device_info_gen_h.py'), str(gen / 'dev/intel_device_info_gen.h')], check=True)
subprocess.run([sys.executable, str(src / 'dev/gen_wa_helpers.py'), str(src / 'dev/mesa_defs.json'), str(gen / 'dev/intel_wa.h'), str(gen / 'dev/intel_wa.c')], check=True)
subprocess.run([sys.executable, str(src / 'compiler/brw_device_sha1_gen_c.py'), '--outdir', str(gen / 'compiler')], check=True)
for script, target in [('brw_nir_lower_fsign.py', 'brw_nir_lower_fsign.c'), ('brw_nir_trig_workarounds.py', 'brw_nir_trig_workarounds.c')]:
    with (gen / 'compiler' / target).open('wb') as stream:
        subprocess.run([sys.executable, str(src / 'compiler' / script), '-p', str(mesa / 'src/compiler/nir')], stdout=stream, check=True)
subprocess.run([sys.executable, str(src / 'isl/gen_format_layout.py'), '--csv', str(src / 'isl/isl_format_layout.csv'), '--out', str(gen / 'isl/isl_format_layout.c')], check=True)
meson = (src / 'compiler/meson.build').read_text()
files = []
for group in ('intel_nir_files', 'libintel_compiler_brw_files'):
    block = re.search(group + r' = files\((.*?)\n\)', meson, re.S)[1]
    files += [name for name in re.findall(r"'([^']+)'", block) if name.endswith(('.c', '.cpp'))]
objects = ['$(OUT)/intel/compiler/' + Path(p).with_suffix('.o').as_posix() for p in files]
objects += ['$(OUT)/intel/dev/intel_device_info.o', '$(OUT)/intel/dev/intel_debug.o', '$(OUT)/intel/isl/isl_format.o']
objects += ['$(OUT)/generated/intel/' + p for p in ['dev/intel_wa.o', 'compiler/brw_device_sha1_gen.o', 'compiler/brw_nir_lower_fsign.o', 'compiler/brw_nir_trig_workarounds.o', 'isl/isl_format_layout.o']]
(out / 'intel-sources.mk').write_text('INTEL_OBJ := ' + ' '.join(objects) + '\n')
print('Intel source compilation units', len(objects))
