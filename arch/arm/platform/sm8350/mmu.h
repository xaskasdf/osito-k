/*
 * mmu.h -- SM8350 stage-1 MMU, caches and the uncached DMA window
 */
#ifndef SM8350_MMU_H
#define SM8350_MMU_H

#include "../../include/types.h"

/* DDR bank 0 is 0x80000000-0xB5000000 (device memory node); the low part
 * is firmware carve-outs. We use 0xA0000000 up: the shim loads the kernel
 * at 0xA0200000, the page allocator takes 0xA8000000 up, and the top 2 MB
 * are the uncached DMA window. */
#define RAM_CACHED_BASE     0xA0000000UL
#define RAM_END             0xB5000000UL
#define DMA_WINDOW_BASE     0xB4E00000UL
#define DMA_WINDOW_SIZE     0x00200000UL

/* Uncached memory for device DMA (zeroed, 64-byte aligned, never freed).
 * Physical == virtual, so the address is also what the device uses. */
void *dma_alloc(uint32_t bytes);

/* Clean the cached RAM to the point of coherency, turn the MMU and caches
 * off and branch to `entry` with x0 = 0: the entry state of a fresh boot
 * through the shim. Used by kload. */
void __attribute__((noreturn)) mmu_off_and_jump(uint64_t entry);

#endif
