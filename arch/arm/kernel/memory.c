/*
 * memory.c -- Physical page frame allocator for AArch64
 *
 * Bitmap allocator with 4KB page granularity.
 * Supports QEMU virt (fixed RAM) and SM8350 (platform-reported).
 * Identity-mapped: phys == virt.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "../include/spinlock.h"

/* Tasks on several CPUs allocate; the zeroing happens outside the lock */
static spinlock_t mem_lock;

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

/* High memory, for big allocations (mem_alloc_aligned_high): the RAM the
 * platform maps beyond the first region and hands over with mem_add_high,
 * on SM8350 the rest of its 16 GB. One bitmap for all of it, each region
 * owning a stretch from its first_bit. mem_alloc_pages never uses it, so
 * kload's kernels stay in the first region. */
#define HIGH_MAX_PAGES  (16ULL * 1024 * 1024 * 1024 / PAGE_SIZE)  /* 4M pages */
#define HIGH_REGIONS    8
static uint8_t high_bitmap[HIGH_MAX_PAGES / 8];                    /* 512 KB */
static struct {
    uint64_t base, pages, first_bit;
} high[HIGH_REGIONS];
static int high_count;
static uint64_t high_pages, high_free;

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

extern char _start[], _stack_top[];

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

    /* Reserve the running kernel: image, BSS, stack. Only that: a kernel
     * kload placed inside this range finds below itself what its
     * predecessor left, which nothing uses any more (the DMA window is
     * reserved apart, by mmu.c). Reserving from the base up to it shrank
     * the free memory with every kload in a row. */
    uint64_t kernel_end = (uint64_t)_stack_top;
    uint64_t res_start = (uint64_t)_start >> PAGE_SHIFT;
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
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t p = start_page + i;
        if (bitmap_test(p)) {
            bitmap_clear(p);
            free_pages--;
            reserved++;
        }
    }
    spin_unlock_irqrestore(&mem_lock, flags);
    return (int)reserved;
}

/* ── Allocate contiguous physical pages (first-fit) ──────── */

void *mem_alloc_pages(uint64_t count)
{
    if (count == 0)
        return (void *)0;

    uint64_t start_page = ram_base >> PAGE_SHIFT;
    uint64_t end_page = start_page + ram_pages;

    uint64_t run_start = 0;
    uint64_t run_len = 0;
    void *addr = (void *)0;

    uint64_t flags = spin_lock_irqsave(&mem_lock);
    for (uint64_t p = start_page; p < end_page && p < MAX_PHYS_PAGES &&
                                  free_pages >= count; p++) {
        if (bitmap_test(p)) {
            if (run_len == 0) run_start = p;
            run_len++;
            if (run_len == count) {
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_clear(run_start + i);
                    free_pages--;
                }
                addr = (void *)(run_start << PAGE_SHIFT);
                break;
            }
        } else {
            run_len = 0;
        }
    }
    spin_unlock_irqrestore(&mem_lock, flags);

    if (addr)
        memset(addr, 0, count * PAGE_SIZE);
    return addr;
}

/* ── Allocate aligned pages (for page tables, DMA) ──────── */

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t align_pages = alignment >> PAGE_SHIFT;
    if (align_pages == 0) align_pages = 1;

    uint64_t start_page = ram_base >> PAGE_SHIFT;
    uint64_t end_page = start_page + ram_pages;
    void *addr = (void *)0;

    uint64_t flags = spin_lock_irqsave(&mem_lock);
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
            addr = (void *)(p << PAGE_SHIFT);
            break;
        }
    }
    spin_unlock_irqrestore(&mem_lock, flags);

    /* Skip memset for large allocs (>64KB) — too slow on QEMU TCG */
    if (addr && pages <= 16)
        memset(addr, 0, pages * PAGE_SIZE);
    return addr;
}

/* ── The high region ─────────────────────────────────────── */

static int high_test(uint64_t i) { return (high_bitmap[i / 8] >> (i % 8)) & 1; }
static void high_set(uint64_t i) { high_bitmap[i / 8] |= (uint8_t)(1 << (i % 8)); }
static void high_clear(uint64_t i) { high_bitmap[i / 8] &= (uint8_t)~(1 << (i % 8)); }

/* Hand over a mapped region (all of them before any high allocation) */
void mem_add_high(uint64_t base, uint64_t size)
{
    uint64_t pages = size >> PAGE_SHIFT;
    if (high_count == HIGH_REGIONS || pages > HIGH_MAX_PAGES - high_pages)
        return;
    uint64_t first = high_pages;
    for (uint64_t i = 0; i < pages; i++)
        high_set(first + i);
    high[high_count].base = base;
    high[high_count].pages = pages;
    high[high_count].first_bit = first;
    high_count++;
    high_pages += pages;
    high_free += pages;
    serial_puts("[MEM ] High region: ");
    serial_putdec(pages * PAGE_SIZE >> 20);
    serial_puts(" MB at ");
    serial_puthex(base, 10);
    serial_puts("\n");
}

/* A run of pages in region r at an aligned address: its first page's
 * index within the region, or -1. Caller holds mem_lock. */
static int64_t high_find(int r, uint64_t pages, uint64_t step)
{
    uint64_t base_page = high[r].base >> PAGE_SHIFT, bit = high[r].first_bit;
    uint64_t first = (base_page + step - 1) / step * step - base_page;
    for (uint64_t p = first; p + pages <= high[r].pages; ) {
        uint64_t i = 0;
        while (i < pages && high_test(bit + p + i))
            i++;
        if (i == pages)
            return (int64_t)p;
        uint64_t next = p + i + 1;              /* past the used page */
        p = first + (next - first + step - 1) / step * step;
    }
    return -1;
}

/* Big, aligned, physically contiguous memory (models, tensor arenas): from
 * the high regions, else the first one. Not zeroed. */
void *mem_alloc_aligned_high(uint64_t size, uint64_t alignment)
{
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t step = alignment >> PAGE_SHIFT;
    if (step == 0)
        step = 1;
    void *addr = (void *)0;
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    for (int r = 0; r < high_count && pages && pages <= high_free && !addr; r++) {
        int64_t p = high_find(r, pages, step);
        if (p < 0)
            continue;
        for (uint64_t i = 0; i < pages; i++)
            high_clear(high[r].first_bit + (uint64_t)p + i);
        high_free -= pages;
        addr = (void *)(high[r].base + ((uint64_t)p << PAGE_SHIFT));
    }
    spin_unlock_irqrestore(&mem_lock, flags);
    return addr ? addr : mem_alloc_aligned(size, alignment);
}

/* ── Free pages ─────────────────────────────────────────── */

void mem_free_pages(void *addr, uint64_t count)
{
    uint64_t pa = (uint64_t)addr;
    uint64_t flags = spin_lock_irqsave(&mem_lock);
    int r = 0;
    while (r < high_count &&
           !(pa >= high[r].base && pa < high[r].base + (high[r].pages << PAGE_SHIFT)))
        r++;
    if (r < high_count) {
        uint64_t first = (pa - high[r].base) >> PAGE_SHIFT;
        for (uint64_t i = 0; i < count && first + i < high[r].pages; i++) {
            uint64_t bit = high[r].first_bit + first + i;
            if (!high_test(bit)) {
                high_set(bit);
                high_free++;
            }
        }
    } else {
        uint64_t start_page = pa >> PAGE_SHIFT;
        for (uint64_t i = 0; i < count; i++) {
            bitmap_set(start_page + i);
            free_pages++;
        }
    }
    spin_unlock_irqrestore(&mem_lock, flags);
}

/* ── Info (both regions) ─────────────────────────────────── */

uint64_t mem_get_total(void)
{
    return total_memory + (high_pages << PAGE_SHIFT);
}

uint64_t mem_get_free(void)
{
    return (free_pages + high_free) * PAGE_SIZE;
}

uint64_t mem_get_used(void)
{
    return (ram_pages - free_pages + high_pages - high_free) * PAGE_SIZE;
}
