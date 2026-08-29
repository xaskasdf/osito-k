#!/usr/bin/env python3
"""Rebuild invalid OsitoFS v2 superblocks from an intact file table."""

import os
import struct
import sys
import time

from osfs2_journal import (FILE_ENTRY_SIZE, FILETAB_OFF, JOURNAL_COMMIT_OFF,
                           JOURNAL_COMMIT_SIZE, LAYOUT_MAGIC, LEGACY_MAX_FILES,
                           MAGIC, MAX_BLOCKS, VERSION, JournalError,
                           fix_super_crc, layout_from_super, lock)

BLOCK_SIZE = 1 << 20
FLAG_VALID = 1
FLAG_INLINE = 8


def recover_layout(image):
    for offset in (0, 4096):
        image.seek(offset)
        candidate = image.read(512)
        try:
            layout = layout_from_super(candidate)
        except JournalError:
            continue
        block_size = struct.unpack_from('<I', candidate, 8)[0]
        if block_size >= 65536 and block_size <= BLOCK_SIZE and \
                not block_size & (block_size - 1):
            return candidate, layout

    fallback = bytearray(512)
    struct.pack_into('<3I', fallback, 0, MAGIC, VERSION, BLOCK_SIZE)
    return fallback, {
        'version': VERSION,
        'max_files': LEGACY_MAX_FILES,
        'filetab_size': LEGACY_MAX_FILES * FILE_ENTRY_SIZE,
        'data_off': 4 << 20,
    }


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
        candidate, layout = recover_layout(image)
        version, block_size = struct.unpack_from('<2I', candidate, 4)
        image.seek(0, os.SEEK_END)
        image_size = image.tell()
        if image_size % block_size:
            print(f'image size is not aligned to the {block_size}-byte block size')
            return 1
        total_blocks = image_size // block_size
        data_start = layout['data_off'] // block_size
        if total_blocks <= data_start or total_blocks > MAX_BLOCKS:
            print('image size is outside the OsitoFS v2 range')
            return 1

        image.seek(FILETAB_OFF)
        table = image.read(layout['filetab_size'])
        if len(table) != layout['filetab_size']:
            print('short file table')
            return 1

        file_count = 0
        used_blocks = data_start
        high_water = used_blocks
        for slot in range(layout['max_files']):
            entry = table[slot * FILE_ENTRY_SIZE:(slot + 1) * FILE_ENTRY_SIZE]
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
                    count > 0 and start >= data_start and
                    start + count <= total_blocks and
                    size <= count * block_size)
            if not valid_extent:
                print(f'invalid extent in slot {slot}')
                return 1
            file_count += 1
            used_blocks += count
            high_water = max(high_water, start + count)

        superblock = bytearray(512)
        struct.pack_into('<7I', superblock, 0, MAGIC, version, block_size,
                         total_blocks, used_blocks, file_count, high_water)
        superblock[28:44] = os.urandom(16)
        superblock[44:44 + len(encoded_label)] = encoded_label
        struct.pack_into('<Q', superblock, 76, int(time.time()))
        if struct.unpack_from('<I', candidate, 88)[0] == LAYOUT_MAGIC:
            superblock[88:100] = candidate[88:100]
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
