#ifndef OSITO_BOOT_KERNEL_H
#define OSITO_BOOT_KERNEL_H
#include "../include/types.h"

#define BOOT_KERNEL_MAX (32U * 1024 * 1024)

/* Byte I/O on one pinned disk; never changes the filesystem's active device. */
typedef struct {
    void *context;
    uint32_t sector_size;
    uint64_t sectors;
    int (*read)(void *, uint64_t, void *, uint64_t);
    int (*write)(void *, uint64_t, const void *, uint64_t);
    int (*flush)(void *);
} boot_kernel_io_t;
typedef struct boot_kernel boot_kernel_t;

/* Requires a unique GPT ESP alongside the mounted data partition. */
const char *boot_kernel_open(const boot_kernel_io_t *, uint64_t data_offset,
                             boot_kernel_t **);
uint32_t boot_kernel_size(const boot_kernel_t *);
const char *boot_kernel_read(boot_kernel_t *, void *);
const char *boot_kernel_replace(boot_kernel_t *, const void *, uint32_t);
void boot_kernel_close(boot_kernel_t *);
#endif
