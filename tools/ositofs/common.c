/*
 * OsitoFS v2 host tools — common utilities
 *
 * Block-aligned I/O using pread/pwrite with O_DIRECT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#ifdef __APPLE__
#include <sys/disk.h>
#endif
#ifdef __linux__
#include <linux/fs.h>
#endif
#include <time.h>

#include "common.h"

uint32_t osfs2_block_sz = OSFS2_DEFAULT_BLOCK_SIZE;

/* ── Device I/O ──────────────────────────────────────────────── */

int osfs2_open_device(const char *path, int readonly)
{
    int flags = (readonly ? O_RDONLY : O_RDWR);
#ifdef O_DIRECT
    flags |= O_DIRECT;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "osfs2: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

void osfs2_close_device(int fd)
{
    if (fd >= 0) close(fd);
}

int osfs2_read_block(int fd, uint32_t block, void *buf)
{
    uint32_t shift = osfs2_block_shift(osfs2_block_sz);
    uint64_t offset = (uint64_t)block << shift;
    ssize_t n = pread(fd, buf, osfs2_block_sz, offset);
    if (n != (ssize_t)osfs2_block_sz) {
        fprintf(stderr, "osfs2: read block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs2_write_block(int fd, uint32_t block, const void *buf)
{
    uint32_t shift = osfs2_block_shift(osfs2_block_sz);
    uint64_t offset = (uint64_t)block << shift;
    ssize_t n = pwrite(fd, buf, osfs2_block_sz, offset);
    if (n != (ssize_t)osfs2_block_sz) {
        fprintf(stderr, "osfs2: write block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

int osfs2_read_bytes(int fd, uint64_t offset, void *buf, size_t len)
{
    ssize_t n = pread(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs2: read %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs2_write_bytes(int fd, uint64_t offset, const void *buf, size_t len)
{
    ssize_t n = pwrite(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs2: write %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

/* ── Superblock ──────────────────────────────────────────────── */

static int osfs2_validate_super(const osfs2_super_t *sb)
{
    if (sb->magic != OSFS2_MAGIC) return -1;
    if (sb->version != OSFS2_VERSION) return -1;
    uint32_t saved_crc = sb->crc32;
    osfs2_super_t tmp;
    memcpy(&tmp, sb, sizeof(tmp));
    tmp.crc32 = 0;
    uint32_t calc_crc = osfs2_crc32(&tmp, sizeof(tmp));
    if (calc_crc != saved_crc) return -1;
    if (!osfs2_valid_block_size(sb->block_size)) return -1;
    if (!osfs2_valid_layout(sb)) return -1;
    if (osfs2_layout_data_off(sb) % sb->block_size != 0) return -1;
    return 0;
}

int osfs2_read_super(int fd, osfs2_super_t *sb)
{
    void *buf = osfs2_alloc_aligned(4096);
    if (!buf) return -1;

    if (osfs2_read_bytes(fd, 0, buf, 4096) < 0) {
        free(buf);
        return -1;
    }
    memcpy(sb, buf, sizeof(*sb));

    if (osfs2_validate_super(sb) < 0) {
        fprintf(stderr, "osfs2: primary superblock invalid, trying backup\n");
        if (osfs2_read_bytes(fd, OSFS2_SUPER_BACKUP_OFF, buf, 4096) < 0) {
            free(buf);
            return -1;
        }
        memcpy(sb, buf, sizeof(*sb));
        if (osfs2_validate_super(sb) < 0) {
            fprintf(stderr, "osfs2: backup superblock also invalid\n");
            free(buf);
            return -1;
        }
        fprintf(stderr, "osfs2: WARNING — using backup superblock\n");
    }

    free(buf);
    osfs2_block_sz = sb->block_size;
    return 0;
}

/* ── Device size ─────────────────────────────────────────────── */

uint64_t osfs2_device_size(int fd)
{
    uint64_t size = 0;
#ifdef BLKGETSIZE64
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
#else
    {
#endif
#ifdef __APPLE__
        uint64_t blocks = 0;
        uint32_t block_size = 0;

        if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 &&
            ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0 &&
            block_size != 0) {
            return blocks * (uint64_t)block_size;
        }
#endif
        /* Might be a regular file */
        off_t pos = lseek(fd, 0, SEEK_END);
        if (pos < 0) return 0;
        size = (uint64_t)pos;
        lseek(fd, 0, SEEK_SET);
    }
    return size;
}

/* ── Aligned allocation ──────────────────────────────────────── */

void *osfs2_alloc_aligned(uint32_t size)
{
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, size) != 0) {
        fprintf(stderr, "osfs2: out of memory (alloc %u)\n", size);
        return NULL;
    }
    memset(buf, 0, size);
    return buf;
}

void *osfs2_alloc_block(void)
{
    return osfs2_alloc_aligned(osfs2_block_sz);
}

void osfs2_free_block(void *buf)
{
    free(buf);
}

/* ── UUID ────────────────────────────────────────────────────── */

void osfs2_gen_uuid(uint8_t uuid[16])
{
    /* Simple random UUID (version 4) */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 16; i++)
        uuid[i] = (uint8_t)(rand() & 0xFF);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;  /* version 4 */
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  /* variant 1 */
}

/* ── Display helpers ─────────────────────────────────────────── */

void osfs2_print_size(uint64_t bytes)
{
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        printf("%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024 * 1024)
        printf("%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        printf("%.1f KB", (double)bytes / 1024.0);
    else
        printf("%llu B", (unsigned long long)bytes);
}

static const char *quant_names[] = {
    [OSFS2_QUANT_NONE]    = "none",
    [OSFS2_QUANT_F32]     = "F32",
    [OSFS2_QUANT_F16]     = "F16",
    [OSFS2_QUANT_Q8_0]    = "Q8_0",
    [OSFS2_QUANT_Q4_0]    = "Q4_0",
    [OSFS2_QUANT_Q4_1]    = "Q4_1",
    [OSFS2_QUANT_Q5_0]    = "Q5_0",
    [OSFS2_QUANT_Q5_1]    = "Q5_1",
    [OSFS2_QUANT_Q2_K]    = "Q2_K",
    [OSFS2_QUANT_Q3_K]    = "Q3_K",
    [OSFS2_QUANT_Q4_K]    = "Q4_K",
    [OSFS2_QUANT_Q5_K]    = "Q5_K",
    [OSFS2_QUANT_Q6_K]    = "Q6_K",
    [OSFS2_QUANT_IQ2_XXS] = "IQ2_XXS",
    [OSFS2_QUANT_IQ3_XXS] = "IQ3_XXS",
};

const char *osfs2_quant_name(uint32_t quant_type)
{
    if (quant_type < sizeof(quant_names) / sizeof(quant_names[0]) && quant_names[quant_type])
        return quant_names[quant_type];
    return "unknown";
}
