#!/usr/bin/env python3
"""Add a manifest of files through independent OsitoFS v2 transactions."""

MAGIC = 0x4F534632
LAYOUT_MAGIC = 0x4F324C59
SUPER_BACKUP_OFF = 4096
FILETAB_OFF = 1 << 20
LEGACY_MAX_FILES = 4096
MAX_BLOCKS  = 262144
CRCTAB_SIZE = MAX_BLOCKS * 4
LAYERIDX_SIZE = 512 * 2048
NAME_LEN    = 64
FLAG_VALID  = 1
FLAG_RAW    = 4
CRC_OFF_IN_SUPER = 84

def layout(s):
    layout_magic, file_slots, metadata_bytes = struct.unpack_from('<3I', s, 88)
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
    return slots, crctab_off

img, manifest = sys.argv[1], sys.argv[2]
entries = []
with open(manifest) as mf:
    for line in mf:
        line = line.rstrip("\n")
        if not line or "\t" not in line:
            continue
        host, key = line.split("\t", 1)
        entries.append((host, key))

with open(img, 'r+b') as f:
    f.seek(0, os.SEEK_END); fsize = f.tell()
    f.seek(0); s = bytearray(f.read(512))
    magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', s, 0)
    assert magic == MAGIC
    p = 0  # raw image
    max_files, crctab_off = layout(s)

    # read the whole file table once; map existing names -> slot, collect free slots
    f.seek(p + FILETAB_OFF); ftab = bytearray(f.read(max_files * 256))
    existing = set(); free = []
    for i in range(max_files):
        e = i * 256
        flags = struct.unpack_from('<I', ftab, e + 84)[0]
        if flags & FLAG_VALID:
            nm = ftab[e:e+NAME_LEN].split(b'\x00')[0].decode('latin1', 'replace')
            existing.add(nm.lower())
        else:
            free.append(i)
    free.reverse()  # pop() gives ascending slots

    added = skipped = dup = 0
    for host, key in entries:
        if not os.path.isfile(host): skipped += 1; continue
        if len(key.encode()) >= NAME_LEN: skipped += 1; continue
        if key.lower() in existing: dup += 1; continue
        if not free: print("OUT OF FILE SLOTS"); break
        data = open(host, 'rb').read()
        bcount = (len(data) + bsz - 1) // bsz
        if nextblk + bcount > total:
            print(f"OUT OF SPACE at {key} (need {bcount}, free {total-nextblk})"); break
        start = nextblk
        f.seek(p + start * bsz); f.write(data)
        pad = bcount * bsz - len(data)
        if pad: f.write(b'\x00' * pad)
        slot = free.pop()
        ent = bytearray(256)
        nm = key.encode('latin1'); ent[:len(nm)] = nm
        struct.pack_into('<Q', ent, 64, len(data))
        struct.pack_into('<I', ent, 72, start)
        struct.pack_into('<I', ent, 76, bcount)
        struct.pack_into('<I', ent, 80, zlib.crc32(data) & 0xFFFFFFFF)
        struct.pack_into('<I', ent, 84, FLAG_VALID | FLAG_RAW)
        struct.pack_into('<H', ent, 244, 0xFFFF)
        f.seek(p + FILETAB_OFF + slot * 256); f.write(ent)
        for b in range(start, start + bcount):
            if b < MAX_BLOCKS:
                f.seek(p + crctab_off + b * 4); f.write(b'\x00\x00\x00\x00')
        nextblk += bcount; used += bcount; fcount += 1
        existing.add(key.lower())
        added += 1

        existing = set()
        free_slots = []
        for slot in range(MAX_FILES):
            entry = table[slot * 256:(slot + 1) * 256]
            flags = struct.unpack_from('<I', entry, 84)[0]
            if flags & FLAG_VALID:
                existing.add(entry[:NAME_LEN].split(b'\0')[0]
                             .decode('latin1', 'replace').lower())
            else:
                free_slots.append(slot)

        added = duplicate = skipped = 0
        incomplete = False
        for host_path, destination in entries:
            try:
                encoded_destination = destination.encode('latin1')
            except UnicodeEncodeError:
                skipped += 1
                continue
            if not os.path.isfile(host_path) or not encoded_destination or \
                    len(encoded_destination) >= NAME_LEN:
                skipped += 1
                continue
            if destination.lower() in existing:
                duplicate += 1
                continue
            if not free_slots:
                print('OUT OF FILE SLOTS')
                incomplete = True
                break

            data = open(host_path, 'rb').read()
            block_count = (len(data) + block_size - 1) // block_size
            start = find_free_extent(table, total_blocks, block_size, block_count)
            if start is None:
                print(f'OUT OF SPACE at {destination} (need {block_count})')
                incomplete = True
                break
            if block_count:
                image.seek(partition + start * block_size)
                image.write(data)
                image.write(bytes(block_count * block_size - len(data)))
                for block in range(start, start + block_count):
                    image.seek(partition + CRCTAB_OFF + block * 4)
                    image.write(bytes(4))
            image.flush()
            os.fsync(image.fileno())

            slot = free_slots.pop(0)
            old_entry = bytes(table[slot * 256:(slot + 1) * 256])
            new_entry = bytearray(256)
            new_entry[:len(encoded_destination)] = encoded_destination
            struct.pack_into('<Q', new_entry, 64, len(data))
            struct.pack_into('<II', new_entry, 72, start, block_count)
            struct.pack_into('<I', new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
            struct.pack_into('<I', new_entry, 84, FLAG_VALID | FLAG_RAW)
            struct.pack_into('<H', new_entry, 244, 0xFFFF)
            now = int(time.time())
            struct.pack_into('<II', new_entry, 246, now, now)

            before_super = bytearray(superblock)
            table[slot * 256:(slot + 1) * 256] = new_entry
            after_super = bytearray(superblock)
            recompute_super(after_super, table)
            commit_entries(image, partition, JOURNAL_OP_REPLACE,
                           before_super, after_super, [slot], [old_entry],
                           [new_entry])
            superblock = after_super
            existing.add(destination.lower())
            added += 1

        next_block = struct.unpack_from('<I', superblock, 24)[0]
        print(f'added={added} dup={duplicate} skipped={skipped} '
              f'nextblk={next_block}/{total_blocks}')
    return 1 if incomplete or skipped else 0


if __name__ == '__main__':
    sys.exit(main())
