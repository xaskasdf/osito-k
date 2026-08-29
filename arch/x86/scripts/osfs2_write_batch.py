#!/usr/bin/env python3
"""Add a manifest of files through independent OsitoFS v2 transactions."""

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
    if len(sys.argv) != 3:
        print('usage: osfs2_write_batch.py <image> <manifest>')
        return 1
    image_path, manifest_path = sys.argv[1:]
    entries = []
    with open(manifest_path) as manifest:
        for line in manifest:
            line = line.rstrip('\n')
            if line and '\t' in line:
                entries.append(tuple(line.split('\t', 1)))

    with open(image_path, 'r+b') as image:
        lock(image, True)
        partition = find_part(image)
        if partition is None:
            print('no superblock')
            return 1
        recover(image, partition, True)
        superblock = read_super(image, partition, True)
        _, _, block_size, total_blocks = struct.unpack_from('<4I', superblock)
        image.seek(partition + FILETAB_OFF)
        table = bytearray(image.read(MAX_FILES * 256))

        existing = set()
        free_slots = []
        for slot in range(MAX_FILES):
            entry = table[slot * 256:(slot + 1) * 256]
            flags = struct.unpack_from('<I', entry, 84)[0]
            if flags & FLAG_VALID:
                existing.add(entry[:NAME_LEN].split(b'\0')[0]
                             .decode('latin1', 'replace').lower())
            else:
                free_slots.append(slot)

        added = duplicate = skipped = 0
        incomplete = False
        for host_path, destination in entries:
            try:
                encoded_destination = destination.encode('latin1')
            except UnicodeEncodeError:
                skipped += 1
                continue
            if not os.path.isfile(host_path) or not encoded_destination or \
                    len(encoded_destination) >= NAME_LEN:
                skipped += 1
                continue
            if destination.lower() in existing:
                duplicate += 1
                continue
            if not free_slots:
                print('OUT OF FILE SLOTS')
                incomplete = True
                break

            data = open(host_path, 'rb').read()
            block_count = (len(data) + block_size - 1) // block_size
            start = find_free_extent(table, total_blocks, block_size, block_count)
            if start is None:
                print(f'OUT OF SPACE at {destination} (need {block_count})')
                incomplete = True
                break
            if block_count:
                image.seek(partition + start * block_size)
                image.write(data)
                image.write(bytes(block_count * block_size - len(data)))
                for block in range(start, start + block_count):
                    image.seek(partition + CRCTAB_OFF + block * 4)
                    image.write(bytes(4))
            image.flush()
            os.fsync(image.fileno())

            slot = free_slots.pop(0)
            old_entry = bytes(table[slot * 256:(slot + 1) * 256])
            new_entry = bytearray(256)
            new_entry[:len(encoded_destination)] = encoded_destination
            struct.pack_into('<Q', new_entry, 64, len(data))
            struct.pack_into('<II', new_entry, 72, start, block_count)
            struct.pack_into('<I', new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
            struct.pack_into('<I', new_entry, 84, FLAG_VALID | FLAG_RAW)
            struct.pack_into('<H', new_entry, 244, 0xFFFF)
            now = int(time.time())
            struct.pack_into('<II', new_entry, 246, now, now)

            before_super = bytearray(superblock)
            table[slot * 256:(slot + 1) * 256] = new_entry
            after_super = bytearray(superblock)
            recompute_super(after_super, table)
            commit_entries(image, partition, JOURNAL_OP_REPLACE,
                           before_super, after_super, [slot], [old_entry],
                           [new_entry])
            superblock = after_super
            existing.add(destination.lower())
            added += 1

        next_block = struct.unpack_from('<I', superblock, 24)[0]
        print(f'added={added} dup={duplicate} skipped={skipped} '
              f'nextblk={next_block}/{total_blocks}')
    return 1 if incomplete or skipped else 0


if __name__ == '__main__':
    sys.exit(main())
