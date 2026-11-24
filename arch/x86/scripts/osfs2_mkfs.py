#!/usr/bin/env python3
"""Create an empty raw OsitoFS v2 image."""

import os
import struct
import sys
import time

from osfs2_journal import fix_super_crc

BLOCK_SIZE = 1 << 20
DATA_OFF = 4 << 20
MAGIC = 0x4F534632
VERSION = 2


def main():
    if len(sys.argv) not in (3, 4):
        print('usage: osfs2_mkfs.py <image> <total_blocks> [label]')
        return 1
    image_path = sys.argv[1]
    total_blocks = int(sys.argv[2])
    label = sys.argv[3] if len(sys.argv) == 4 else 'osito'
    if total_blocks <= DATA_OFF // BLOCK_SIZE or total_blocks > 262144:
        print('total_blocks must be in the range 5..262144')
        return 1
    try:
        encoded_label = label.encode('latin1')
    except UnicodeEncodeError:
        print('label must be Latin-1')
        return 1
    if len(encoded_label) >= 32:
        print('label must contain at most 31 bytes')
        return 1

    superblock = bytearray(512)
    data_start = DATA_OFF // BLOCK_SIZE
    struct.pack_into('<7I', superblock, 0, MAGIC, VERSION, BLOCK_SIZE,
                     total_blocks, data_start, 0, data_start)
    superblock[28:44] = os.urandom(16)
    superblock[44:44 + len(encoded_label)] = encoded_label
    struct.pack_into('<Q', superblock, 76, int(time.time()))
    fix_super_crc(superblock)

    with open(image_path, 'w+b') as image:
        image.truncate(total_blocks * BLOCK_SIZE)
        image.seek(0)
        image.write(superblock)
        image.seek(4096)
        image.write(superblock)
        image.flush()
        os.fsync(image.fileno())
    print(f'created {image_path}: {total_blocks} blocks '
          f'({total_blocks} MB), label={label}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
