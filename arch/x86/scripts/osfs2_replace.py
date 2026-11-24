#!/usr/bin/env python3
"""Crash-safe copy-on-write replacement of one OsitoFS v2 file."""

import os
import struct
import sys
import time
import zlib

from osfs2_journal import (JOURNAL_COMMIT_MAGIC, JOURNAL_COMMIT_OFF,
                           JOURNAL_OP_REPLACE, FILETAB_OFF, commit_entries,
                           fix_super_crc, layout_from_super, lock, read_super,
                           recover, super_valid)

MAGIC = 0x4F534632
NAME_LEN = 64
FLAG_VALID = 1
FLAG_RAW = 4


def find_part(image):
    image.seek(0, os.SEEK_END)
    size = image.tell()
    for offset in range(0, min(size, 256 << 20), 1 << 20):
        image.seek(offset)
        primary = image.read(512)
        image.seek(offset + 4096)
        backup = image.read(512)
        image.seek(offset + JOURNAL_COMMIT_OFF)
        commit_magic = image.read(4)
        if super_valid(primary) or super_valid(backup) or \
                (len(commit_magic) == 4 and
                 struct.unpack('<I', commit_magic)[0] == JOURNAL_COMMIT_MAGIC):
            return offset
    return None


def find_free_extent(table, total_blocks, block_size, needed, layout):
    if needed == 0:
        return 0
    data_start = layout['data_off'] // block_size
    used = bytearray(total_blocks)
    used[:data_start] = bytes([1]) * data_start
    for slot in range(layout['max_files']):
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


def recompute_super(superblock, table, layout=None):
    if layout is None:
        layout = layout_from_super(superblock)
    block_size = struct.unpack_from('<I', superblock, 8)[0]
    data_start = layout['data_off'] // block_size
    files, used, high_water = 0, data_start, data_start
    for slot in range(layout['max_files']):
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
    if len(sys.argv) != 4:
        print('usage: osfs2_replace.py <image> <hostfile> <destname>')
        return 1
    image_path, host_path, destination = sys.argv[1:]
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
        layout = layout_from_super(before_super)
        _, _, block_size, total_blocks = struct.unpack_from('<4I', before_super)
        image.seek(partition + FILETAB_OFF)
        table = bytearray(image.read(layout['filetab_size']))
        if len(table) != layout['filetab_size']:
            print('short file table')
            return 1

        slot = -1
        for index in range(layout['max_files']):
            entry = table[index * 256:(index + 1) * 256]
            name = entry[:NAME_LEN].split(b'\0')[0].decode('latin1', 'replace')
            flags = struct.unpack_from('<I', entry, 84)[0]
            if flags & FLAG_VALID and name.lower() == destination.lower():
                slot = index
                break
        if slot < 0:
            print(f'not found: {destination}')
            return 1

        old_entry = bytes(table[slot * 256:(slot + 1) * 256])
        old_start, old_count = struct.unpack_from('<2I', old_entry, 72)
        block_count = (len(data) + block_size - 1) // block_size
        start = find_free_extent(table, total_blocks, block_size, block_count,
                                 layout)
        if start is None:
            print(f'NO ROOM: need a separate {block_count}-block extent')
            return 1

        if block_count:
            image.seek(partition + start * block_size)
            image.write(data)
            image.write(bytes(block_count * block_size - len(data)))
            if layout['has_crc']:
                for block in range(start, start + block_count):
                    image.seek(partition + layout['crctab_off'] + block * 4)
                    image.write(bytes(4))
        image.flush()
        os.fsync(image.fileno())

        new_entry = bytearray(256)
        new_entry[:len(encoded_destination)] = encoded_destination
        struct.pack_into('<Q', new_entry, 64, len(data))
        struct.pack_into('<I', new_entry, 72, start)
        struct.pack_into('<I', new_entry, 76, block_count)
        struct.pack_into('<I', new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
        struct.pack_into('<I', new_entry, 84, FLAG_VALID | FLAG_RAW)
        struct.pack_into('<H', new_entry, 244, 0xFFFF)
        now = int(time.time())
        struct.pack_into('<I', new_entry, 246,
                         struct.unpack_from('<I', old_entry, 246)[0] or now)
        struct.pack_into('<I', new_entry, 250, now)

        table[slot * 256:(slot + 1) * 256] = new_entry
        after_super = bytearray(before_super)
        high_water = recompute_super(after_super, table, layout)
        commit_entries(image, partition, JOURNAL_OP_REPLACE,
                       before_super, after_super, [slot], [old_entry],
                       [new_entry])

        if layout['has_crc']:
            for block in range(old_start, old_start + old_count):
                image.seek(partition + layout['crctab_off'] + block * 4)
                image.write(bytes(4))
        image.flush()
        os.fsync(image.fileno())
        print(f'REPLACED {destination} (slot {slot}): {len(data)} B at block '
              f'{start} (+{block_count} blk); next_data_block -> '
              f'{high_water}/{total_blocks}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
