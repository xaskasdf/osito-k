/*
 * OsitoK x86-64 — Physical Memory Manager
 *
 * Bitmap allocator over UEFI memory map.
 * 4KB page granularity. Identity-mapped (phys == virt).
 */

#include "../include/types.h"
#include "../include/sys_caps.h"

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
#define MAX_PHYS_PAGES  (64ULL * 1024 * 1024 * 1024 / PAGE_SIZE) /* Up to 64GB tracked */
#define BITMAP_SIZE     (MAX_PHYS_PAGES / 8)  /* 2MB for 64GB */

/* Bitmap: 1 = free, 0 = used/reserved */
static uint8_t page_bitmap[BITMAP_SIZE];

static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t total_memory;
static uint64_t max_tracked_page;  /* highest page ever marked free */

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
            /* Track highest page for search bounds */
            uint64_t last = start_page + num_pages;
            if (last > max_tracked_page) max_tracked_page = last;
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

    /* Start above 16MB to avoid collisions with:
     * - Page tables (0x100000-0x110000)
     * - ET_EXEC ELF load area (0x400000+, up to ~14MB for large BSS)
     *   e.g. Quake 2 has a 9.5MB BSS reaching 0xDF0000 (~14MB)
     * Heap/general allocator must not overlap the fixed-load VA range. */
    uint64_t limit = max_tracked_page ? max_tracked_page : MAX_PHYS_PAGES;
    for (uint64_t p = 4096; p < limit; p++) {  /* Start above 16MB */
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
    static int dfree_warned = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (bitmap_test(start_page + i)) {
            /* Page was already free — double-free of a phys page.
             * This is the smoking gun for "heap arena got reused
             * while the original holder still had a pointer". Warn
             * loudly the first few times so the caller can be found,
             * but DO NOT increment free_pages or set the bit again
             * (would corrupt the page accounting). */
            if (dfree_warned < 8) {
                dfree_warned++;
                serial_puts("[MEM] !!! DOUBLE PAGE-FREE phys=0x");
                serial_puthex((start_page + i) << PAGE_SHIFT, 16);
                serial_puts(" caller=0x");
                serial_puthex((uint64_t)__builtin_return_address(0), 16);
                serial_puts("\n");
            }
            continue;
        }
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

    if (free_pages < pages) return NULL;

    /* Search for aligned contiguous pages (above 16MB, clear of ELF load area) */
    uint64_t limit = max_tracked_page ? max_tracked_page : MAX_PHYS_PAGES;
    uint64_t p = 4096;
    /* Snap to first aligned candidate */
    if (p % align_pages != 0)
        p = ((p / align_pages) + 1) * align_pages;

    while (p + pages <= limit) {
        /* Check if enough contiguous pages starting at p */
        uint64_t ok = 1;
        uint64_t i;
        for (i = 0; i < pages; i++) {
            if (!bitmap_test(p + i)) { ok = 0; break; }
        }

        if (ok) {
            for (i = 0; i < pages; i++) {
                bitmap_clear(p + i);
                free_pages--;
            }
            return (void *)(p << PAGE_SHIFT);
        }

        /* Skip past the failed page to the next aligned candidate */
        uint64_t fail = p + i + 1;
        if (fail % align_pages != 0)
            fail = ((fail / align_pages) + 1) * align_pages;
        p = fail;
    }

    return NULL;
}

/* ── High-memory allocator (for kernel structures) ────────────────
 * Allocates from the top of available memory downward. Used by
 * pt_alloc_page to keep kernel page tables out of the low-memory
 * range (0x400000..0x10000000) where ET_EXEC binaries typically
 * load. Without this, loading a binary whose vaddr overlaps the
 * kernel's PML4 physical address corrupts the page tables. */
void *mem_alloc_aligned_high(uint64_t size, uint64_t alignment)
{
    uint64_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t align_pages = alignment >> PAGE_SHIFT;
    if (align_pages == 0) align_pages = 1;

    if (free_pages < pages) return NULL;

    uint64_t limit = max_tracked_page ? max_tracked_page : MAX_PHYS_PAGES;
    /* Start from the top and work down */
    uint64_t p = (limit - pages) & ~(align_pages - 1);

    while (p >= 4096) {
        uint64_t ok = 1;
        uint64_t i;
        for (i = 0; i < pages; i++) {
            if (!bitmap_test(p + i)) { ok = 0; break; }
        }

        if (ok) {
            for (i = 0; i < pages; i++) {
                bitmap_clear(p + i);
                free_pages--;
            }
            return (void *)(p << PAGE_SHIFT);
        }

        /* Move down past the failed page */
        if (p < align_pages) break;
        p -= align_pages;
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

/* Highest physical address that may contain usable RAM.
 * Used by paging_init to size the identity + upper-half maps. */
uint64_t mem_get_highest_address(void)
{
    return max_tracked_page * PAGE_SIZE;
}

uint64_t mem_get_used(void)
{
    return (total_pages - free_pages) * PAGE_SIZE;
}

/* ── System capabilities (hardware-derived resource limits) ──────── */

#define CLAMP(val, lo, hi)  ((val) < (lo) ? (lo) : ((val) > (hi) ? (hi) : (val)))

sys_caps_t g_sys_caps;

void sys_caps_init(void)
{
    uint64_t total = total_memory;
    uint64_t avail = free_pages * PAGE_SIZE;
    uint64_t mb    = total / (1024 * 1024);

    g_sys_caps.total_ram     = total;
    g_sys_caps.available_ram = avail;
    g_sys_caps.page_count    = (uint32_t)total_pages;
    g_sys_caps.cpu_count     = 1;  /* updated by SMP init later */

    /* Kernel heap: scale with RAM, 256KB min, 16MB max */
    g_sys_caps.heap_init_size = CLAMP(total / 256, 256 * 1024, 16ULL * 1024 * 1024);
    g_sys_caps.heap_grow_size = CLAMP(total / 1024, 64 * 1024, 4ULL * 1024 * 1024);

    /* Per-process limits */
    g_sys_caps.brk_heap_size   = CLAMP(total / 16, 16ULL * 1024 * 1024, 64ULL * 1024 * 1024);
    g_sys_caps.user_stack_size = CLAMP(total / 64, 1ULL * 1024 * 1024, 8ULL * 1024 * 1024);
    g_sys_caps.max_processes   = (uint32_t)CLAMP(mb / 16, 4, 256);
    g_sys_caps.max_fds_global  = (uint32_t)CLAMP(total / (256 * 1024), 128, 4096);

    /* ELF loader */
    g_sys_caps.elf_max_size  = CLAMP(avail * 3 / 4, 64ULL * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    g_sys_caps.elf_max_alloc = avail / 2;

    /* Win32 */
    g_sys_caps.win32_heap_size = CLAMP(total / 4, 32ULL * 1024 * 1024, 512ULL * 1024 * 1024);
    g_sys_caps.crt_pool_size   = CLAMP(total / 8, 8ULL * 1024 * 1024, 256ULL * 1024 * 1024);
    g_sys_caps.win32_va_limit  = 0x7FFF0000ULL;

    /* Networking */
    g_sys_caps.tcp_max_conns = (uint32_t)CLAMP(mb / 4, 8, 256);

    serial_puts("[CAPS] System capabilities (from ");
    serial_putdec(mb);
    serial_puts(" MB RAM):\n");
    serial_puts("[CAPS]   heap_init=");  serial_putdec(g_sys_caps.heap_init_size / 1024); serial_puts("KB");
    serial_puts("  brk=");   serial_putdec(g_sys_caps.brk_heap_size / (1024*1024)); serial_puts("MB");
    serial_puts("  stack="); serial_putdec(g_sys_caps.user_stack_size / (1024*1024)); serial_puts("MB");
    serial_puts("  procs="); serial_putdec(g_sys_caps.max_processes);
    serial_puts("  elf_max="); serial_putdec(g_sys_caps.elf_max_size / (1024*1024)); serial_puts("MB");
    serial_puts("\n");
}

int sys_caps_check_alloc(uint64_t bytes, const char *what)
{
    uint64_t avail = free_pages * PAGE_SIZE;
    if (bytes <= avail) return 1;  /* OK */

    serial_puts("[CAPS] ");
    serial_puts(what);
    serial_puts(": need ");
    serial_putdec(bytes / (1024 * 1024));
    serial_puts(" MB, available ");
    serial_putdec(avail / (1024 * 1024));
    serial_puts(" MB — INSUFFICIENT\n");
    serial_puts("       Tip: free processes with 'kill' or add more RAM\n");
    return 0;  /* not enough */
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

#ifdef __TCC__
/* TCC doesn't support __builtin_unreachable — provide as infinite loop */
void __builtin_unreachable(void)
{
    for (;;) __asm__ volatile("hlt");
}
#endif

extern int sched_set_qos(uint32_t pid, uint8_t qos);
void proc_set_qos(uint8_t qos)
{
    sched_set_qos(0, qos);  /* 0 = current process */
}
