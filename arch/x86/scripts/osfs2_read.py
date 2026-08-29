#!/usr/bin/env python3
# Minimal OsitoFS v2 reader (pure stdlib) — list files / extract a file from an
# image (GPT-wrapped or raw). Finds the partition by scanning for the superblock
# magic. Usage:
#   osfs2_read.py <image> ls
#   osfs2_read.py <image> get <name> [out]
import sys, struct

from osfs2_journal import (FILETAB_OFF, JOURNAL_COMMIT_MAGIC,
                           JOURNAL_COMMIT_OFF, JournalError,
                           layout_from_super, lock, recover, super_valid)

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
    try:
        with open(img, 'rb') as f:
            lock(f, False)
            p = find_part(f)
            if p is None:
                print("OSFS2 superblock not found")
                return 1
            if recover(f, p, False):
                print("OSFS2 committed journal pending; recovery required")
                return 1

            f.seek(p)
            superblock = f.read(512)
            layout = layout_from_super(superblock)
            magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', superblock)
            print(f"part@0x{p:x} ver={ver} block_size={bsz} files={fcount}")
            ftab = p + FILETAB_OFF
            f.seek(ftab)
            table = f.read(layout['filetab_size'])
            if len(table) != layout['filetab_size']:
                print('short file table')
                return 1

            files = []
            for i in range(layout['max_files']):
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
