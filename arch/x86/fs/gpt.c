/*
 * OsitoK x86-64 — GPT Partition Table Parser
 *
 * Parses the GPT from NVMe LBA 1 to find the OsitoFS partition.
 *
 * Strategy:
 *   Pass 1: Name match — partition name (UTF-16LE) contains "osito"
 *   Pass 2: Superblock probe — read first 512B, check OSFS2_MAGIC
 *
 * Uses osfs2_crc32() from the shared format header (same IEEE 802.3 poly).
 */

#include "../include/types.h"
#include "gpt.h"
#include "../../../include/common/ositofs2_format.h"
#include "../../../include/common/ositofs3_format.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

/* Read via the active blkdev (NVMe / USB / etc.). main.c selects which
 * device is active before each mount probe via disk_set_active(). */
extern int      disk_read_bytes(uint64_t byte_offset, void *buf, uint64_t len);
extern uint32_t disk_lba_size(void);

/* ── Helpers ─────────────────────────────────────────────────── */

static char gpt_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/*
 * Check if GPT entry name (UTF-16LE, 36 chars max) contains needle
 * (ASCII, case-insensitive substring match).
 */
static bool gpt_name_contains(const uint16_t name[36], const char *needle)
{
    /* Convert UTF-16LE name to lowercase ASCII (skip chars >= 128) */
    char ascii[37];
    int  len = 0;

    for (int i = 0; i < 36 && name[i]; i++) {
        if (name[i] < 128)
            ascii[len++] = gpt_tolower((char)name[i]);
    }
    ascii[len] = '\0';

    /* Lowercase the needle and do substring search */
    int nlen = 0;
    for (const char *p = needle; *p; p++)
        nlen++;

    for (int i = 0; i <= len - nlen; i++) {
        int j;
        for (j = 0; j < nlen; j++) {
            if (ascii[i + j] != gpt_tolower(needle[j]))
                break;
        }
        if (j == nlen)
            return true;
    }

    return false;
}

/* Print UTF-16LE name to serial as ASCII */
static void gpt_print_name(const uint16_t name[36])
{
    char buf[37];
    int len = 0;
    for (int i = 0; i < 36 && name[i]; i++) {
        buf[len++] = (name[i] < 128) ? (char)name[i] : '?';
    }
    buf[len] = '\0';
    serial_puts(buf);
}

/* Check if GUID is all zeros (unused entry) */
static bool gpt_guid_is_zero(gpt_guid_t g)
{
    return g.d1 == 0 && g.d2 == 0 && g.d3 == 0 &&
           g.d4[0] == 0 && g.d4[1] == 0 && g.d4[2] == 0 &&
           g.d4[3] == 0 && g.d4[4] == 0 && g.d4[5] == 0 &&
           g.d4[6] == 0 && g.d4[7] == 0;
}

/* ── Main GPT parser ────────────────────────────────────────── */

int gpt_find_ositofs(uint64_t *part_offset, uint64_t *part_size)
{
    uint32_t lba_size = disk_lba_size();

    /* ── Read GPT header (LBA 1) ── */
    serial_puts("[GPT] Reading GPT header...\n");

    gpt_header_t hdr;
    if (disk_read_bytes((uint64_t)lba_size, &hdr, sizeof(hdr)) < 0) {
        serial_puts("[GPT] Failed to read LBA 1\n");
        return -1;
    }

    /* Validate signature */
    if (memcmp(hdr.signature, "EFI PART", 8) != 0) {
        serial_puts("[GPT] No GPT found (invalid signature)\n");
        return -1;
    }

    /* Validate header CRC32 */
    uint32_t saved_crc = hdr.header_crc32;
    hdr.header_crc32 = 0;
    uint32_t calc_crc = osfs2_crc32(&hdr, hdr.header_size);
    if (calc_crc != saved_crc) {
        serial_puts("[GPT] Header CRC32 mismatch (got ");
        serial_puthex(calc_crc, 8);
        serial_puts(", expected ");
        serial_puthex(saved_crc, 8);
        serial_puts(")\n");
        return -1;
    }
    hdr.header_crc32 = saved_crc;

    uint32_t num_entries = hdr.num_partition_entries;
    uint32_t entry_size = hdr.partition_entry_size;
    uint64_t entries_start = hdr.partition_entry_lba * lba_size;

    serial_puts("[GPT] GPT header valid, ");
    serial_putdec(num_entries);
    serial_puts(" entries (");
    serial_putdec(entry_size);
    serial_puts("B each)\n");

    /* Cap to reasonable limit */
    if (num_entries > 256)
        num_entries = 256;

    /* ── Pass 1: Name match ("osito") ── */
    for (uint32_t i = 0; i < num_entries; i++) {
        gpt_entry_t entry;
        uint64_t entry_off = entries_start + (uint64_t)i * entry_size;

        if (disk_read_bytes(entry_off, &entry, sizeof(entry)) < 0)
            continue;

        if (gpt_guid_is_zero(entry.type_guid))
            continue;

        uint16_t name_copy[36];
        memcpy(name_copy, entry.name, sizeof(name_copy));

        if (gpt_name_contains(name_copy, "osito")) {
            uint64_t off  = entry.first_lba * lba_size;
            uint64_t size = (entry.last_lba - entry.first_lba + 1) * lba_size;

            serial_puts("[GPT] Partition ");
            serial_putdec(i + 1);
            serial_puts(": \"");
            gpt_print_name(name_copy);
            serial_puts("\" at LBA ");
            serial_puthex(entry.first_lba, 8);
            serial_puts(" (");
            serial_putdec(size >> 20);
            serial_puts(" MB)\n");

            *part_offset = off;
            *part_size   = size;
            return 0;
        }
    }

    /* ── Pass 2: Superblock magic probe ── */
    serial_puts("[GPT] No name match, probing for OsitoFS magic...\n");

    for (uint32_t i = 0; i < num_entries; i++) {
        gpt_entry_t entry;
        uint64_t entry_off = entries_start + (uint64_t)i * entry_size;

        if (disk_read_bytes(entry_off, &entry, sizeof(entry)) < 0)
            continue;

        if (gpt_guid_is_zero(entry.type_guid))
            continue;

        /* Read first 512 bytes of partition */
        uint64_t part_start = entry.first_lba * lba_size;
        uint8_t probe[512];

        if (disk_read_bytes(part_start, probe, sizeof(probe)) < 0)
            continue;

        /* Check for either supported OsitoFS superblock at offset 0. */
        uint32_t magic = *(uint32_t *)probe;
        if (magic == OSFS2_MAGIC || magic == OSFS3_MAGIC) {
            uint64_t off  = part_start;
            uint64_t size = (entry.last_lba - entry.first_lba + 1) * lba_size;

            serial_puts("[GPT] Partition ");
            serial_putdec(i + 1);
            serial_puts(": OsitoFS magic found at LBA ");
            serial_puthex(entry.first_lba, 8);
            serial_puts(" (");
            serial_putdec(size >> 20);
            serial_puts(" MB)\n");

            *part_offset = off;
            *part_size   = size;
            return 0;
        }
    }

    serial_puts("[GPT] No OsitoFS partition found\n");
    return -1;
}
