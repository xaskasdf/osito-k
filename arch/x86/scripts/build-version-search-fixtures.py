"""Build directory-search fixtures from the owned file-version test resources."""
from pathlib import Path
import argparse
import struct

parser = argparse.ArgumentParser()
parser.add_argument('fixtures', type=Path)
parser.add_argument('out', type=Path)
args = parser.parse_args()


def version_offset(data):
    nt = struct.unpack_from('<I', data, 0x3c)[0]
    count = struct.unpack_from('<H', data, nt + 6)[0]
    optional = struct.unpack_from('<H', data, nt + 20)[0]
    table = nt + 24 + optional

    def offset(rva):
        for i in range(count):
            start, size, raw = struct.unpack_from('<III', data, table + i * 40 + 12)
            if start <= rva < start + size:
                return raw + rva - start
        raise ValueError('resource outside fixture sections')

    directory = nt + 24 + (112 if optional == 240 else 96)
    root = offset(struct.unpack_from('<I', data, directory + 16)[0])
    node = root
    for level in range(3):
        named, numeric = struct.unpack_from('<HH', data, node + 12)
        for i in range(named + numeric):
            kind, child = struct.unpack_from('<II', data, node + 16 + i * 8)
            if level or kind == 16:
                node = root + (child & 0x7fffffff)
                break
        else:
            raise ValueError('version resource absent')
    return offset(struct.unpack_from('<I', data, node)[0])


def write(source, target, tag):
    data = bytearray(source.read_bytes())
    struct.pack_into('<I', data, version_offset(data) + 48, tag)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


for bits in (32, 64):
    base = args.out / f'pe{bits}'
    for directory, tag, names in [('', 1001, ['choice.dll', 'noext.dll', 'plain']),
                                  ('cwd', 2002, ['choice.dll', 'current.dll']),
                                  ('user', 3003, ['choice.dll', 'user.dll']),
                                  ('path', 4004, ['choice.dll', 'onpath.dll'])]:
        for name in names:
            write(args.fixtures / f'fixture{bits}.dll', base / directory / name, tag)
    for directory, name, tag in [('', 'localized.dll', 5005),
                                 ('cwd', 'localized.dll', 6006),
                                 ('path', 'onmui.dll', 7007)]:
        write(args.fixtures / 'localized64.dll', base / directory / name, 1001)
        write(args.fixtures / 'en-US/localized64.dll.mui',
              base / directory / 'en-US' / (name + '.mui'), tag)
