#!/usr/bin/env python3
"""Grow OsitoFS v2 and transactionally add maps from a host directory."""

import glob
import os
import struct
import sys
import time
import zlib

from osfs2_journal import (FILETAB_OFF, JOURNAL_OP_REPLACE, commit_entries,
                           fix_super_crc, layout_from_super, lock, read_super,
                           recover)
from osfs2_replace import (FLAG_RAW, FLAG_VALID, NAME_LEN, find_free_extent,
                           find_part, recompute_super)

def main():
    if len(sys.argv) < 3:
        print('usage: osfs2_grow_addmaps.py <image> <map-dir> [headroom-blocks]')
        return 1
    image_path, host_directory = sys.argv[1:3]
    headroom = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    paths = sorted(glob.glob(os.path.join(host_directory, '*.unr')))
    if not paths:
        print(f'no *.unr in {host_directory}')
        return 1

    with open(image_path, 'r+b') as image:
        lock(image, True)
        partition = find_part(image)
        if partition is None:
            print('no superblock')
            return 1
        if partition != 0:
            print('grow_addmaps only supports raw OsitoFS images')
            return 1
        recover(image, partition, True)
        superblock = read_super(image, partition, True)
        layout = layout_from_super(superblock)
        _, _, block_size, total_blocks = struct.unpack_from('<4I', superblock)
        image.seek(partition + FILETAB_OFF)
        table = bytearray(image.read(layout['filetab_size']))
        if len(table) != layout['filetab_size']:
            print('short file table')
            return 1

        names = set()
        free_slots = []
        for slot in range(layout['max_files']):
            entry = table[slot * 256:(slot + 1) * 256]
            flags = struct.unpack_from('<I', entry, 84)[0]
            if flags & FLAG_VALID:
                names.add(entry[:NAME_LEN].split(b'\0')[0]
                          .decode('latin1', 'replace').lower())
            else:
                free_slots.append(slot)

        plan = []
        blocks_needed = 0
        for path in paths:
            destination = os.path.basename(path)
            try:
                encoded = destination.encode('latin1')
            except UnicodeEncodeError:
                print(f'invalid non-Latin-1 map name: {destination}')
                return 1
            if not encoded or len(encoded) >= NAME_LEN:
                print(f'invalid map name length: {destination}')
                return 1
            if destination.lower() in names:
                continue
            size = os.path.getsize(path)
            count = (size + block_size - 1) // block_size
            plan.append((path, destination, count))
            blocks_needed += count
        if len(plan) > len(free_slots):
            print(f'not enough file slots: need {len(plan)}, free {len(free_slots)}')
            return 1

        current_hwm = struct.unpack_from('<I', superblock, 24)[0]
        new_total = max(total_blocks, current_hwm + blocks_needed + headroom)
        if new_total > 262144:
            print('requested image exceeds OsitoFS v2 block limit')
            return 1
        new_size = partition + new_total * block_size
        print(f'adding {len(plan)} maps, {blocks_needed} blocks; growing total '
              f'{total_blocks} -> {new_total}')
        image.truncate(new_size)
        image.flush()
        os.fsync(image.fileno())

        if new_total != total_blocks:
            before_super = bytearray(superblock)
            struct.pack_into('<I', superblock, 12, new_total)
            fix_super_crc(superblock)
            unchanged = bytes(table[:256])
            commit_entries(image, partition, JOURNAL_OP_REPLACE,
                           before_super, superblock, [0], [unchanged],
                           [unchanged])
            total_blocks = new_total

        for path, destination, block_count in plan:
            data = open(path, 'rb').read()
            start = find_free_extent(table, total_blocks, block_size,
                                     block_count, layout)
            if start is None:
                print(f'no contiguous extent for {destination}')
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

            slot = free_slots.pop(0)
            old_entry = bytes(table[slot * 256:(slot + 1) * 256])
            new_entry = bytearray(256)
            name = destination.encode('latin1')
            new_entry[:len(name)] = name
            struct.pack_into('<QII', new_entry, 64, len(data), start, block_count)
            struct.pack_into('<I', new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
            struct.pack_into('<I', new_entry, 84, FLAG_VALID | FLAG_RAW)
            struct.pack_into('<H', new_entry, 244, 0xFFFF)
            now = int(time.time())
            struct.pack_into('<II', new_entry, 246, now, now)
            before_super = bytearray(superblock)
            table[slot * 256:(slot + 1) * 256] = new_entry
            after_super = bytearray(superblock)
            recompute_super(after_super, table, layout)
            commit_entries(image, partition, JOURNAL_OP_REPLACE,
                           before_super, after_super, [slot], [old_entry],
                           [new_entry])
            superblock = after_super

        next_block = struct.unpack_from('<I', superblock, 24)[0]
        print(f'after: total={new_total} files added={len(plan)} '
              f'nextblk={next_block}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
