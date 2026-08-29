#!/usr/bin/env python3
# Minimal OsitoFS v2 reader (pure stdlib) — list files / extract a file from an
# image (GPT-wrapped or raw). Finds the partition by scanning for the superblock
# magic. Usage:
#   osfs2_read.py <image> ls
#   osfs2_read.py <image> get <name> [out]
import sys, struct

from osfs2_journal import (JOURNAL_COMMIT_MAGIC, JOURNAL_COMMIT_OFF,
                           JournalError, lock, recover, super_valid)

MAGIC = 0x4F534632  # "OSF2"
LAYOUT_MAGIC = 0x4F324C59
FILETAB_OFF = 1 << 20
LEGACY_MAX_FILES = 4096
CRCTAB_SIZE = 262144 * 4
LAYERIDX_SIZE = 512 * 2048

def layout(data, p):
    layout_magic, file_slots, metadata_bytes = struct.unpack_from('<3I', data, p + 88)
    slots = LEGACY_MAX_FILES
    if layout_magic == LAYOUT_MAGIC:
        slots = file_slots
        if slots < LEGACY_MAX_FILES or slots % LEGACY_MAX_FILES:
            raise ValueError(f"invalid file slot count: {slots}")
    crctab_off = FILETAB_OFF + slots * 256
    layeridx_off = crctab_off + CRCTAB_SIZE
    data_off = layeridx_off + LAYERIDX_SIZE
    if layout_magic == LAYOUT_MAGIC and metadata_bytes != data_off:
        raise ValueError("metadata_bytes does not match layout")
    return slots

def find_part(image):
    # Scan 1MB-aligned offsets for the superblock magic (LE u32 at off+0).
    image.seek(0, 2)
    size = image.tell()
    step = 1 << 20
    for off in range(0, min(size, 256 << 20), step):
        image.seek(off)
        primary = image.read(512)
        image.seek(off + 4096)
        backup = image.read(512)
        image.seek(off + JOURNAL_COMMIT_OFF)
        commit = image.read(4)
        if super_valid(primary) or super_valid(backup) or \
                (len(commit) == 4 and
                 struct.unpack('<I', commit)[0] == JOURNAL_COMMIT_MAGIC):
            return off
    return None

def main():
    if len(sys.argv) < 3:
        print("usage: osfs2_read.py <image> <ls|get> [name] [output]")
        return 1
    img, cmd = sys.argv[1], sys.argv[2]
    with open(img, 'rb') as f:
        data = f.read()
    p = find_part(data)
    if p is None:
        print("OSFS2 superblock not found"); return
    magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', data, p)
    max_files = layout(data, p)
    print(f"part@0x{p:x} ver={ver} block_size={bsz} files={fcount}/{max_files}")
    ftab = p + FILETAB_OFF
    files = []
    for i in range(max_files):
        e = ftab + i*256
        if e+256 > len(data): break
        name = data[e:e+64].split(b'\x00')[0].decode('latin1', 'replace')
        size, = struct.unpack_from('<Q', data, e+64)
        start_block, block_count, crc, flags = struct.unpack_from('<4I', data, e+72)
        if not (flags & 1):  # FLAG_VALID
            continue
        files.append((name, size, start_block, block_count, flags))
    if cmd == 'ls':
        for name, size, sb, bc, fl in sorted(files):
            print(f"  {size:>10}  blk={sb:<6} cnt={bc:<3} fl=0x{fl:x}  {name}")
        print(f"({len(files)} files)")
    elif cmd == 'get':
        target = sys.argv[3]
        for name, size, sb, bc, fl in files:
            if name.lower() == target.lower():
                if fl & 8:  # FLAG_INLINE — data in model_name[128] @ entry+?
                    # inline data location: model_name field. Compute its offset.
                    # entry layout: name[64]+size8+sb4+bc4+crc4+flags4 (=84) + 7*u32 GGUF(28)=112 ; model_name@112
                    e = ftab + files.index((name,size,sb,bc,fl))  # not reliable; recompute below
                print(f"FOUND {name}: size={size} start_block={sb} block_count={bc} flags=0x{fl:x}")
                doff = p + sb * bsz
                blob = data[doff:doff+size]
                print(f"data@0x{doff:x} firstbytes={blob[:16].hex()}")
                out = sys.argv[4] if len(sys.argv) > 4 else None
                if out:
                    with open(out, 'wb') as o: o.write(blob)
                    print(f"wrote {len(blob)} bytes -> {out}")
                return
        print(f"not found: {target}")

            f.seek(p)
            superblock = f.read(512)
            magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', superblock)
            print(f"part@0x{p:x} ver={ver} block_size={bsz} files={fcount}")
            ftab = p + FILETAB_OFF
            f.seek(ftab)
            table = f.read(4096 * 256)

            files = []
            for i in range(4096):
                entry = table[i*256:(i+1)*256]
                name = entry[:64].split(b'\x00')[0].decode('latin1', 'replace')
                size, = struct.unpack_from('<Q', entry, 64)
                start_block, block_count, crc, flags = struct.unpack_from('<4I', entry, 72)
                if flags & 1:  # FLAG_VALID
                    files.append((name, size, start_block, block_count, flags))

            if cmd == 'ls':
                for name, size, sb, bc, fl in sorted(files):
                    print(f"  {size:>10}  blk={sb:<6} cnt={bc:<3} fl=0x{fl:x}  {name}")
                print(f"({len(files)} files)")
            elif cmd == 'get':
                target = sys.argv[3]
                for name, size, sb, bc, fl in files:
                    if name.lower() == target.lower():
                        print(f"FOUND {name}: size={size} start_block={sb} block_count={bc} flags=0x{fl:x}")
                        data_offset = p + sb * bsz
                        f.seek(data_offset)
                        blob = f.read(size)
                        print(f"data@0x{data_offset:x} firstbytes={blob[:16].hex()}")
                        out = sys.argv[4] if len(sys.argv) > 4 else None
                        if out:
                            with open(out, 'wb') as output:
                                output.write(blob)
                            print(f"wrote {len(blob)} bytes -> {out}")
                        return 0
                print(f"not found: {target}")
                return 1
            return 0
    except JournalError as error:
        print(f"OSFS2 journal error: {error}")
        return 1

sys.exit(main())
