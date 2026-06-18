#!/usr/bin/env python3
# OsitoFS v2 REPLACE — overwrite the data of an existing file by appending the
# new bytes at next_data_block and repointing the file-table entry to them
# (old extent leaks; mount-time bitmap rebuild reconciles). Counterpart to
# osfs2_write.py (which refuses to overwrite an existing name).
#   osfs2_replace.py <image> <hostfile> <destname>
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

def find_part(f):
    f.seek(0, os.SEEK_END); n = f.tell()
    off = 0
    while off < min(n, 256 << 20):
        f.seek(off); d = f.read(4)
        if len(d) == 4 and struct.unpack('<I', d)[0] == MAGIC:
            return off
        off += (1 << 20)
    return None

def main():
    img, host, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(host, 'rb').read()
    with open(img, 'r+b') as f:
        p = find_part(f)
        if p is None: print("no superblock"); return 1
        f.seek(p); s = bytearray(f.read(512))
        magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', s, 0)
        # locate the entry to replace
        slot = -1
        for i in range(MAX_FILES):
            e = p + FILETAB_OFF + i*256
            f.seek(e); ent = f.read(256)
            name = ent[:NAME_LEN].split(b'\x00')[0].decode('latin1','replace')
            flags = struct.unpack_from('<I', ent, 84)[0]
            if (flags & FLAG_VALID) and name.lower() == dest.lower():
                slot = i; break
        if slot < 0: print(f"not found: {dest}"); return 1
        bcount = (len(data) + bsz - 1) // bsz
        start = nextblk
        if start + bcount > total:
            print(f"NO ROOM: need {bcount} blk at {start}, total={total}"); return 1
        # write new data at the bump pointer
        f.seek(p + start*bsz); f.write(data)
        pad = bcount*bsz - len(data)
        if pad: f.write(b'\x00'*pad)
        # repoint the existing entry
        e = p + FILETAB_OFF + slot*256
        f.seek(e); ent = bytearray(f.read(256))
        struct.pack_into('<Q', ent, 64, len(data))
        struct.pack_into('<I', ent, 72, start)
        struct.pack_into('<I', ent, 76, bcount)
        struct.pack_into('<I', ent, 80, zlib.crc32(data) & 0xFFFFFFFF)
        struct.pack_into('<I', ent, 84, FLAG_VALID | FLAG_RAW)
        f.seek(e); f.write(ent)
        # crc_table slots for new blocks = 0 (skip verify)
        for b in range(start, start+bcount):
            f.seek(p + CRCTAB_OFF + b*4); f.write(b'\x00\x00\x00\x00')
        # superblock: used += bcount (old extent leaks), nextblk advance; fcount same
        struct.pack_into('<I', s, 16, used + bcount)
        struct.pack_into('<I', s, 24, start + bcount)
        struct.pack_into('<I', s, CRC_OFF_IN_SUPER, 0)
        crc = zlib.crc32(bytes(s)) & 0xFFFFFFFF
        struct.pack_into('<I', s, CRC_OFF_IN_SUPER, crc)
        f.seek(p + 0); f.write(s)
        f.seek(p + SUPER_BACKUP_OFF); f.write(s)
        print(f"REPLACED {dest} (slot {slot}): {len(data)} B at block {start} "
              f"(+{bcount} blk); next_data_block -> {start+bcount}/{total}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
