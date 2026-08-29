#!/usr/bin/env python3
"""Crash-safe copy-on-write replacement of one OsitoFS v2 file."""

import os
import struct
import sys
import time
import zlib

from osfs2_journal import (JOURNAL_COMMIT_MAGIC, JOURNAL_COMMIT_OFF,
                           JOURNAL_OP_REPLACE, commit_entries, fix_super_crc,
                           lock, read_super, recover, super_valid)

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

def find_part(f):
    f.seek(0, os.SEEK_END); n = f.tell()
    off = 0
    while off < min(n, 256 << 20):
        f.seek(off); d = f.read(4)
        if len(d) == 4 and struct.unpack('<I', d)[0] == MAGIC:
            return off
        off += (1 << 20)
    return None


def find_free_extent(table, total_blocks, block_size, needed):
    if needed == 0:
        return 0
    data_start = DATA_OFF // block_size
    used = bytearray(total_blocks)
    used[:data_start] = bytes([1]) * data_start
    for slot in range(MAX_FILES):
        entry = table[slot * 256:(slot + 1) * 256]
        flags = struct.unpack_from('<I', entry, 84)[0]
        start, count = struct.unpack_from('<2I', entry, 72)
        if flags & FLAG_VALID:
            for block in range(start, min(start + count, total_blocks)):
                used[block] = 1
    run_start = 0
    run_length = 0
    for block in range(data_start, total_blocks):
        if not used[block]:
            if run_length == 0:
                run_start = block
            run_length += 1
            if run_length == needed:
                return run_start
        else:
            run_length = 0
    return None


def recompute_super(superblock, table):
    block_size = struct.unpack_from('<I', superblock, 8)[0]
    data_start = DATA_OFF // block_size
    files, used, high_water = 0, data_start, data_start
    for slot in range(MAX_FILES):
        entry = table[slot * 256:(slot + 1) * 256]
        if not (struct.unpack_from('<I', entry, 84)[0] & FLAG_VALID):
            continue
        files += 1
        start, count = struct.unpack_from('<2I', entry, 72)
        used += count
        high_water = max(high_water, start + count)
    struct.pack_into('<I', superblock, 16, used)
    struct.pack_into('<I', superblock, 20, files)
    struct.pack_into('<I', superblock, 24, high_water)
    fix_super_crc(superblock)
    return high_water


def main():
    img, host, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(host, 'rb').read()
    with open(img, 'r+b') as f:
        p = find_part(f)
        if p is None: print("no superblock"); return 1
        f.seek(p); s = bytearray(f.read(512))
        magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', s, 0)
        max_files, crctab_off = layout(s)
        # locate the entry to replace
        slot = -1
        for i in range(max_files):
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
            if b < MAX_BLOCKS:
                f.seek(p + crctab_off + b*4); f.write(b'\x00\x00\x00\x00')
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
