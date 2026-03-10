/*
 * OsitoK x86-64 — Physical Memory Manager
 *
 * Bitmap allocator over UEFI memory map.
 * 4KB page granularity. Identity-mapped (phys == virt).
 */

#include "../include/types.h"

/* ── Declarations ────────────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);

/* ── UEFI Memory Descriptor (matches EFI spec) ──────────────── */

#define EFI_CONVENTIONAL_MEMORY   7
#define EFI_BOOT_SERVICES_CODE    3
#define EFI_BOOT_SERVICES_DATA    4

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

/* ── Bitmap allocator ────────────────────────────────────────── */

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define MAX_PHYS_PAGES  (16ULL * 1024 * 1024 * 1024 / PAGE_SIZE) /* Up to 16GB tracked */
#define BITMAP_SIZE     (MAX_PHYS_PAGES / 8)  /* 512KB for 16GB */

/* Bitmap: 1 = free, 0 = used/reserved */
static uint8_t page_bitmap[BITMAP_SIZE];

static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t total_memory;

/* ── Bitmap helpers ──────────────────────────────────────────── */

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

/* ── Initialize from UEFI memory map ────────────────────────── */

void mem_init(void *mmap, uint64_t mmap_size, uint64_t desc_size)
{
    /* Start with everything reserved */
    memset(page_bitmap, 0, sizeof(page_bitmap));
    total_pages = 0;
    free_pages = 0;
    total_memory = 0;

    /* Walk the UEFI memory map */
    uint64_t entries = mmap_size / desc_size;
    uint8_t *ptr = (uint8_t *)mmap;

    uint64_t usable_regions = 0;

    for (uint64_t i = 0; i < entries; i++) {
        efi_memory_descriptor_t *desc = (efi_memory_descriptor_t *)(ptr + i * desc_size);

        total_memory += desc->number_of_pages * PAGE_SIZE;

        /* Only use conventional memory and freed boot services memory */
        if (desc->type == EFI_CONVENTIONAL_MEMORY ||
            desc->type == EFI_BOOT_SERVICES_CODE ||
            desc->type == EFI_BOOT_SERVICES_DATA) {

            uint64_t start_page = desc->physical_start >> PAGE_SHIFT;
            uint64_t num_pages = desc->number_of_pages;

            /* Skip first 1MB (BIOS, legacy, etc.) */
            if (desc->physical_start < 0x100000) {
                uint64_t skip = (0x100000 - desc->physical_start) >> PAGE_SHIFT;
                if (skip >= num_pages) continue;
                start_page += skip;
                num_pages -= skip;
            }

            /* Mark pages as free */
            for (uint64_t p = 0; p < num_pages && (start_page + p) < MAX_PHYS_PAGES; p++) {
                bitmap_set(start_page + p);
                free_pages++;
            }
            total_pages += num_pages;
            usable_regions++;
        }
    }

    serial_puts("[MEM] Memory manager initialized\n");
    serial_puts("[MEM] Total: ");
    serial_putdec(total_memory / (1024 * 1024));
    serial_puts(" MB, Usable: ");
    serial_putdec(free_pages * PAGE_SIZE / (1024 * 1024));
    serial_puts(" MB (");
    serial_putdec(free_pages);
    serial_puts(" pages), Regions: ");
    serial_putdec(usable_regions);
    serial_puts("\n");

    fb_puts("\n Memory: ");
    fb_putdec(total_memory / (1024 * 1024));
    fb_puts(" MB total, ");
    fb_putdec(free_pages * PAGE_SIZE / (1024 * 1024));
    fb_puts(" MB usable\n");
}

/* ── Reserve kernel pages (prevent allocator from handing them out) ── */

void mem_reserve_kernel(uint64_t phys_base, uint64_t size)
{
    uint64_t start_page = phys_base >> PAGE_SHIFT;
    uint64_t num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;

    uint64_t reserved = 0;
    for (uint64_t p = 0; p < num_pages && (start_page + p) < MAX_PHYS_PAGES; p++) {
        if (bitmap_test(start_page + p)) {
            bitmap_clear(start_page + p);
            free_pages--;
            reserved++;
        }
    }

    serial_puts("[MEM] Reserved kernel region: 0x");
    serial_puthex(phys_base, 16);
    serial_puts(" (");
    serial_putdec(reserved);
    serial_puts(" pages)\n");
}

/* ── Allocate physical pages ─────────────────────────────────── */

void *mem_alloc_pages(uint64_t count)
{
    if (count == 0 || free_pages < count) return NULL;

    /* Simple first-fit search */
    uint64_t run_start = 0;
    uint64_t run_len = 0;

    /* Start above 8MB to avoid collisions with:
     * - Page tables (0x100000-0x110000)
     * - Heap (0x10F000+)
     * - ET_EXEC ELF load area (typically 0x400000-0x600000)
     * This ensures first-fit doesn't consume the ELF VA range. */
    for (uint64_t p = 2048; p < MAX_PHYS_PAGES; p++) {  /* Start above 8MB */
        if (bitmap_test(p)) {
            if (run_len == 0) run_start = p;
            run_len++;
            if (run_len == count) {
                /* Found a contiguous run */
                for (uint64_t i = 0; i < count; i++) {
                    bitmap_clear(run_start + i);
                    free_pages--;
                }
                return (void *)(run_start << PAGE_SHIFT);
            }
        } else {
            run_len = 0;
        }
    }

    return NULL; /* Out of contiguous pages */
}

/* ── Reserve specific physical pages (for ET_EXEC fixed loads) ─ */

int mem_reserve_range(uint64_t phys, uint64_t count)
{
    uint64_t start_page = phys >> PAGE_SHIFT;

    /* Check all pages are free first */
    for (uint64_t i = 0; i < count; i++) {
        if (!bitmap_test(start_page + i)) {
            serial_puts("[MEM] CONFLICT: page 0x");
            serial_puthex((start_page + i) << PAGE_SHIFT, 16);
            serial_puts(" already allocated\n");
            return -1;
        }
    }

    /* Mark as used */
    for (uint64_t i = 0; i < count; i++) {
        bitmap_clear(start_page + i);
        free_pages--;
    }

    return 0;
}

/* ── Free physical pages ─────────────────────────────────────── */

void mem_free_pages(void *addr, uint64_t count)
{
    uint64_t start_page = (uint64_t)addr >> PAGE_SHIFT;
    for (uint64_t i = 0; i < count; i++) {
        bitmap_set(start_page + i);
        free_pages++;
    }
}

/* ── Allocate aligned buffer (for DMA, NVMe queues, etc.) ───── */

void *mem_alloc_aligned(uint64_t size, uint64_t alignment)
{
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t align_pages = alignment >> PAGE_SHIFT;
    if (align_pages == 0) align_pages = 1;

    /* Search for aligned contiguous pages (above 8MB, skip ELF load area) */
    for (uint64_t p = 2048; p < MAX_PHYS_PAGES; p++) {
        /* Align to required boundary */
        if (p % align_pages != 0) continue;

        /* Check if enough contiguous pages */
        uint64_t ok = 1;
        for (uint64_t i = 0; i < pages && ok; i++) {
            if (!bitmap_test(p + i)) ok = 0;
        }

        if (ok) {
            for (uint64_t i = 0; i < pages; i++) {
                bitmap_clear(p + i);
                free_pages--;
            }
            return (void *)(p << PAGE_SHIFT);
        }
    }

    return NULL;
}

/* ── Info ────────────────────────────────────────────────────── */

uint64_t mem_get_free(void)
{
    return free_pages * PAGE_SIZE;
}

uint64_t mem_get_total(void)
{
    return total_memory;
}

uint64_t mem_get_used(void)
{
    return (total_pages - free_pages) * PAGE_SIZE;
}

/* ── libc stubs (no linked libc — needed by kernel code + TCC codegen) ── */

int strncmp(const char *a, const char *b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/* TCC generates memmove calls for struct assignments (GCC inlines them) */
void *memmove(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (uint64_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

/* TCC soft-float/soft-int helpers (GCC uses libgcc or inlines these).
 * These convert between uint64_t and float/double without using float
 * intermediates (which would recursively call the same helpers). */

float __floatundisf(uint64_t a)
{
    /* uint64_t → float: construct IEEE 754 float via integer math */
    if (a == 0) return 0.0f;
    /* If fits in int32, use hardware conversion */
    if (a <= 0x7FFFFFUL) return (float)(uint32_t)a;
    /* Find highest set bit (manual clz) */
    int shift = 0;
    uint64_t tmp = a;
    while (tmp > 0xFFFFFF) { tmp >>= 1; shift++; }
    /* Round: check the bits we're about to discard */
    if (shift > 0 && (a & ((1ULL << (shift - 1))))) tmp++;
    return (float)(uint32_t)tmp * (float)(1ULL << shift);
}

double __floatundidf(uint64_t a)
{
    /* uint64_t → double */
    if (a == 0) return 0.0;
    if (a <= 0x1FFFFFFFFFFFFFULL) return (double)(int64_t)a; /* fits in 53 mantissa bits */
    /* Split into high and low 32-bit parts */
    uint32_t hi = (uint32_t)(a >> 32);
    uint32_t lo = (uint32_t)a;
    return (double)hi * 4294967296.0 + (double)lo;
}

uint64_t __fixunssfdi(float a)
{
    /* float → uint64_t */
    if (a <= 0.0f) return 0;
    /* Extract via integer bit manipulation */
    union { float f; uint32_t u; } u = { .f = a };
    uint32_t exp = (u.u >> 23) & 0xFF;
    uint32_t mant = (u.u & 0x7FFFFF) | 0x800000; /* add implicit 1 */
    if (exp < 127) return 0;
    int shift = (int)exp - 127 - 23;
    if (shift >= 40) return ~0ULL; /* overflow */
    if (shift >= 0) return (uint64_t)mant << shift;
    return (uint64_t)mant >> (-shift);
}

uint64_t __fixunsdfdi(double a)
{
    /* double → uint64_t */
    if (a <= 0.0) return 0;
    union { double d; uint64_t u; } u = { .d = a };
    uint32_t exp = (u.u >> 52) & 0x7FF;
    uint64_t mant = (u.u & 0xFFFFFFFFFFFFFULL) | 0x10000000000000ULL;
    if (exp < 1023) return 0;
    int shift = (int)exp - 1023 - 52;
    if (shift >= 11) return ~0ULL;
    if (shift >= 0) return mant << shift;
    return mant >> (-shift);
}

/* TCC doesn't support __builtin_unreachable — provide as infinite loop */
void __builtin_unreachable(void)
{
    for (;;) __asm__ volatile("hlt");
}

/* proc_set_qos — stub (full implementation in scheduler, not yet linked) */
void proc_set_qos(uint8_t qos)
{
    (void)qos;
}
