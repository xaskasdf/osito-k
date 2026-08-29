#!/usr/bin/env python3
"""Add one file to OsitoFS v2 using a recoverable metadata transaction."""

import os
import struct
import sys
import time
import zlib

from osfs2_journal import (JOURNAL_OP_REPLACE, commit_entries, lock, read_super,
                           recover)
from osfs2_replace import (CRCTAB_OFF, FILETAB_OFF, FLAG_RAW, FLAG_VALID,
                           MAX_FILES, NAME_LEN, find_free_extent, find_part,
                           recompute_super)


def main():
    if len(sys.argv) < 4 or sys.argv[2] != 'add':
        print('usage: osfs2_write.py <image> add <hostfile> [destname]')
        return 1
    image_path, host_path = sys.argv[1], sys.argv[3]
    destination = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(host_path)
    try:
        encoded_destination = destination.encode('latin1')
    except UnicodeEncodeError:
        print('destination must be Latin-1')
        return 1
    if not encoded_destination or len(encoded_destination) >= NAME_LEN:
        print('destination name must contain 1-63 bytes')
        return 1
    data = open(host_path, 'rb').read()

    with open(image_path, 'r+b') as image:
        lock(image, True)
        partition = find_part(image)
        if partition is None:
            print('no superblock')
            return 1
        recover(image, partition, True)
        before_super = read_super(image, partition, True)
        _, _, block_size, total_blocks = struct.unpack_from('<4I', before_super)
        image.seek(partition + FILETAB_OFF)
        table = bytearray(image.read(MAX_FILES * 256))

        free_slot = -1
        for slot in range(MAX_FILES):
            entry = table[slot * 256:(slot + 1) * 256]
            flags = struct.unpack_from('<I', entry, 84)[0]
            name = entry[:NAME_LEN].split(b'\0')[0].decode('latin1', 'replace')
            if flags & FLAG_VALID and name.lower() == destination.lower():
                print(f'already present: {name} (slot {slot}) - skipping')
                return 0
            if free_slot < 0 and not flags & FLAG_VALID:
                free_slot = slot
        if free_slot < 0:
            print('no free file slot')
            return 1

        block_count = (len(data) + block_size - 1) // block_size
        start = find_free_extent(table, total_blocks, block_size, block_count)
        if start is None:
            print(f'NO ROOM: need {block_count} contiguous blocks')
            return 1
        if block_count:
            image.seek(partition + start * block_size)
            image.write(data)
            image.write(bytes(block_count * block_size - len(data)))
            for block in range(start, start + block_count):
                image.seek(partition + CRCTAB_OFF + block * 4)
                image.write(bytes(4))
        image.flush()
        os.fsync(image.fileno())

        old_entry = bytes(table[free_slot * 256:(free_slot + 1) * 256])
        new_entry = bytearray(256)
        new_entry[:len(encoded_destination)] = encoded_destination
        struct.pack_into('<Q', new_entry, 64, len(data))
        struct.pack_into('<I', new_entry, 72, start)
        struct.pack_into('<I', new_entry, 76, block_count)
        struct.pack_into('<I', new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
        struct.pack_into('<I', new_entry, 84, FLAG_VALID | FLAG_RAW)
        struct.pack_into('<H', new_entry, 244, 0xFFFF)
        now = int(time.time())
        struct.pack_into('<II', new_entry, 246, now, now)

        table[free_slot * 256:(free_slot + 1) * 256] = new_entry
        after_super = bytearray(before_super)
        high_water = recompute_super(after_super, table)
        commit_entries(image, partition, JOURNAL_OP_REPLACE,
                       before_super, after_super, [free_slot], [old_entry],
                       [new_entry])
        print(f'ADDED {destination}: {len(data)} B at block {start} '
              f'(+{block_count} blk), slot {free_slot}; next_data_block -> '
              f'{high_water}/{total_blocks}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
