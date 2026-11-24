/*
 * OsitoFS v3 host tools — common utilities
 */

#ifndef OSITOFS3_COMMON_H
#define OSITOFS3_COMMON_H

#include <stdint.h>
#include <stddef.h>

#include "../../include/common/ositofs3_format.h"

/* Block-aligned I/O */
int  osfs3_open_device(const char *path, int readonly);
void osfs3_close_device(int fd);

int  osfs3_read_block(int fd, uint32_t block, void *buf);
int  osfs3_write_block(int fd, uint32_t block, const void *buf);

int  osfs3_read_bytes(int fd, uint64_t offset, void *buf, size_t len);
int  osfs3_write_bytes(int fd, uint64_t offset, const void *buf, size_t len);

/* Read superblock and validate magic/version */
int  osfs3_read_super(int fd, osfs3_super_t *sb);

/* Get device size in bytes */
uint64_t osfs3_device_size(int fd);

/* Aligned buffer allocation */
void *osfs3_alloc_block(void);
void  osfs3_free_block(void *buf);

/* UUID generation */
void osfs3_gen_uuid(uint8_t uuid[16]);

/* Formatted output helpers */
void osfs3_print_size(uint64_t bytes);

#endif /* OSITOFS3_COMMON_H */
