#!/usr/bin/env python3
"""Rebuild invalid OsitoFS v2 superblocks from an intact file table."""

import os
import struct
import sys
import time

from osfs2_journal import (JOURNAL_COMMIT_OFF, JOURNAL_COMMIT_SIZE,
                           fix_super_crc, lock)

BLOCK_SIZE = 1 << 20
DATA_OFF = 4 << 20
FILETAB_OFF = 1 << 20
MAX_FILES = 4096
ENTRY_SIZE = 256
FLAG_VALID = 1
FLAG_INLINE = 8
MAGIC = 0x4F534632
VERSION = 2


def main():
    if len(sys.argv) not in (2, 3):
        print('usage: osfs2_rebuild_super.py <image> [label]')
        return 1
    image_path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) == 3 else 'osito-recovered'
    try:
        encoded_label = label.encode('latin1')
    except UnicodeEncodeError:
        print('label must be Latin-1')
        return 1
    if len(encoded_label) >= 32:
        print('label must contain at most 31 bytes')
        return 1

    with open(image_path, 'r+b') as image:
        lock(image, True)
        image.seek(0, os.SEEK_END)
        image_size = image.tell()
        if image_size % BLOCK_SIZE:
            print('image size is not aligned to the 1 MiB block size')
            return 1
        total_blocks = image_size // BLOCK_SIZE
        if total_blocks <= DATA_OFF // BLOCK_SIZE or total_blocks > 262144:
            print('image size is outside the OsitoFS v2 range')
            return 1

        image.seek(FILETAB_OFF)
        table = image.read(MAX_FILES * ENTRY_SIZE)
        if len(table) != MAX_FILES * ENTRY_SIZE:
            print('short file table')
            return 1

        file_count = 0
        used_blocks = DATA_OFF // BLOCK_SIZE
        high_water = used_blocks
        for slot in range(MAX_FILES):
            entry = table[slot * ENTRY_SIZE:(slot + 1) * ENTRY_SIZE]
            flags = struct.unpack_from('<I', entry, 84)[0]
            if not flags & FLAG_VALID:
                continue
            if entry[0] == 0:
                print(f'empty name in slot {slot}')
                return 1
            size = struct.unpack_from('<Q', entry, 64)[0]
            start, count = struct.unpack_from('<2I', entry, 72)
            if flags & FLAG_INLINE:
                valid_extent = size <= 128 and start == 0 and count == 0
            else:
                valid_extent = (size == 0 and count == 0) or (
                    count > 0 and start >= DATA_OFF // BLOCK_SIZE and
                    start + count <= total_blocks and
                    size <= count * BLOCK_SIZE)
            if not valid_extent:
                print(f'invalid extent in slot {slot}')
                return 1
            file_count += 1
            used_blocks += count
            high_water = max(high_water, start + count)

        superblock = bytearray(512)
        struct.pack_into('<7I', superblock, 0, MAGIC, VERSION, BLOCK_SIZE,
                         total_blocks, used_blocks, file_count, high_water)
        superblock[28:44] = os.urandom(16)
        superblock[44:44 + len(encoded_label)] = encoded_label
        struct.pack_into('<Q', superblock, 76, int(time.time()))
        fix_super_crc(superblock)

        image.seek(0)
        image.write(superblock)
        image.seek(4096)
        image.write(superblock)
        image.seek(JOURNAL_COMMIT_OFF)
        image.write(bytes(JOURNAL_COMMIT_SIZE))
        image.flush()
        os.fsync(image.fileno())
    print(f'rebuilt superblocks: files={file_count} used={used_blocks} '
          f'next_data_block={high_water}/{total_blocks}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
