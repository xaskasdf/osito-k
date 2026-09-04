#ifndef OSITOFS_METADATA_H
#define OSITOFS_METADATA_H

#include "../include/types.h"

#define OSFS_FILE_TIME_CREATION  (1U << 0)
#define OSFS_FILE_TIME_ACCESS    (1U << 1)
#define OSFS_FILE_TIME_MODIFIED  (1U << 2)
#define OSFS_FILE_TIME_CHANGED   (1U << 3)
#define OSFS_FILE_TIME_MASK      ((1U << 4) - 1U)

#define OSFS_IO_PRESERVE_MTIME   (1U << 0)
#define OSFS_IO_PRESERVE_CTIME   (1U << 1)
#define OSFS_IO_FLAG_MASK        (OSFS_IO_PRESERVE_MTIME | \
                                  OSFS_IO_PRESERVE_CTIME)

#define OSFS_DOS_ATTR_READ_ONLY  0x01U
#define OSFS_DOS_ATTR_HIDDEN     0x02U
#define OSFS_DOS_ATTR_SYSTEM     0x04U
#define OSFS_DOS_ATTR_ARCHIVE    0x20U
#define OSFS_DOS_ATTR_MASK       (OSFS_DOS_ATTR_READ_ONLY | \
                                  OSFS_DOS_ATTR_HIDDEN | \
                                  OSFS_DOS_ATTR_SYSTEM | \
                                  OSFS_DOS_ATTR_ARCHIVE)

typedef struct {
    uint64_t creation;
    uint64_t access;
    uint64_t modified;
    uint64_t changed;
} osfs_file_times_t;

uint64_t osfs2_volume_id(void);
uint64_t osfs2_file_id(const void *file);
int osfs2_file_get_times(void *file, osfs_file_times_t *times);
int osfs2_file_set_times(void *file, uint32_t mask,
                         const osfs_file_times_t *times);
int osfs2_file_get_dos_attributes(const void *file, uint8_t *attributes);
int osfs2_file_set_dos_attributes(void *file, uint8_t attributes);

int osfs2_write_ex(void *file, uint64_t offset, const void *buf,
                   uint64_t len, uint32_t io_flags);
int osfs2_truncate_ex(void *file, uint64_t size, uint32_t io_flags);

#endif /* OSITOFS_METADATA_H */
