#!/usr/bin/env python3
# Grow a raw OsitoFS v2 image: extend the backing file and bump total_blocks in
# the superblock (+ backup) so more 1 MB blocks are available. New blocks read as
# zero → free in the mount-time bitmap rebuild. OsitoFS supports up to 262144
# blocks (bitmap), so growing well under that is safe.
#   osfs2_grow_image.py <image> <new_total_blocks>
import sys, struct, zlib, os

MAGIC = 0x4F534632
SUPER_BACKUP_OFF = 4096
CRC_OFF_IN_SUPER = 84

img, new_total = sys.argv[1], int(sys.argv[2])
with open(img, 'r+b') as f:
    f.seek(0); s = bytearray(f.read(512))
    magic, ver, bsz, total, used, fc, nb = struct.unpack_from('<7I', s, 0)
    assert magic == MAGIC, "not an OsitoFS image (raw, magic@0)"
    print(f"current: total={total} block_size={bsz} used={used} nextblk={nb}")
    assert new_total > total, "new_total must be larger"
    new_size = new_total * bsz
    # extend the backing file (sparse) to the new capacity
    f.truncate(new_size)
    # update total_blocks (offset 12) and fix CRC
    struct.pack_into('<I', s, 12, new_total)
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, 0)
    crc = zlib.crc32(bytes(s)) & 0xFFFFFFFF
    struct.pack_into('<I', s, CRC_OFF_IN_SUPER, crc)
    f.seek(0); f.write(s)
    f.seek(SUPER_BACKUP_OFF); f.write(s)
    print(f"grown: total={new_total} ({new_total*bsz//(1<<20)} MB), file size now {new_size//(1<<20)} MB")
