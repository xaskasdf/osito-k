#!/usr/bin/env python3
# Batch OsitoFS v2 writer — inject MANY files in one pass (one image open, one
# superblock update). Reads a manifest of "hostpath\tdestkey" lines. Skips dupes
# and names >=64 chars. Far faster than per-file osfs2_write.py for big closures.
#   osfs2_write_batch.py <image> <manifest>
import sys, struct, zlib, os

MAGIC = 0x4F534632
SUPER_BACKUP_OFF = 4096
FILETAB_OFF = 1 << 20
CRCTAB_OFF  = 2 << 20
MAX_FILES   = 4096
NAME_LEN    = 64
FLAG_VALID  = 1
FLAG_RAW    = 4
CRC_OFF_IN_SUPER = 84

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

    # read the whole file table once; map existing names -> slot, collect free slots
    f.seek(p + FILETAB_OFF); ftab = bytearray(f.read(MAX_FILES * 256))
    existing = set(); free = []
    for i in range(MAX_FILES):
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
            f.seek(p + CRCTAB_OFF + b * 4); f.write(b'\x00\x00\x00\x00')
        nextblk += bcount; used += bcount; fcount += 1
        existing.add(key.lower())
        added += 1

    struct.pack_into('<I', s, 16, used)
    struct.pack_into('<I', s, 20, fcount)
    struct.pack_into('<I', s, 24, nextblk)
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, 0)
    crc = zlib.crc32(bytes(s)) & 0xFFFFFFFF
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, crc)
    f.seek(p); f.write(s)
    f.seek(p + SUPER_BACKUP_OFF); f.write(s)
    print(f"added={added} dup={dup} skipped={skipped} nextblk={nextblk}/{total}")
