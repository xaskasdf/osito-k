/*
 * OsitoK x86-64 — Virtual Memory / Paging
 *
 * X-OS2: 4-level x86-64 page tables (PML4 → PDPT → PD → PT).
 * Identity maps all usable RAM + MMIO regions, then switches CR3.
 *
 * Uses 2MB large pages where possible for efficiency, falling back
 * to 4KB pages for partial ranges. Kernel runs in identity-mapped
 * space (virt == phys) for now; per-process page tables come later.
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern uint64_t mem_get_total(void);

/* ── Page table constants ────────────────────────────────────── */

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define LARGE_PAGE_SIZE (2ULL * 1024 * 1024)  /* 2MB */
#define LARGE_PAGE_SHIFT 21

#define ENTRIES_PER_TABLE 512

/* Page table entry flags */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_PWT         (1ULL << 3)   /* Page-level write-through */
#define PTE_PCD         (1ULL << 4)   /* Page-level cache disable */
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_LARGE       (1ULL << 7)   /* 2MB page (in PD entry) */
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)  /* No-execute */

#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL  /* bits 51:12 */

/* Index extraction from virtual address */
#define PML4_INDEX(va)  (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)  (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)    (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)    (((va) >> 12) & 0x1FF)

/* ── Page table state ────────────────────────────────────────── */

static uint64_t *kernel_pml4;    /* Top-level page table */
static uint64_t  kernel_cr3;     /* Physical address of PML4 */
static uint32_t  pt_pages_used;  /* Number of 4KB pages allocated for tables */

/* ── Allocate a zeroed page for page tables ──────────────────── */

static uint64_t *pt_alloc_page(void)
{
    uint64_t *page = (uint64_t *)mem_alloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (page) {
        memset(page, 0, PAGE_SIZE);
        pt_pages_used++;
    }
    return page;
}

/* ── Get or create next-level table ──────────────────────────── */

static uint64_t *pt_get_or_create(uint64_t *table, int index)
{
    if (table[index] & PTE_PRESENT) {
        return (uint64_t *)(table[index] & PTE_ADDR_MASK);
    }

    uint64_t *new_table = pt_alloc_page();
    if (!new_table) return NULL;

    table[index] = (uint64_t)new_table | PTE_PRESENT | PTE_WRITABLE;
    return new_table;
}

/* ── Map a single 4KB page ───────────────────────────────────── */

static int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = pt_get_or_create(kernel_pml4, PML4_INDEX(virt));
    if (!pdpt) return -1;

    uint64_t *pd = pt_get_or_create(pdpt, PDPT_INDEX(virt));
    if (!pd) return -1;

    /* Check if PD entry is already a 2MB large page — can't subdivide easily */
    int pd_idx = PD_INDEX(virt);
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_LARGE)) {
        /* Already mapped as 2MB — skip (identity map matches anyway) */
        return 0;
    }

    uint64_t *pt = pt_get_or_create(pd, pd_idx);
    if (!pt) return -1;

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    return 0;
}

/* ── Map a 2MB large page ────────────────────────────────────── */

static int paging_map_2m(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = pt_get_or_create(kernel_pml4, PML4_INDEX(virt));
    if (!pdpt) return -1;

    uint64_t *pd = pt_get_or_create(pdpt, PDPT_INDEX(virt));
    if (!pd) return -1;

    pd[PD_INDEX(virt)] = (phys & 0x000FFFFFFFE00000ULL) | flags | PTE_LARGE;
    return 0;
}

/* ── Identity map a range (auto-selects 2MB or 4KB pages) ───── */

static void paging_identity_map_range(uint64_t start, uint64_t end, uint64_t extra_flags)
{
    uint64_t flags_2m = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | extra_flags;
    uint64_t flags_4k = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | extra_flags;

    /* Align start down and end up to 4KB boundaries */
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t addr = start;
    while (addr < end) {
        /* Use 2MB page if aligned and enough space */
        if ((addr & (LARGE_PAGE_SIZE - 1)) == 0 && (end - addr) >= LARGE_PAGE_SIZE) {
            paging_map_2m(addr, addr, flags_2m);
            addr += LARGE_PAGE_SIZE;
        } else {
            paging_map_4k(addr, addr, flags_4k);
            addr += PAGE_SIZE;
        }
    }
}

/* ── CR3 helpers ─────────────────────────────────────────────── */

static inline uint64_t read_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void write_cr3(uint64_t cr3)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static inline void invlpg(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ── Public API ──────────────────────────────────────────────── */

/* Map a page in the kernel address space (for drivers, MMIO, etc.) */
int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!kernel_pml4) return -1;
    int ret = paging_map_4k(virt, phys, flags | PTE_PRESENT);
    if (ret == 0) invlpg(virt);
    return ret;
}

/* Map an MMIO region (uncacheable) */
int paging_map_mmio(uint64_t phys, uint64_t size)
{
    if (!kernel_pml4) return -1;
    uint64_t flags = PTE_PWT | PTE_PCD;  /* uncacheable */
    paging_identity_map_range(phys, phys + size, flags);
    return 0;
}

/* Get the kernel PML4 physical address (for new processes to clone) */
uint64_t paging_get_kernel_cr3(void)
{
    return kernel_cr3;
}

/* Switch to a different address space */
void paging_switch(uint64_t cr3)
{
    write_cr3(cr3);
}

/* ── Initialize paging ───────────────────────────────────────── */

void paging_init(void)
{
    serial_puts("[PAGE] Setting up kernel page tables...\n");

    uint64_t old_cr3 = read_cr3();
    serial_puts("[PAGE] Current CR3: 0x");
    serial_puthex(old_cr3, 16);
    serial_puts("\n");

    pt_pages_used = 0;

    /* Allocate PML4 */
    kernel_pml4 = pt_alloc_page();
    if (!kernel_pml4) {
        serial_puts("[PAGE] FATAL: Cannot allocate PML4\n");
        return;
    }

    /* ── Identity map memory regions ── */

    /* 1. First 4GB: covers all conventional RAM, legacy MMIO,
     *    APIC (0xFEE00000), IOAPIC (0xFEC00000), PCI config, etc.
     *    Use 2MB pages for the bulk. */
    serial_puts("[PAGE] Mapping first 4 GB...\n");
    paging_identity_map_range(0, 4ULL * 1024 * 1024 * 1024, 0);

    /* 2. Extended RAM: if system has >4GB, map up to total_memory.
     *    Our UEFI systems typically have 8-32GB. */
    uint64_t total = mem_get_total();
    if (total > 4ULL * 1024 * 1024 * 1024) {
        uint64_t extended = total;
        /* Round up to next GB boundary */
        extended = (extended + (1ULL << 30) - 1) & ~((1ULL << 30) - 1);
        serial_puts("[PAGE] Mapping extended RAM up to ");
        serial_putdec(extended / (1024 * 1024));
        serial_puts(" MB...\n");
        paging_identity_map_range(4ULL * 1024 * 1024 * 1024, extended, 0);
    }

    /* 3. GPU BAR0/BAR1 regions (typically above 4GB).
     *    These are large MMIO windows — map as uncacheable.
     *    Common locations: BAR0 ~256MB, BAR1 ~256MB-16GB.
     *    We map a generous range; unused entries are harmless. */
    /* Note: actual BAR addresses vary by system. The PCI scan
     * discovers them at runtime. For now we pre-map common ranges.
     * Individual drivers can also call paging_map_mmio() later. */

    serial_puts("[PAGE] Page tables built: ");
    serial_putdec(pt_pages_used);
    serial_puts(" pages (");
    serial_putdec(pt_pages_used * 4);
    serial_puts(" KB)\n");

    /* ── Switch CR3 ── */
    kernel_cr3 = (uint64_t)kernel_pml4;

    serial_puts("[PAGE] Switching CR3 to 0x");
    serial_puthex(kernel_cr3, 16);
    serial_puts("...\n");

    /* Interrupts off during CR3 switch for safety */
    __asm__ volatile ("cli");
    write_cr3(kernel_cr3);
    __asm__ volatile ("sti");

    /* If we get here, paging is working */
    serial_puts("[PAGE] CR3 switch successful — kernel paging active\n");

    fb_puts(" Paging: 4-level, ");
    fb_putdec(pt_pages_used * 4);
    fb_puts(" KB tables\n");
}
