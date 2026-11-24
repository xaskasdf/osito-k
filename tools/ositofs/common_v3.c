/*
 * OsitoFS v3 host tools — common utilities
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#include <time.h>

#include "common_v3.h"

int osfs3_open_device(const char *path, int readonly)
{
    int flags = (readonly ? O_RDONLY : O_RDWR);
#ifdef O_DIRECT
    flags |= O_DIRECT;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        fprintf(stderr, "osfs3: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

void osfs3_close_device(int fd)
{
    if (fd >= 0) close(fd);
}

int osfs3_read_block(int fd, uint32_t block, void *buf)
{
    uint64_t offset = (uint64_t)block << OSFS3_BLOCK_SHIFT;
    ssize_t n = pread(fd, buf, OSFS3_BLOCK_SIZE, offset);
    if (n != OSFS3_BLOCK_SIZE) {
        fprintf(stderr, "osfs3: read block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs3_write_block(int fd, uint32_t block, const void *buf)
{
    uint64_t offset = (uint64_t)block << OSFS3_BLOCK_SHIFT;
    ssize_t n = pwrite(fd, buf, OSFS3_BLOCK_SIZE, offset);
    if (n != OSFS3_BLOCK_SIZE) {
        fprintf(stderr, "osfs3: write block %u failed: %s\n",
                block, n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

int osfs3_read_bytes(int fd, uint64_t offset, void *buf, size_t len)
{
    ssize_t n = pread(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs3: read %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    return 0;
}

int osfs3_write_bytes(int fd, uint64_t offset, const void *buf, size_t len)
{
    ssize_t n = pwrite(fd, buf, len, offset);
    if (n != (ssize_t)len) {
        fprintf(stderr, "osfs3: write %zu bytes at 0x%llx failed: %s\n",
                len, (unsigned long long)offset,
                n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    return 0;
}

int osfs3_read_super(int fd, osfs3_super_t *sb)
{
    void *blk = osfs3_alloc_block();
    if (!blk) return -1;

    if (osfs3_read_block(fd, OSFS3_SUPERBLOCK_BLK, blk) < 0) {
        osfs3_free_block(blk);
        return -1;
    }

    memcpy(sb, blk, sizeof(*sb));
    osfs3_free_block(blk);

    if (sb->magic != OSFS3_MAGIC) {
        fprintf(stderr, "osfs3: bad magic 0x%08X (expected 0x%08X)\n",
                sb->magic, OSFS3_MAGIC);
        return -1;
    }
    if (sb->version != OSFS3_VERSION) {
        fprintf(stderr, "osfs3: unsupported version %u\n", sb->version);
        return -1;
    }
    if (sb->block_size != OSFS3_BLOCK_SIZE ||
        sb->total_blocks <= sb->first_data_block ||
        sb->total_blocks > OSFS3_BITMAP_BITS ||
        sb->total_inodes < 2 || sb->total_inodes > OSFS3_MAX_INODES ||
        sb->first_data_block != OSFS3_FIRST_DATA_BLOCK(sb->total_inodes) ||
        sb->root_inode == 0 || sb->root_inode >= sb->total_inodes) {
        fprintf(stderr, "osfs3: invalid filesystem geometry\n");
        return -1;
    }

    uint32_t saved_crc = sb->crc32;
    sb->crc32 = 0;
    uint32_t calc_crc = osfs3_crc32(sb, sizeof(*sb));
    sb->crc32 = saved_crc;

    if (calc_crc != saved_crc) {
        fprintf(stderr, "osfs3: superblock CRC mismatch\n");
        return -1;
    }
    if (sb->block_size != OSFS3_BLOCK_SIZE) {
        fprintf(stderr, "osfs3: unsupported block size %u\n", sb->block_size);
        return -1;
    }
    if (sb->total_blocks > osfs3_max_blocks()) {
        fprintf(stderr, "osfs3: total blocks %u exceed bitmap capacity %u\n",
                sb->total_blocks, osfs3_max_blocks());
        return -1;
    }
    if (!osfs3_valid_inode_count(sb->total_inodes)) {
        fprintf(stderr, "osfs3: invalid inode count %u\n", sb->total_inodes);
        return -1;
    }
    if (sb->first_data_block != osfs3_first_data_block_for_inodes(sb->total_inodes)) {
        fprintf(stderr, "osfs3: invalid first_data_block %u for %u inodes\n",
                sb->first_data_block, sb->total_inodes);
        return -1;
    }

    return 0;
}

uint64_t osfs3_device_size(int fd)
{
    uint64_t size = 0;
#ifdef BLKGETSIZE64
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
#else
    {
#endif
        off_t pos = lseek(fd, 0, SEEK_END);
        if (pos < 0) return 0;
        size = (uint64_t)pos;
        lseek(fd, 0, SEEK_SET);
    }
    return size;
}

void *osfs3_alloc_block(void)
{
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, OSFS3_BLOCK_SIZE) != 0) {
        fprintf(stderr, "osfs3: out of memory (block alloc)\n");
        return NULL;
    }
    memset(buf, 0, OSFS3_BLOCK_SIZE);
    return buf;
}

void osfs3_free_block(void *buf)
{
    free(buf);
}

void osfs3_gen_uuid(uint8_t uuid[16])
{
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (int i = 0; i < 16; i++)
        uuid[i] = (uint8_t)(rand() & 0xFF);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

void osfs3_print_size(uint64_t bytes)
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
