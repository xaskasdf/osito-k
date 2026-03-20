/*
 * OsitoK x86-64 — GPT Partition Table Parser
 *
 * Parses UEFI GPT to find the OsitoFS partition.
 * Search strategy:
 *   1. Name match: partition name contains "osito" (case-insensitive)
 *   2. Superblock probe: read first sector, check OSFS2_MAGIC
 */

#ifndef OSITOK_GPT_H
#define OSITOK_GPT_H

#include "../include/types.h"

/* ── GPT structures (UEFI spec 2.10, Table 5-5/5-6) ─────────── */

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t  d4[8];
} gpt_guid_t;

typedef struct __attribute__((packed)) {
    char       signature[8];           /* "EFI PART" */
    uint32_t   revision;               /* 0x00010000 */
    uint32_t   header_size;            /* 92 typical */
    uint32_t   header_crc32;
    uint32_t   reserved;
    uint64_t   current_lba;
    uint64_t   backup_lba;
    uint64_t   first_usable_lba;
    uint64_t   last_usable_lba;
    gpt_guid_t disk_guid;
    uint64_t   partition_entry_lba;
    uint32_t   num_partition_entries;
    uint32_t   partition_entry_size;   /* 128 typical */
    uint32_t   partition_entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    gpt_guid_t type_guid;
    gpt_guid_t unique_guid;
    uint64_t   first_lba;
    uint64_t   last_lba;
    uint64_t   attributes;
    uint16_t   name[36];               /* UTF-16LE */
} gpt_entry_t;

/*
 * gpt_find_ositofs — Locate OsitoFS partition in GPT
 *
 * Returns 0 on success, -1 if not found.
 * On success, *part_offset = byte offset, *part_size = byte size.
 */
int gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size);

#endif /* OSITOK_GPT_H */
