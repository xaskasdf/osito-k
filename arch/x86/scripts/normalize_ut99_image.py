#!/usr/bin/env python3
"""Create a UT99 OsitoFS2 fixture with a conventional directory layout."""

import argparse
import os
import shutil
import struct
import sys
import time
import zlib

from osfs2_journal import (FILETAB_OFF, JOURNAL_OP_RENAME,
                           JOURNAL_OP_REPLACE, commit_entries,
                           layout_from_super, lock, read_super, recover)
from osfs2_replace import (FLAG_RAW, FLAG_VALID, NAME_LEN, find_free_extent,
                           find_part, recompute_super)


CONTENT_DIRECTORIES = {
    ".unr": "Maps",
    ".utx": "Textures",
    ".uax": "Sounds",
    ".umx": "Music",
}
SYSTEM_EXTENSIONS = {
    ".bat", ".dll", ".exe", ".ini", ".int", ".log", ".u",
    ".det", ".est", ".frt", ".itt", ".jpt", ".kot", ".ptt",
    ".rut", ".tct",
}
INI_PATHS = {
    "*.u": r"..\System\*.u",
    "*.unr": r"..\Maps\*.unr",
    "*.utx": r"..\Textures\*.utx",
    "*.uax": r"..\Sounds\*.uax",
    "*.umx": r"..\Music\*.umx",
}


def entry_name(entry):
    return entry[:NAME_LEN].split(b"\0", 1)[0].decode("latin1", "replace")


def canonical_name(name):
    normalized = name.replace("/", "\\")
    parts = normalized.split("\\")
    basename = parts[-1]
    extension = os.path.splitext(basename)[1].lower()

    if extension in CONTENT_DIRECTORIES:
        directory = CONTENT_DIRECTORIES[extension]
        if len(parts) == 1 or parts[0].lower() == "system":
            return directory + "\\" + basename
        return name

    if extension in SYSTEM_EXTENSIONS and len(parts) == 1:
        return "System\\" + basename
    return name


def rename_entries(image, partition, superblock, table, layout):
    existing = {}
    for slot in range(layout["max_files"]):
        entry = table[slot * 256:(slot + 1) * 256]
        if struct.unpack_from("<I", entry, 84)[0] & FLAG_VALID:
            existing[entry_name(entry).lower()] = slot

    pending = []
    collisions = []
    for slot in range(layout["max_files"]):
        entry = table[slot * 256:(slot + 1) * 256]
        if not struct.unpack_from("<I", entry, 84)[0] & FLAG_VALID:
            continue
        old_name = entry_name(entry)
        new_name = canonical_name(old_name)
        if old_name.lower() == new_name.lower():
            continue
        if len(new_name.encode("latin1")) >= NAME_LEN:
            collisions.append((old_name, new_name, "name too long"))
            continue
        owner = existing.get(new_name.lower())
        if owner is not None and owner != slot:
            collisions.append((old_name, new_name, "destination exists"))
            continue
        existing.pop(old_name.lower(), None)
        existing[new_name.lower()] = slot
        pending.append((slot, old_name, new_name))

    renamed = 0
    while pending:
        batch = pending[:2]
        pending = pending[2:]
        slots = []
        before_entries = []
        after_entries = []
        for slot, old_name, new_name in batch:
            offset = slot * 256
            old_entry = bytes(table[offset:offset + 256])
            new_entry = bytearray(old_entry)
            new_entry[:NAME_LEN] = bytes(NAME_LEN)
            encoded = new_name.encode("latin1")
            new_entry[:len(encoded)] = encoded
            struct.pack_into("<I", new_entry, 250, int(time.time()))
            table[offset:offset + 256] = new_entry
            slots.append(slot)
            before_entries.append(old_entry)
            after_entries.append(bytes(new_entry))
            print(f"rename: {old_name} -> {new_name}")
        commit_entries(image, partition, JOURNAL_OP_RENAME,
                       superblock, superblock, slots, before_entries,
                       after_entries)
        renamed += len(batch)

    for old_name, new_name, reason in collisions:
        print(f"skip: {old_name} -> {new_name}: {reason}")
    return renamed, len(collisions)


def normalized_ini(data):
    text = data.decode("latin1")
    output = []
    changed = False
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        ending = line[len(body):]
        if "=" in body:
            key, value = body.split("=", 1)
            replacement = INI_PATHS.get(value.strip().replace("/", "\\").lower())
            if key.strip().lower() == "paths" and replacement:
                body = key + "=" + replacement
                changed = True
        output.append(body + ending)
    return "".join(output).encode("latin1"), changed


def replace_entry(image, partition, superblock, table, layout, slot, data):
    _, _, block_size, total_blocks = struct.unpack_from("<4I", superblock)
    offset = slot * 256
    old_entry = bytes(table[offset:offset + 256])
    old_start, old_count = struct.unpack_from("<2I", old_entry, 72)
    block_count = (len(data) + block_size - 1) // block_size
    start = find_free_extent(table, total_blocks, block_size, block_count,
                             layout)
    if start is None:
        raise RuntimeError("no free extent for normalized INI")

    if block_count:
        image.seek(partition + start * block_size)
        image.write(data)
        image.write(bytes(block_count * block_size - len(data)))
        if layout["has_crc"]:
            for block in range(start, start + block_count):
                image.seek(partition + layout["crctab_off"] + block * 4)
                image.write(bytes(4))
    image.flush()
    os.fsync(image.fileno())

    new_entry = bytearray(old_entry)
    struct.pack_into("<Q", new_entry, 64, len(data))
    struct.pack_into("<2I", new_entry, 72, start, block_count)
    struct.pack_into("<I", new_entry, 80, zlib.crc32(data) & 0xFFFFFFFF)
    struct.pack_into("<I", new_entry, 84,
                     struct.unpack_from("<I", new_entry, 84)[0] | FLAG_RAW)
    struct.pack_into("<I", new_entry, 250, int(time.time()))
    table[offset:offset + 256] = new_entry

    after_super = bytearray(superblock)
    recompute_super(after_super, table, layout)
    commit_entries(image, partition, JOURNAL_OP_REPLACE,
                   superblock, after_super, [slot], [old_entry],
                   [bytes(new_entry)])
    if layout["has_crc"]:
        for block in range(old_start, old_start + old_count):
            image.seek(partition + layout["crctab_off"] + block * 4)
            image.write(bytes(4))
    return after_super


def rewrite_ini_files(image, partition, superblock, table, layout):
    rewritten = 0
    targets = {
        r"system\default.ini",
        r"system\unrealed.ini",
        r"system\unrealtournament.ini",
    }
    for slot in range(layout["max_files"]):
        entry = bytes(table[slot * 256:(slot + 1) * 256])
        if not struct.unpack_from("<I", entry, 84)[0] & FLAG_VALID:
            continue
        name = entry_name(entry)
        if name.replace("/", "\\").lower() not in targets:
            continue
        size = struct.unpack_from("<Q", entry, 64)[0]
        start = struct.unpack_from("<I", entry, 72)[0]
        block_size = struct.unpack_from("<I", superblock, 8)[0]
        image.seek(partition + start * block_size)
        data = image.read(size)
        replacement, changed = normalized_ini(data)
        if not changed:
            continue
        superblock = replace_entry(image, partition, superblock, table,
                                   layout, slot, replacement)
        rewritten += 1
        print(f"rewrite: {name}")
    return superblock, rewritten


def normalize_image(path):
    with open(path, "r+b") as image:
        lock(image, True)
        partition = find_part(image)
        if partition is None:
            raise RuntimeError("no OsitoFS2 superblock found")
        recover(image, partition, True)
        superblock = bytearray(read_super(image, partition, True))
        layout = layout_from_super(superblock)
        image.seek(partition + FILETAB_OFF)
        table = bytearray(image.read(layout["filetab_size"]))
        if len(table) != layout["filetab_size"]:
            raise RuntimeError("short file table")

        renamed, collisions = rename_entries(
            image, partition, superblock, table, layout)
        superblock, rewritten = rewrite_ini_files(
            image, partition, superblock, table, layout)
        image.flush()
        os.fsync(image.fileno())
    print(f"done: renamed={renamed} ini={rewritten} collisions={collisions}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="source OsitoFS2 image")
    parser.add_argument("output", nargs="?",
                        help="new image; omit only with --in-place")
    parser.add_argument("--in-place", action="store_true",
                        help="normalize source directly")
    parser.add_argument("--force", action="store_true",
                        help="replace an existing output image")
    args = parser.parse_args()

    if args.in_place:
        if args.output:
            parser.error("output cannot be used with --in-place")
        target = args.source
    else:
        if not args.output:
            parser.error("output is required unless --in-place is used")
        target = args.output
        if os.path.exists(target) and not args.force:
            parser.error(f"output exists: {target} (use --force)")
        shutil.copy2(args.source, target)

    normalize_image(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
