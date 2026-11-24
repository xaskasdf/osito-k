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
extern uint32_t osfs2_file_capacity;
extern uint32_t osfs2_filetab_size;
extern uint32_t osfs2_crctab_offset;
extern uint32_t osfs2_layeridx_offset;
extern int osfs2_crc_table_enabled;
extern int osfs2_layer_index_enabled;

void osfs2_set_layout(uint32_t version);
void osfs2_set_layout_from_super(const osfs2_super_t *sb);

/* Paths shorter than 64 bytes live in name[]. Longer raw-file paths use a
 * stable short alias in name[] and store the full path in model_name[]. */
const char *osfs2_entry_name(const osfs2_file_t *file);
int osfs2_set_entry_name(osfs2_file_t *file, const char *name);

/* Block-aligned I/O (uses O_DIRECT internally) */
int  osfs2_open_device(const char *path, int readonly);
void osfs2_close_device(int fd);

int  osfs2_read_block(int fd, uint32_t block, void *buf);
int  osfs2_write_block(int fd, uint32_t block, const void *buf);

int  osfs2_read_bytes(int fd, uint64_t offset, void *buf, size_t len);
int  osfs2_write_bytes(int fd, uint64_t offset, const void *buf, size_t len);
int  osfs2_sync(int fd);

/* Metadata journal. recover returns 1 for a committed transaction when
 * repair is false, 0 for a clean/recovered journal, and -1 on corruption. */
int osfs2_journal_recover(int fd, int repair);
int osfs2_journal_commit_entries(
    int fd, uint32_t operation,
    const osfs2_super_t *before_super,
    const osfs2_super_t *after_super,
    const uint32_t slots[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t before[OSFS2_JOURNAL_MAX_ENTRIES],
    const osfs2_file_t after[OSFS2_JOURNAL_MAX_ENTRIES], uint32_t count);

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

/* Keep existing tool code layout-aware without duplicating conditionals in
 * every file-table loop and metadata I/O call. */
#undef OSFS2_MAX_FILES
#undef OSFS2_FILETAB_SIZE
#undef OSFS2_CRCTAB_OFF
#undef OSFS2_LAYERIDX_OFF
#define OSFS2_MAX_FILES   osfs2_file_capacity
#define OSFS2_FILETAB_SIZE osfs2_filetab_size
#define OSFS2_CRCTAB_OFF  osfs2_crctab_offset
#define OSFS2_LAYERIDX_OFF osfs2_layeridx_offset

#endif /* OSITOFS_COMMON_H */
