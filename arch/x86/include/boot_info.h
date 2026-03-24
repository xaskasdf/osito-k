#ifndef BOOT_INFO_H
#define BOOT_INFO_H

/* Use kernel types.h when available, otherwise stdint.h (for boot.efi) */
#include "types.h"

#define BOOT_INFO_MAGIC   0x4F53544B  /* 'OSTK' */
#define BOOT_INFO_VERSION 2

#define BOOT_MAX_DISPLAY_MODES 16

/* One GOP mode entry — filled by bootloader, read by display subsystem */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;          /* pixels per scanline */
    uint32_t pixel_format;   /* 0 = RGBX, 1 = BGRX */
} boot_display_mode_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* Framebuffer (GOP) — active/selected mode */
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

    /* Display mode enumeration (v2) — placeholders for multi-monitor */
    uint32_t display_mode_count;
    uint32_t display_current_mode;   /* index into display_modes[] */
    boot_display_mode_t display_modes[BOOT_MAX_DISPLAY_MODES];
} boot_info_t;

#endif /* BOOT_INFO_H */
