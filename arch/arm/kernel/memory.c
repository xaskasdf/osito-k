/*
 * memory.c -- Physical page frame allocator for AArch64
 *
 * Bitmap allocator with 4KB page granularity.
 * Supports QEMU virt (fixed RAM) and SM8350 (platform-reported).
 * Identity-mapped: phys == virt.
 */

#include "../include/hal.h"
#include "../include/types.h"

/* ── Bitmap allocator ────────────────────────────────────── */

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12

/* Track up to 4GB of physical address space (enough for both platforms) */
#define MAX_PHYS_PAGES  (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)  /* 1M pages */
#define BITMAP_SIZE     (MAX_PHYS_PAGES / 8)                       /* 128 KB */

/* Bitmap: 1 = free, 0 = used/reserved */
static uint8_t page_bitmap[BITMAP_SIZE];

static uint64_t ram_base;
static uint64_t ram_pages;     /* total pages in RAM region */
static uint64_t free_pages;
static uint64_t total_memory;

/* ── Bitmap helpers ──────────────────────────────────────── */

static void bitmap_set(uint64_t page)
{
    if (page < MAX_PHYS_PAGES)
        page_bitmap[page / 8] |= (1 << (page % 8));
}

static void bitmap_clear(uint64_t page)
{
    if (page < MAX_PHYS_PAGES)
        page_bitmap[page / 8] &= ~(1 << (page % 8));
}

static int bitmap_test(uint64_t page)
{
    if (page >= MAX_PHYS_PAGES) return 0;
    return (page_bitmap[page / 8] >> (page % 8)) & 1;
}

/* ── Linker symbols ─────────────────────────────────────── */

extern char _stack_top[];

/* ── Initialize from known RAM range ────────────────────── */

void mem_init(uint64_t base, uint64_t size)
{
    memset(page_bitmap, 0, sizeof(page_bitmap));
    ram_base = base;
    total_memory = size;
    ram_pages = size >> PAGE_SHIFT;
    free_pages = 0;

    /* Mark all RAM pages as free */
    uint64_t start_page = base >> PAGE_SHIFT;
    for (uint64_t i = 0; i < ram_pages && (start_page + i) < MAX_PHYS_PAGES; i++) {
        bitmap_set(start_page + i);
        free_pages++;
    }

    /* Reserve: base up through kernel image + stack */
    uint64_t kernel_end = (uint64_t)_stack_top;
    uint64_t res_start = base >> PAGE_SHIFT;
    uint64_t res_end = (kernel_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
    for (uint64_t p = res_start; p < res_end && p < MAX_PHYS_PAGES; p++) {
        if (bitmap_test(p)) {
            bitmap_clear(p);
            free_pages--;
        }
    }

    serial_puts("[MEM ] Initialized: ");
    serial_putdec(total_memory / (1024 * 1024));
    serial_puts(" MB total, ");
    serial_putdec(free_pages * PAGE_SIZE / (1024 * 1024));
    serial_puts(" MB free (");
    serial_putdec(free_pages);
    serial_puts(" pages)\n");
}

/* ── Reserve a range of physical pages ──────────────────── */

int mem_reserve_range(uint64_t phys, uint64_t count)
{
    uint64_t start_page = phys >> PAGE_SHIFT;
    uint64_t reserved = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t p = start_page + i;
        if (bitmap_test(p)) {
            bitmap_clear(p);
            free_pages--;
            reserved++;
        }
    }
    return (int)reserved;
}

/* ── Allocate contiguous physical pages (first-fit) ──────── */

void *mem_alloc_pages(uint64_t count)
{
    if (count == 0 || free_pages < count)
        return (void *)0;

    uint64_t start_page = ram_base >> PAGE_SHIFT;
    uint64_t end_page = start_page + ram_pages;

    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t p = start_page; p < end_page && p < MAX_PHYS_PAGES; p++) {
        if (bitmap_test(p)) {
            if (run_len == 0) run_start = p;
            run_len++;
            if (run_len == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_clear(run_start + i);
                    free_pages--;
                }
                void *addr = (void *)(run_start << PAGE_SHIFT);
                memset(addr, 0, count * PAGE_SIZE);
                return addr;
            }
        } else {
            run_len = 0;
        }
    }

    return (void *)0;
}

/* ── Allocate aligned pages (for page tables, DMA) ──────── */

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t align_pages = alignment >> PAGE_SHIFT;
    if (align_pages == 0) align_pages = 1;

    uint64_t start_page = ram_base >> PAGE_SHIFT;
    uint64_t end_page = start_page + ram_pages;

    for (uint64_t p = start_page; p < end_page && p < MAX_PHYS_PAGES; p++) {
        if (p % align_pages != 0) continue;

        uint64_t ok = 1;
        for (uint64_t i = 0; i < pages && ok; i++) {
            if (!bitmap_test(p + i)) ok = 0;
        }

        if (ok) {
            for (uint64_t i = 0; i < pages; i++) {
                bitmap_clear(p + i);
                free_pages--;
            }
            void *addr = (void *)(p << PAGE_SHIFT);
            memset(addr, 0, pages * PAGE_SIZE);
            return addr;
        }
    }

    return (void *)0;
}

/* ── Free pages ─────────────────────────────────────────── */

void mem_free_pages(void *addr, uint64_t count)
{
    uint64_t start_page = (uint64_t)addr >> PAGE_SHIFT;
    for (uint64_t i = 0; i < count; i++) {
        bitmap_set(start_page + i);
        free_pages++;
    }
}

/* ── Info ────────────────────────────────────────────────── */

uint64_t mem_get_total(void)
{
    return total_memory;
}

uint64_t mem_get_free(void)
{
    return free_pages * PAGE_SIZE;
}

uint64_t mem_get_used(void)
{
    return (ram_pages - free_pages) * PAGE_SIZE;
}
