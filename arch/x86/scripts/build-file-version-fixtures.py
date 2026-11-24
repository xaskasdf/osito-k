"""Build owned PE/MUI fixtures on Windows with LLVM in WSL and SDK MUIRCT.

Run from the repository root after build-file-version-test.sh. The resource
compiler and MUI tool are build dependencies; their binaries are not copied.
"""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('out', type=Path)
parser.add_argument('--distro', default='osito')
parser.add_argument('--sdk-bin', type=Path,
                    default=Path('C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64'))
args = parser.parse_args()
base = args.out.resolve()
base.mkdir(parents=True, exist_ok=True)
tool = args.sdk_bin / 'muirct.exe'
assert tool.is_file()

def linux(path):
    return subprocess.check_output(['wsl.exe', '-d', args.distro, '--', 'wslpath', '-u', path.resolve().as_posix()], text=True).strip()

def llvm(*command):
    subprocess.run(['wsl.exe', '-d', args.distro, '--', *command], check=True)

resource = base / 'fixture.res'
llvm('llvm-rc', '/fo', linux(resource), linux(Path('arch/x86/test/file_version_fixture.rc')))
for bits, machine in [(32, 'x86'), (64, 'x64')]:
    llvm('lld-link', '/dll', '/noentry', '/nodefaultlib', '/machine:' + machine,
         '/timestamp:0', '/out:' + linux(base / f'fixture{bits}.dll'), linux(resource))

config = str(Path('arch/x86/test/file_version_fixture.rcconfig').resolve())
for output, language, lang_id in [('localized64', 'en-US', '0x409'), ('fallback64', 'fr-FR', '0x40c')]:
    (base / language).mkdir(exist_ok=True)
    subprocess.run([str(tool), '-q', config, '-f', '-g', lang_id, '-x', lang_id,
                    str(base / 'fixture64.dll'), str(base / (output + '.dll')),
                    str(base / language / (output + '.dll.mui'))], check=True)
shutil.copyfile(base / 'localized64.dll', base / 'bad64.dll')
shutil.copyfile(base / 'en-US/localized64.dll.mui', base / 'en-US/bad64.dll.mui')

def resources(data):
    nt = struct.unpack_from('<I', data, 0x3c)[0]
    sections, optional = struct.unpack_from('<H', data, nt + 6)[0], struct.unpack_from('<H', data, nt + 20)[0]
    table = nt + 24 + optional
    def offset(rva):
        for i in range(sections):
            pos = table + i * 40
            start, size, raw = struct.unpack_from('<III', data, pos + 12)
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError('fixture resource RVA outside file sections')
    directory = nt + 24 + (112 if optional == 240 else 96)
    root = offset(struct.unpack_from('<I', data, directory + 16)[0])
    def entries(relative):
        pos = root + relative
        count = sum(struct.unpack_from('<HH', data, pos + 12))
        return [struct.unpack_from('<II', data, pos + 16 + i * 8) for i in range(count)]
    for kind, names in entries(0):
        for _, languages in entries(names & 0x7fffffff):
            for _, leaf in entries(languages & 0x7fffffff):
                rva, size = struct.unpack_from('<II', data, root + leaf)
                yield kind, offset(rva), size, root + leaf

for name in ('en-US/localized64.dll.mui', 'en-US/bad64.dll.mui', 'fr-FR/fallback64.dll.mui'):
    path = base / name
    data = bytearray(path.read_bytes())
    for kind, start, size, _ in resources(data):
        if kind == 16:
            struct.pack_into('<I', data, start + 48, 0x00250029)
            old = 'Osito-K test fixture'.encode('utf-16-le')
            new = 'Osito-K MUI resource'.encode('utf-16-le')
            found = data.find(old, start, start + size)
            assert found >= 0 and len(old) == len(new)
            data[found:found + len(old)] = new
        if kind & 0x80000000 and name == 'en-US/bad64.dll.mui':
            data[start + 44] ^= 0x5a
    path.write_bytes(data)

data = bytearray((base / 'fixture64.dll').read_bytes())
for kind, start, size, entry in resources(data):
    if kind == 16:
        assert start + size + 2 <= len(data)
        struct.pack_into('<I', data, entry + 4, size + 2)
(base / 'padded64.dll').write_bytes(data)

files = ['fixture32.dll', 'fixture64.dll', 'localized64.dll', 'bad64.dll', 'fallback64.dll', 'padded64.dll',
         'en-US/localized64.dll.mui', 'en-US/bad64.dll.mui', 'fr-FR/fallback64.dll.mui']
for bits in (32, 64):
    for name in files:
        target = base / f'pe{bits}' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(base / name, target)
