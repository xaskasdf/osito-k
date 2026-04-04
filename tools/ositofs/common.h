/*
 * OsitoFS v2 host tools — common utilities
 */

#ifndef OSITOFS_COMMON_H
#define OSITOFS_COMMON_H

#include <stdint.h>
#include <stddef.h>

#include "../../include/common/ositofs2_format.h"

/* Runtime block size (set by osfs2_read_super or mkfs before first use) */
extern uint32_t osfs2_block_sz;

/* Block-aligned I/O (uses O_DIRECT internally) */
int  osfs2_open_device(const char *path, int readonly);
void osfs2_close_device(int fd);

int  osfs2_read_block(int fd, uint32_t block, void *buf);
int  osfs2_write_block(int fd, uint32_t block, const void *buf);

int  osfs2_read_bytes(int fd, uint64_t offset, void *buf, size_t len);
int  osfs2_write_bytes(int fd, uint64_t offset, const void *buf, size_t len);

/* Read superblock and validate magic/version. Sets osfs2_block_sz. */
int  osfs2_read_super(int fd, osfs2_super_t *sb);

/* Get device size in bytes */
uint64_t osfs2_device_size(int fd);

/* Aligned buffer allocation (4K alignment for O_DIRECT) */
void *osfs2_alloc_block(void);     /* allocates osfs2_block_sz bytes */
void *osfs2_alloc_aligned(uint32_t size);  /* allocates arbitrary aligned buf */
void  osfs2_free_block(void *buf);

/* UUID generation (random) */
void osfs2_gen_uuid(uint8_t uuid[16]);

/* Formatted output helpers */
void osfs2_print_size(uint64_t bytes);
const char *osfs2_quant_name(uint32_t quant_type);

#endif /* OSITOFS_COMMON_H */
