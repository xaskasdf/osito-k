#!/usr/bin/env python3
# Grow a raw OsitoFS v2 image: extend the backing file and bump total_blocks in
# the superblock (+ backup) so more 1 MB blocks are available. New blocks read as
# zero → free in the mount-time bitmap rebuild. OsitoFS supports up to 262144
# blocks (bitmap), so growing well under that is safe.
#   osfs2_grow_image.py <image> <new_total_blocks>
import sys, struct, zlib, os

from osfs2_journal import (JOURNAL_OP_REPLACE, commit_entries, fix_super_crc,
                           lock, read_super, recover)

MAGIC = 0x4F534632
SUPER_BACKUP_OFF = 4096
CRC_OFF_IN_SUPER = 84
FILETAB_OFF = 1 << 20

img, new_total = sys.argv[1], int(sys.argv[2])
with open(img, 'r+b') as f:
    lock(f, True)
    recover(f, 0, True)
    s = read_super(f, 0, True)
    magic, ver, bsz, total, used, fc, nb = struct.unpack_from('<7I', s, 0)
    if magic != MAGIC:
        raise SystemExit('not an OsitoFS image (raw, magic@0)')
    print(f"current: total={total} block_size={bsz} used={used} nextblk={nb}")
    if new_total <= total:
        raise SystemExit('new_total must be larger')
    if new_total > 262144:
        raise SystemExit('new_total exceeds OsitoFS v2 CRC table')
    new_size = new_total * bsz
    # extend the backing file (sparse) to the new capacity
    f.seek(0, os.SEEK_END)
    if new_size > f.tell():
        f.truncate(new_size)
    f.flush(); os.fsync(f.fileno())
    before_super = bytearray(s)
    struct.pack_into('<I', s, 12, new_total)
    fix_super_crc(s)
    f.seek(FILETAB_OFF); unchanged_entry = f.read(256)
    commit_entries(f, 0, JOURNAL_OP_REPLACE, before_super, s, [0],
                   [unchanged_entry], [unchanged_entry])
    print(f"grown: total={new_total} ({new_total*bsz//(1<<20)} MB), file size now {new_size//(1<<20)} MB")
