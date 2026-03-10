#ifndef BOOT_INFO_H
#define BOOT_INFO_H

/* Use kernel types.h when available, otherwise stdint.h (for boot.efi) */
#include "types.h"

#define BOOT_INFO_MAGIC   0x4F53544B  /* 'OSTK' */
#define BOOT_INFO_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* Framebuffer (GOP) */
    uint64_t fb_base;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;       /* pixels per scanline */

    /* UEFI memory map */
    uint64_t mmap_addr;
    uint64_t mmap_size;
    uint64_t mmap_desc_size;
    uint32_t mmap_desc_ver;
    uint32_t _pad0;

    /* ACPI */
    uint64_t acpi_rsdp;

    /* Kernel location (so kernel can reserve its own pages) */
    uint64_t kernel_phys_base;
    uint64_t kernel_size;
} boot_info_t;

#endif /* BOOT_INFO_H */
