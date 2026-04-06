/*
 * OsitoK x86-64 — initramfs Loader
 *
 * Loads a cpio (newc format) archive as initial root filesystem.
 * Used for early boot before disk drivers are available.
 * Files extracted to tmpfs or OsitoFS.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* tmpfs API for extracting files */
extern void *tmpfs_create(const char *name) __attribute__((weak));
extern int   tmpfs_write(void *handle, uint64_t offset,
                         const void *buf, uint64_t len) __attribute__((weak));

/* ── CPIO newc Header (110 bytes ASCII) ──────────────────────── */

#define CPIO_MAGIC "070701"

typedef struct {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} cpio_header_t;

/* ── Hex Parsing ─────────────────────────────────────────────── */

static uint32_t parse_hex8(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        else d = 0;
        v = (v << 4) | d;
    }
    return v;
}

/* ── Load initramfs ──────────────────────────────────────────── */

/* Extract files from a cpio newc archive in memory.
 * base: pointer to cpio data
 * size: total size of archive
 * Returns number of files extracted. */
int initramfs_load(const uint8_t *base, uint64_t size)
{
    if (!tmpfs_create || !tmpfs_write) {
        serial_puts("[INITRAMFS] tmpfs not available\n");
        return -1;
    }

    serial_puts("[INITRAMFS] Loading archive (");
    serial_putdec(size);
    serial_puts(" bytes)...\n");

    const uint8_t *p = base;
    const uint8_t *end = base + size;
    int file_count = 0;

    while (p + sizeof(cpio_header_t) <= end) {
        const cpio_header_t *hdr = (const cpio_header_t *)p;

        /* Verify magic */
        if (hdr->magic[0] != '0' || hdr->magic[1] != '7' ||
            hdr->magic[2] != '0' || hdr->magic[3] != '7' ||
            hdr->magic[4] != '0' || hdr->magic[5] != '1')
            break;

        uint32_t namesize = parse_hex8(hdr->namesize);
        uint32_t filesize = parse_hex8(hdr->filesize);
        uint32_t mode     = parse_hex8(hdr->mode);

        /* Name follows header, padded to 4-byte boundary */
        const char *name = (const char *)(p + sizeof(cpio_header_t));
        uint32_t hdr_plus_name = sizeof(cpio_header_t) + namesize;
        hdr_plus_name = (hdr_plus_name + 3) & ~3;  /* Align to 4 */

        /* Data follows name, padded to 4-byte boundary */
        const uint8_t *data = p + hdr_plus_name;
        uint32_t data_padded = (filesize + 3) & ~3;

        /* Check for end-of-archive marker */
        if (namesize == 11 && name[0] == 'T' && name[1] == 'R' &&
            name[2] == 'A' && name[3] == 'I' && name[4] == 'L' &&
            name[5] == 'E' && name[6] == 'R')
            break;

        /* Skip directories and special files */
        if ((mode & 0xF000) == 0x8000 && filesize > 0) {
            /* Regular file — extract to tmpfs */
            /* Strip leading "./" if present */
            const char *fname = name;
            if (fname[0] == '.' && fname[1] == '/') fname += 2;
            if (fname[0] == '/') fname++;

            void *fh = tmpfs_create(fname);
            if (fh) {
                tmpfs_write(fh, 0, data, filesize);
                file_count++;
                serial_puts("[INITRAMFS] ");
                serial_puts(fname);
                serial_puts(" (");
                serial_putdec(filesize);
                serial_puts(")\n");
            }
        }

        p += hdr_plus_name + data_padded;
    }

    serial_puts("[INITRAMFS] Extracted ");
    serial_putdec((uint64_t)file_count);
    serial_puts(" files\n");
    return file_count;
}

/* Check if a memory region contains a cpio archive */
bool initramfs_detect(const uint8_t *base, uint64_t size)
{
    if (size < sizeof(cpio_header_t)) return false;
    return (base[0] == '0' && base[1] == '7' && base[2] == '0' &&
            base[3] == '7' && base[4] == '0' && base[5] == '1');
}
