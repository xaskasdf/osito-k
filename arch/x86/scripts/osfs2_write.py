#!/usr/bin/env python3
# Minimal OsitoFS v2 WRITER (pure stdlib) — inject a host file into an image.
# Appends at next_data_block (bump), adds a file-table entry, fixes superblock CRC
# (primary+backup), leaves the new block's CRC-table slot 0 (read path skips verify
# when slot==0). Mount-time blk_bitmap_rebuild reconciles the allocator bitmap.
#   osfs2_write.py <image> add <hostfile> [destname]
import sys, struct, zlib, os

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
CRC_OFF_IN_SUPER = 84   # offset of crc32 field within the 512B superblock

def find_part(f):
    f.seek(0, os.SEEK_END); n = f.tell()
    step = 1 << 20
    off = 0
    while off < min(n, 256 << 20):
        f.seek(off); d = f.read(4)
        if len(d) == 4 and struct.unpack('<I', d)[0] == MAGIC:
            return off
        off += step
    return None

def read_super(f, p):
    f.seek(p); s = bytearray(f.read(512))
    magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', s, 0)
    layout_magic, file_slots, metadata_bytes = struct.unpack_from('<3I', s, 88)
    return s, dict(magic=magic, ver=ver, bsz=bsz, total=total, used=used,
                   fcount=fcount, nextblk=nextblk, layout_magic=layout_magic,
                   file_slots=file_slots, metadata_bytes=metadata_bytes)

def layout(sb):
    slots = LEGACY_MAX_FILES
    if sb['layout_magic'] == LAYOUT_MAGIC:
        slots = sb['file_slots']
        if slots < LEGACY_MAX_FILES or slots % LEGACY_MAX_FILES:
            raise ValueError(f"invalid file slot count: {slots}")
    crctab_off = FILETAB_OFF + slots * 256
    layeridx_off = crctab_off + CRCTAB_SIZE
    data_off = layeridx_off + LAYERIDX_SIZE
    if sb['layout_magic'] == LAYOUT_MAGIC and sb['metadata_bytes'] != data_off:
        raise ValueError("metadata_bytes does not match layout")
    return slots, crctab_off

def fix_super_crc(s):
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, 0)
    crc = zlib.crc32(bytes(s)) & 0xFFFFFFFF
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, crc)
    return crc

def main():
    img, cmd, host = sys.argv[1], sys.argv[2], sys.argv[3]
    dest = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(host)
    assert cmd == 'add'
    data = open(host, 'rb').read()
    if len(dest.encode()) >= NAME_LEN:
        print("dest name too long"); return 1
    with open(img, 'r+b') as f:
        p = find_part(f)
        if p is None: print("no superblock"); return 1
        s, sb = read_super(f, p)
        bsz = sb['bsz']
        max_files, crctab_off = layout(sb)
        # check duplicate + find free entry slot
        free_slot = -1
        for i in range(max_files):
            e = p + FILETAB_OFF + i*256
            f.seek(e); ent = f.read(256)
            name = ent[:NAME_LEN].split(b'\x00')[0].decode('latin1','replace')
            flags = struct.unpack_from('<I', ent, 84)[0]
            if (flags & FLAG_VALID) and name.lower() == dest.lower():
                print(f"already present: {name} (slot {i}) — skipping"); return 0
            if free_slot < 0 and not (flags & FLAG_VALID):
                free_slot = i
        if free_slot < 0: print("no free file slot"); return 1
        bcount = (len(data) + bsz - 1) // bsz
        start = sb['nextblk']
        if start + bcount > sb['total']:
            print(f"NO ROOM: need {bcount} blk at {start}, total={sb['total']} "
                  f"(free={sb['total']-start})"); return 1
        # 1) write data
        f.seek(p + start*bsz); f.write(data)
        # zero-pad the tail of the last block so stale bytes don't linger
        pad = bcount*bsz - len(data)
        if pad: f.write(b'\x00'*pad)
        # 2) file-table entry
        ent = bytearray(256)
        nm = dest.encode('latin1'); ent[:len(nm)] = nm
        struct.pack_into('<Q', ent, 64, len(data))      # size
        struct.pack_into('<I', ent, 72, start)          # start_block
        struct.pack_into('<I', ent, 76, bcount)         # block_count
        struct.pack_into('<I', ent, 80, zlib.crc32(data) & 0xFFFFFFFF)  # file crc32
        struct.pack_into('<I', ent, 84, FLAG_VALID | FLAG_RAW)          # flags=0x5
        struct.pack_into('<H', ent, 244, 0xFFFF)        # layer_index_slot = none
        f.seek(p + FILETAB_OFF + free_slot*256); f.write(ent)
        # 3) leave crc_table[start..start+bcount] = 0 (skip verify). Force-zero them.
        for b in range(start, start+bcount):
            if b < MAX_BLOCKS:
                f.seek(p + crctab_off + b*4); f.write(b'\x00\x00\x00\x00')
        # 4) superblock counters + CRC (primary + backup)
        struct.pack_into('<I', s, 16, sb['used']  + bcount)  # used_blocks
        struct.pack_into('<I', s, 20, sb['fcount'] + 1)      # file_count
        struct.pack_into('<I', s, 24, start + bcount)        # next_data_block
        fix_super_crc(s)
        f.seek(p + 0);                 f.write(s)
        f.seek(p + SUPER_BACKUP_OFF);  f.write(s)
        print(f"ADDED {dest}: {len(data)} B at block {start} (+{bcount} blk), "
              f"slot {free_slot}; next_data_block -> {start+bcount}/{sb['total']}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
