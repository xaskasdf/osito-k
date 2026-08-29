#!/usr/bin/env python3
"""Power-fail-safe metadata transactions shared by OsitoFS v2 scripts."""

import os
import fcntl
import struct
import time
import zlib

MAGIC = 0x4F534632
VERSION = 2
FILETAB_OFF = 1 << 20
SUPER_BACKUP_OFF = 4096
JOURNAL_RECORD_OFF = 8192
JOURNAL_RECORD_SIZE = 12288
JOURNAL_COMMIT_OFF = JOURNAL_RECORD_OFF + JOURNAL_RECORD_SIZE
JOURNAL_COMMIT_SIZE = 512
JOURNAL_MAGIC = 0x4A324653
JOURNAL_VERSION = 2
JOURNAL_COMMIT_MAGIC = 0x43324A53
JOURNAL_MAX_ENTRIES = 2
JOURNAL_OP_RENAME = 1
JOURNAL_OP_DELETE = 2
JOURNAL_OP_REPLACE = 3
SUPER_CRC_OFF = 84
RECORD_CRC_OFF = 9776
COMMIT_CRC_OFF = 20


class JournalError(RuntimeError):
    pass


def lock(image, exclusive):
    fcntl.flock(image.fileno(), fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)


def read_super(image, partition, repair=False):
    image.seek(partition)
    primary = image.read(512)
    image.seek(partition + SUPER_BACKUP_OFF)
    backup = image.read(512)
    selected = primary if super_valid(primary) else backup if super_valid(backup) else None
    if selected is None:
        raise JournalError('both superblocks are invalid')
    if repair and (primary != selected or backup != selected):
        image.seek(partition)
        image.write(selected)
        image.seek(partition + SUPER_BACKUP_OFF)
        image.write(selected)
        _sync(image)
    return bytearray(selected)


def _crc_with_zero(data, offset):
    copy = bytearray(data)
    struct.pack_into('<I', copy, offset, 0)
    return zlib.crc32(copy) & 0xFFFFFFFF


def super_valid(superblock):
    if len(superblock) != 512:
        return False
    magic, version, block_size = struct.unpack_from('<3I', superblock)
    if magic != MAGIC or version != VERSION:
        return False
    if block_size < 65536 or block_size > 1048576 or block_size & (block_size - 1):
        return False
    return struct.unpack_from('<I', superblock, SUPER_CRC_OFF)[0] == \
        _crc_with_zero(superblock, SUPER_CRC_OFF)


def fix_super_crc(superblock):
    struct.pack_into('<I', superblock, SUPER_CRC_OFF, 0)
    crc = zlib.crc32(superblock) & 0xFFFFFFFF
    struct.pack_into('<I', superblock, SUPER_CRC_OFF, crc)
    return crc


def _sync(image):
    image.flush()
    os.fsync(image.fileno())


def _failpoint(image, name):
    if os.environ.get('OSFS2_JOURNAL_FAILPOINT') == name:
        _sync(image)
        os._exit(86)


def _record_valid(record):
    if len(record) != JOURNAL_RECORD_SIZE:
        return False
    magic, version, operation, count = struct.unpack_from('<4I', record)
    if magic != JOURNAL_MAGIC or version != JOURNAL_VERSION:
        return False
    if operation < JOURNAL_OP_RENAME or operation > JOURNAL_OP_REPLACE:
        return False
    if count < 1 or count > JOURNAL_MAX_ENTRIES:
        return False
    slots = struct.unpack_from('<2I', record, 24)
    if any(slot >= 4096 for slot in slots[:count]):
        return False
    page_count = struct.unpack_from('<I', record, 32)[0]
    pages = struct.unpack_from('<2I', record, 36)
    if page_count < 1 or page_count > 2 or \
            any(page >= 256 for page in pages[:page_count]) or \
            len(set(pages[:page_count])) != page_count:
        return False
    if any(slot // 16 not in pages[:page_count] for slot in slots[:count]):
        return False
    before_super = record[48:560]
    after_super = record[560:1072]
    if not super_valid(before_super) or not super_valid(after_super):
        return False
    before_block_size = struct.unpack_from('<I', before_super, 8)[0]
    block_size, total_blocks = struct.unpack_from('<2I', after_super, 8)
    if before_block_size != block_size or before_super[28:44] != after_super[28:44] or \
            total_blocks <= (4 << 20) // block_size or total_blocks > 262144:
        return False
    for index, slot in enumerate(slots[:count]):
        page_index = pages[:page_count].index(slot // 16)
        start = 1584 + page_index * 4096 + (slot % 16) * 256
        entry = record[start:start + 256]
        flags = struct.unpack_from('<I', entry, 84)[0]
        if not flags & 1:
            continue
        if b'\0' not in entry[:64]:
            return False
        size = struct.unpack_from('<Q', entry, 64)[0]
        extent_start, extent_count = struct.unpack_from('<2I', entry, 72)
        if flags & 8:
            if size > 128 or extent_start or extent_count:
                return False
        elif extent_count:
            if extent_start < (4 << 20) // block_size or \
                    extent_start + extent_count > total_blocks or \
                    size > extent_count * block_size:
                return False
        elif size:
            return False
    expected = struct.unpack_from('<I', record, RECORD_CRC_OFF)[0]
    return expected == _crc_with_zero(record, RECORD_CRC_OFF)


def _commit_valid(commit):
    if len(commit) != JOURNAL_COMMIT_SIZE:
        return False
    magic, version = struct.unpack_from('<2I', commit)
    if magic != JOURNAL_COMMIT_MAGIC or version != JOURNAL_VERSION:
        return False
    expected = struct.unpack_from('<I', commit, COMMIT_CRC_OFF)[0]
    return expected == _crc_with_zero(commit, COMMIT_CRC_OFF)


def _clear_commit(image, partition):
    image.seek(partition + JOURNAL_COMMIT_OFF)
    image.write(bytes(JOURNAL_COMMIT_SIZE))
    _sync(image)


def _apply(image, partition, record):
    count = struct.unpack_from('<I', record, 12)[0]
    slots = struct.unpack_from('<2I', record, 24)
    page_count = struct.unpack_from('<I', record, 32)[0]
    pages = struct.unpack_from('<2I', record, 36)
    for index in range(page_count):
        page = record[1584 + index * 4096:5680 + index * 4096]
        image.seek(partition + FILETAB_OFF + pages[index] * 4096)
        image.write(page)
    _failpoint(image, 'entries')
    after_super = record[560:1072]
    image.seek(partition)
    image.write(after_super)
    _failpoint(image, 'primary-super')
    image.seek(partition + SUPER_BACKUP_OFF)
    image.write(after_super)
    _failpoint(image, 'backup-super')
    _sync(image)


def recover(image, partition, repair):
    """Return True when a valid committed transaction is pending."""
    image.seek(partition + JOURNAL_COMMIT_OFF)
    commit = image.read(JOURNAL_COMMIT_SIZE)
    if len(commit) != JOURNAL_COMMIT_SIZE:
        raise JournalError('short journal commit')
    if struct.unpack_from('<I', commit)[0] != JOURNAL_COMMIT_MAGIC:
        return False
    if not _commit_valid(commit):
        if repair:
            _clear_commit(image, partition)
        return False

    image.seek(partition + JOURNAL_RECORD_OFF)
    record = image.read(JOURNAL_RECORD_SIZE)
    if not _record_valid(record):
        raise JournalError('committed journal record is corrupt')
    commit_tx, commit_record_crc = struct.unpack_from('<QI', commit, 8)
    record_tx = struct.unpack_from('<Q', record, 16)[0]
    record_crc = struct.unpack_from('<I', record, RECORD_CRC_OFF)[0]
    if commit_tx != record_tx or commit_record_crc != record_crc:
        raise JournalError('journal commit does not match its record')
    image.seek(partition)
    primary = image.read(512)
    image.seek(partition + SUPER_BACKUP_OFF)
    backup = image.read(512)
    current = primary if super_valid(primary) else backup if super_valid(backup) else None
    after_super = record[560:1072]
    if current is not None and \
            (current[8:12] != after_super[8:12] or
             current[28:44] != after_super[28:44]):
        raise JournalError('journal belongs to another filesystem')
    if repair:
        _apply(image, partition, record)
        _clear_commit(image, partition)
    return True


def commit_entries(image, partition, operation, before_super, after_super,
                   slots, before_entries, after_entries):
    count = len(slots)
    if count < 1 or count > JOURNAL_MAX_ENTRIES or \
            len(before_entries) != count or len(after_entries) != count:
        raise JournalError('invalid journal entry count')
    if len(before_super) != 512 or len(after_super) != 512 or \
            not super_valid(after_super):
        raise JournalError('invalid post-transaction superblock')
    if operation < JOURNAL_OP_RENAME or operation > JOURNAL_OP_REPLACE:
        raise JournalError('invalid journal operation')
    if len(set(slots)) != count or any(slot < 0 or slot >= 4096 for slot in slots):
        raise JournalError('invalid journal slots')
    if any(len(entry) != 256 for entry in before_entries + after_entries):
        raise JournalError('journal entries must be exactly 256 bytes')

    record = bytearray(JOURNAL_RECORD_SIZE)
    transaction_id = (time.time_ns() ^ os.getpid()) & 0xFFFFFFFFFFFFFFFF
    struct.pack_into('<4IQ2I', record, 0, JOURNAL_MAGIC, JOURNAL_VERSION,
                     operation, count, transaction_id,
                     slots[0], slots[1] if count > 1 else 0)
    pages = []
    for slot in slots:
        page = slot // 16
        if page not in pages:
            pages.append(page)
    struct.pack_into('<I2I', record, 32, len(pages), pages[0],
                     pages[1] if len(pages) > 1 else 0)
    record[48:560] = before_super
    record[560:1072] = after_super
    for index in range(count):
        record[1072 + index * 256:1328 + index * 256] = before_entries[index]
    for page_index, page in enumerate(pages):
        image.seek(partition + FILETAB_OFF + page * 4096)
        page_data = image.read(4096)
        if len(page_data) != 4096:
            raise JournalError('short file-table page')
        start = 1584 + page_index * 4096
        record[start:start + 4096] = page_data
    for index, slot in enumerate(slots):
        page_index = pages.index(slot // 16)
        start = 1584 + page_index * 4096 + (slot % 16) * 256
        record[start:start + 256] = after_entries[index]
    record_crc = _crc_with_zero(record, RECORD_CRC_OFF)
    struct.pack_into('<I', record, RECORD_CRC_OFF, record_crc)

    _clear_commit(image, partition)
    image.seek(partition + JOURNAL_RECORD_OFF)
    image.write(record)
    _sync(image)
    _failpoint(image, 'prepared')

    commit = bytearray(JOURNAL_COMMIT_SIZE)
    struct.pack_into('<IIQI', commit, 0, JOURNAL_COMMIT_MAGIC,
                     JOURNAL_VERSION, transaction_id, record_crc)
    struct.pack_into('<I', commit, COMMIT_CRC_OFF,
                     _crc_with_zero(commit, COMMIT_CRC_OFF))
    image.seek(partition + JOURNAL_COMMIT_OFF)
    image.write(commit)
    _sync(image)
    _failpoint(image, 'committed')
    _apply(image, partition, record)
    _clear_commit(image, partition)
