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
#include "../include/paging.h"
#include "smp.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_putdec(uint64_t val);
extern void fb_puthex(uint64_t val, int digits);

extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);
extern int   mem_reserve_range(uint64_t phys, uint64_t count);
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
#define PTE_COW         (1ULL << 9)   /* Copy-on-write (software bit, AVL) */
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
static spinlock_t paging_lock = SPINLOCK_INIT;

static inline uint64_t paging_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&paging_lock);
    return flags;
}

static inline void paging_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&paging_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

/* EFER.NXE must be enabled before any PTE uses bit 63 as NX.
 * Otherwise the CPU treats bit 63 as reserved and raises #PF.RSVD
 * on otherwise-valid non-executable mappings. */
#define MSR_EFER        0xC0000080u
#define EFER_NXE        (1ULL << 11)

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr" : : "c"(msr),
                      "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static void paging_enable_nxe(void)
{
    uint64_t efer = rdmsr(MSR_EFER);
    if (!(efer & EFER_NXE)) {
        wrmsr(MSR_EFER, efer | EFER_NXE);
        serial_puts("[PAGE] EFER.NXE enabled for NX page mappings\n");
    }
}

/* ── Allocate a zeroed page for page tables ──────────────────── */

extern void *mem_alloc_aligned_high(uint64_t size, uint64_t alignment);
extern void *mem_alloc_aligned_high_below4g(uint64_t size, uint64_t alignment);

/* Allocate a fresh page-table page and return an UPPER-HALF (virt)
 * pointer to it. The hardware always sees the physical address (stored
 * via VIRT_TO_PHYS in parent PTEs); kernel C code reads/writes the
 * page through the upper-half mirror so it does not depend on the
 * lower-half identity map. */
static uint64_t *pt_alloc_page(void)
{
    /* Page-table pages MUST be physically isolated from the bottom-up general
     * pool (mem_alloc_pages/mem_alloc_aligned) that backs the Win32 heap and PE
     * images. Otherwise a guest HeapAlloc block ends up PHYSICALLY ADJACENT to a
     * page-table page, and a guest heap overrun overwrites the PTEs — silently
     * unmapping a 2 MB swath of Core.dll (the UT99 "Preferences crash": PTEs at
     * PT page 0x01C9D000 found overwritten with guest pointers, PE-PTE=0 i.e. no
     * paging-API call). So allocate page tables TOP-DOWN so they cluster near
     * the top of RAM, far from the bottom-up heap/PE pool.
     * MUST stay < 4 GB: the AP trampoline loads the PML4 base into CR3 with a
     * 32-bit `mov` in protected mode (before long mode), so a >= 4 GB page-table
     * physical address is truncated and faults the AP — the SMP "Starting AP N"
     * hang on >4 GB RAM configs. mem_alloc_aligned_high_below4g caps the
     * top-down search just under 4 GB. */
    void *phys = mem_alloc_aligned_high_below4g(PAGE_SIZE, PAGE_SIZE);
    if (!phys)
        phys = mem_alloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!phys)
        return NULL;
    uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(phys);
    memset(virt, 0, PAGE_SIZE);
    pt_pages_used++;
    return virt;
}

/* ── Get or create next-level table ──────────────────────────── */

static uint64_t *pt_get_or_create(uint64_t *table, int index)
{
    if (table[index] & PTE_PRESENT) {
        return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
    }

    uint64_t *new_table = pt_alloc_page();
    if (!new_table) return NULL;

    table[index] = (uint64_t)VIRT_TO_PHYS(new_table) | PTE_PRESENT | PTE_WRITABLE;
    return new_table;
}

/* ── Map a single 4KB page ───────────────────────────────────── */

static int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq_flags = paging_lock_irqsave();
    int result = -1;
    uint64_t *pdpt = pt_get_or_create(kernel_pml4, PML4_INDEX(virt));
    if (!pdpt) goto out;

    uint64_t *pd = pt_get_or_create(pdpt, PDPT_INDEX(virt));
    if (!pd) goto out;

    /* If PD entry is a 2MB large page, split into 512 × 4KB pages */
    int pd_idx = PD_INDEX(virt);
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_LARGE)) {
        uint64_t large_phys = pd[pd_idx] & 0x000FFFFFFFE00000ULL;
        uint64_t large_flags = pd[pd_idx] & ~(PTE_ADDR_MASK | PTE_LARGE);
        uint64_t *pt = pt_alloc_page();
        if (!pt) {
            serial_puts("[paging] FAIL: pt_alloc for 2MB split at 0x");
            serial_puthex(virt, 16);
            serial_puts("\n");
            goto out;
        }
        /* Fill PT with 512 identity-mapped 4KB entries */
        for (int i = 0; i < 512; i++)
            pt[i] = (large_phys + i * PAGE_SIZE) | large_flags;
        /* Replace 2MB entry with PT pointer (store phys, hardware reads it) */
        pd[pd_idx] = (uint64_t)VIRT_TO_PHYS(pt) | PTE_PRESENT | PTE_WRITABLE;
#ifndef OK_QUIET
        serial_puts("[paging] split 2MB @ 0x");
        serial_puthex(large_phys, 8);
        serial_puts(" -> PT 0x");
        serial_puthex((uint64_t)VIRT_TO_PHYS(pt), 8);
        serial_puts("\n");
#endif

        /* Full TLB flush after 2MB→4KB split.
         * invlpg alone doesn't reliably flush stale 2MB TLB entries
         * in QEMU TCG. Toggle CR4.PGE + reload CR3 to flush ALL
         * entries including global ones. */
        {
            uint64_t cr3_val, cr4;
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
            __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
            __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
            __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
            __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
        }
    }

    uint64_t *pt = pt_get_or_create(pd, pd_idx);
    if (!pt) goto out;

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    result = 0;

out:
    paging_unlock_irqrestore(irq_flags);
    return result;
}

/* ── Map a 2MB large page ────────────────────────────────────── */

static int paging_map_2m(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq_flags = paging_lock_irqsave();
    int result = -1;
    uint64_t *pdpt = pt_get_or_create(kernel_pml4, PML4_INDEX(virt));
    if (!pdpt) goto out;

    uint64_t *pd = pt_get_or_create(pdpt, PDPT_INDEX(virt));
    if (!pd) goto out;

    pd[PD_INDEX(virt)] = (phys & 0x000FFFFFFFE00000ULL) | flags | PTE_LARGE;
    result = 0;
out:
    paging_unlock_irqrestore(irq_flags);
    return result;
}

/* ── Map a range with an arbitrary virt = phys + virt_offset ── */

/* Maps [phys_start, phys_end) into the kernel PML4 at
 * virt = phys + virt_offset. Auto-selects 2 MB or 4 KB pages.
 * - virt_offset == 0 gives an identity map (the old behavior).
 * - virt_offset == KERNEL_VBASE gives the upper-half direct map.
 * Both can coexist — the kernel currently installs both. */
static void paging_map_range_at(uint64_t phys_start, uint64_t phys_end,
                                uint64_t virt_offset, uint64_t extra_flags)
{
    uint64_t flags_2m = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | extra_flags;
    uint64_t flags_4k = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | extra_flags;

    /* Align start down and end up to 4KB boundaries */
    phys_start &= ~(PAGE_SIZE - 1);
    phys_end = (phys_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t phys = phys_start;
    while (phys < phys_end) {
        uint64_t virt = phys + virt_offset;
        /* Use 2MB page if aligned and enough space */
        if ((phys & (LARGE_PAGE_SIZE - 1)) == 0 && (phys_end - phys) >= LARGE_PAGE_SIZE) {
            paging_map_2m(virt, phys, flags_2m);
            phys += LARGE_PAGE_SIZE;
        } else {
            paging_map_4k(virt, phys, flags_4k);
            phys += PAGE_SIZE;
        }
    }
}

/* ── Identity map a range (thin wrapper) ────────────────────── */

static void paging_identity_map_range(uint64_t start, uint64_t end, uint64_t extra_flags)
{
    paging_map_range_at(start, end, 0, extra_flags);
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

/* Forward decl: pte_walk is defined further down (used by paging_get_pte
 * and the per-process variants below). */
static uint64_t *pte_walk(uint64_t *table, int index);

/* ── Per-process variant of paging_map_4k ─────────────────────── */

/* Same as paging_map_4k but operates on the given PML4 instead of
 * kernel_pml4. Used by paging_map_page_in_cr3 to map pages into a
 * specific process's address space. Auto-creates intermediate tables. */
static int paging_map_4k_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq_flags = paging_lock_irqsave();
    int result = -1;
    uint64_t *pdpt = pt_get_or_create(pml4, PML4_INDEX(virt));
    if (!pdpt) goto out;

    uint64_t *pd = pt_get_or_create(pdpt, PDPT_INDEX(virt));
    if (!pd) goto out;

    int pd_idx = PD_INDEX(virt);
    if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_LARGE)) {
        /* 2 MB → 4 KB split */
        uint64_t large_phys = pd[pd_idx] & 0x000FFFFFFFE00000ULL;
        uint64_t large_flags = pd[pd_idx] & ~(PTE_ADDR_MASK | PTE_LARGE);
        uint64_t *pt = pt_alloc_page();
        if (!pt) goto out;
        for (int i = 0; i < 512; i++)
            pt[i] = (large_phys + i * PAGE_SIZE) | large_flags;
        pd[pd_idx] = (uint64_t)VIRT_TO_PHYS(pt) | PTE_PRESENT | PTE_WRITABLE;

        /* Full TLB flush after split */
        uint64_t cr3_val, cr4;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
        __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3_val) : "memory");
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }

    uint64_t *pt = pt_get_or_create(pd, pd_idx);
    if (!pt) goto out;

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    result = 0;
out:
    paging_unlock_irqrestore(irq_flags);
    return result;
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

/* Map a page in a specific process's address space (used by
 * demand_page_fault for per-process ELF segments). cr3 must point at
 * a valid PML4 created by paging_create_process_cr3. */
int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!cr3) return -1;
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    int ret = paging_map_4k_in(pml4, virt, phys, flags | PTE_PRESENT);
    if (ret == 0) invlpg(virt);
    return ret;
}

/* Unmap a single 4KB page in a specific process's address space. */
int paging_unmap_page_in_cr3(uint64_t cr3, uint64_t virt)
{
    if (!cr3) return -1;
    uint64_t irq_flags = paging_lock_irqsave();
    int result = -1;
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    uint64_t *pdpt, *pd, *pt;
    int idx;

    idx = PML4_INDEX(virt);
    if (!(pml4[idx] & PTE_PRESENT)) goto out;
    pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[idx] & PTE_ADDR_MASK);

    idx = PDPT_INDEX(virt);
    if (!(pdpt[idx] & PTE_PRESENT)) goto out;
    pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx] & PTE_ADDR_MASK);

    idx = PD_INDEX(virt);
    if (!(pd[idx] & PTE_PRESENT)) goto out;
    if (pd[idx] & PTE_LARGE) goto out;
    pt = (uint64_t *)PHYS_TO_VIRT(pd[idx] & PTE_ADDR_MASK);

    pt[PT_INDEX(virt)] = 0;
    result = 0;
out:
    paging_unlock_irqrestore(irq_flags);
    if (result == 0) invlpg(virt);
    return result;
}

/* Read a PTE from a specific process's address space. */
uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t virt)
{
    if (!cr3) return NULL;
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    uint64_t *pdpt = pte_walk(pml4, PML4_INDEX(virt));
    if (!pdpt) return NULL;
    uint64_t *pd = pte_walk(pdpt, PDPT_INDEX(virt));
    if (!pd) return NULL;
    if (pd[PD_INDEX(virt)] & PTE_LARGE) return NULL;
    uint64_t *pt = pte_walk(pd, PD_INDEX(virt));
    if (!pt) return NULL;
    return &pt[PT_INDEX(virt)];
}

int paging_query_mapping_in_cr3(uint64_t cr3, uint64_t virt,
                                uint64_t *flags, uint64_t *page_size)
{
    if (!cr3) return -1;

    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    uint64_t pml4e = pml4[PML4_INDEX(virt)];
    if (!(pml4e & PTE_PRESENT)) return -1;

    int writable = (pml4e & PTE_WRITABLE) != 0;
    int user = (pml4e & PTE_USER) != 0;
    uint64_t nx = pml4e & PTE_NX;

    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4e & PTE_ADDR_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virt)];
    if (!(pdpte & PTE_PRESENT)) return -1;
    writable = writable && (pdpte & PTE_WRITABLE);
    user = user && (pdpte & PTE_USER);
    nx |= pdpte & PTE_NX;
    if (pdpte & PTE_LARGE) {
        uint64_t effective = pdpte;
        if (!writable) effective &= ~PTE_WRITABLE;
        if (!user) effective &= ~PTE_USER;
        effective |= nx;
        if (flags) *flags = effective;
        if (page_size) *page_size = 1ULL << 30;
        return 0;
    }

    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpte & PTE_ADDR_MASK);
    uint64_t pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT)) return -1;
    writable = writable && (pde & PTE_WRITABLE);
    user = user && (pde & PTE_USER);
    nx |= pde & PTE_NX;
    if (pde & PTE_LARGE) {
        uint64_t effective = pde;
        if (!writable) effective &= ~PTE_WRITABLE;
        if (!user) effective &= ~PTE_USER;
        effective |= nx;
        if (flags) *flags = effective;
        if (page_size) *page_size = 1ULL << 21;
        return 0;
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT)) return -1;
    writable = writable && (pte & PTE_WRITABLE);
    user = user && (pte & PTE_USER);
    nx |= pte & PTE_NX;

    uint64_t effective = pte;
    if (!writable) effective &= ~PTE_WRITABLE;
    if (!user) effective &= ~PTE_USER;
    effective |= nx;
    if (flags) *flags = effective;
    if (page_size) *page_size = PAGE_SIZE;
    return 0;
}

uint64_t paging_translate_in_cr3(uint64_t cr3, uint64_t virt)
{
    if (!cr3) return UINT64_MAX;

    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    uint64_t pml4e = pml4[PML4_INDEX(virt)];
    if (!(pml4e & PTE_PRESENT)) return UINT64_MAX;

    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4e & PTE_ADDR_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virt)];
    if (!(pdpte & PTE_PRESENT)) return UINT64_MAX;
    if (pdpte & PTE_LARGE)
        return (pdpte & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);

    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpte & PTE_ADDR_MASK);
    uint64_t pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT)) return UINT64_MAX;
    if (pde & PTE_LARGE)
        return (pde & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT)) return UINT64_MAX;
    return (pte & PTE_ADDR_MASK) | (virt & 0xFFFULL);
}

void paging_debug_dump_walk_in_cr3(uint64_t cr3, uint64_t virt)
{
    uint64_t root = cr3 & PTE_ADDR_MASK;
    serial_puts("  [PT-WALK] cr3=0x");
    serial_puthex(root, 16);
    serial_puts(" va=0x");
    serial_puthex(virt, 16);
    serial_puts("\n");
    if (!root) {
        serial_puts("  [PT-WALK] invalid root\n");
        return;
    }

    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(root);
    uint64_t pml4e = pml4[PML4_INDEX(virt)];
    serial_puts("  [PT-WALK] pml4e=0x");
    serial_puthex(pml4e, 16);
    if (!(pml4e & PTE_PRESENT)) {
        serial_puts(" not-present\n");
        return;
    }

    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4e & PTE_ADDR_MASK);
    uint64_t pdpte = pdpt[PDPT_INDEX(virt)];
    serial_puts(" pdpte=0x");
    serial_puthex(pdpte, 16);
    if (!(pdpte & PTE_PRESENT)) {
        serial_puts(" not-present\n");
        return;
    }
    if (pdpte & PTE_LARGE) {
        serial_puts(" 1g-large\n");
        return;
    }

    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpte & PTE_ADDR_MASK);
    uint64_t pde = pd[PD_INDEX(virt)];
    serial_puts(" pde=0x");
    serial_puthex(pde, 16);
    if (!(pde & PTE_PRESENT)) {
        serial_puts(" not-present\n");
        return;
    }
    if (pde & PTE_LARGE) {
        serial_puts(" 2m-large\n");
        return;
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(virt)];
    serial_puts(" pte=0x");
    serial_puthex(pte, 16);
    serial_puts("\n");
}

static uint64_t paging_next_boundary(uint64_t address, uint64_t span,
                                     uint64_t limit)
{
    uint64_t next = (address & ~(span - 1)) + span;
    if (next <= address || next > limit)
        return limit;
    return next;
}

uint64_t paging_first_mapped_end_in_cr3(uint64_t cr3, uint64_t virt,
                                        uint64_t size)
{
    if (!cr3 || !size || virt + size < virt)
        return UINT64_MAX;

    uint64_t end = virt + size;
    uint64_t cursor = virt;
    uint64_t mapped_end = 0;
    uint64_t irq_flags = paging_lock_irqsave();
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);

    while (cursor < end) {
        uint64_t pml4e = pml4[PML4_INDEX(cursor)];
        if (!(pml4e & PTE_PRESENT)) {
            cursor = paging_next_boundary(cursor, 1ULL << 39, end);
            continue;
        }

        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4e & PTE_ADDR_MASK);
        uint64_t pdpte = pdpt[PDPT_INDEX(cursor)];
        if (!(pdpte & PTE_PRESENT)) {
            cursor = paging_next_boundary(cursor, 1ULL << 30, end);
            continue;
        }
        if (pdpte & PTE_LARGE) {
            mapped_end = paging_next_boundary(cursor, 1ULL << 30,
                                               UINT64_MAX);
            break;
        }

        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpte & PTE_ADDR_MASK);
        uint64_t pde = pd[PD_INDEX(cursor)];
        if (!(pde & PTE_PRESENT)) {
            cursor = paging_next_boundary(cursor, 1ULL << 21, end);
            continue;
        }
        if (pde & PTE_LARGE) {
            mapped_end = paging_next_boundary(cursor, 1ULL << 21,
                                               UINT64_MAX);
            break;
        }

        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PTE_ADDR_MASK);
        uint64_t pt_end = paging_next_boundary(cursor, 1ULL << 21, end);
        while (cursor < pt_end) {
            if (pt[PT_INDEX(cursor)] & PTE_PRESENT) {
                mapped_end = cursor + PAGE_SIZE;
                goto out;
            }
            cursor += PAGE_SIZE;
        }
    }

out:
    paging_unlock_irqrestore(irq_flags);
    return mapped_end;
}

int paging_copy_between_cr3(uint64_t dst_cr3, uint64_t dst_va,
                            uint64_t src_cr3, uint64_t src_va,
                            uint64_t size, uint64_t *bytes_copied)
{
    uint64_t done = 0;
    if (bytes_copied) *bytes_copied = 0;
    if (!size) return 0;
    if (!dst_cr3 || !src_cr3 || dst_va + size < dst_va ||
        src_va + size < src_va)
        return -1;

    int backward = dst_cr3 == src_cr3 && dst_va > src_va &&
                   dst_va < src_va + size;
    while (done < size) {
        uint64_t src_cur;
        uint64_t dst_cur;
        uint64_t chunk;
        if (backward) {
            src_cur = src_va + size - done - 1;
            dst_cur = dst_va + size - done - 1;
            chunk = size - done;
            uint64_t src_in_page = (src_cur & 0xFFFULL) + 1;
            uint64_t dst_in_page = (dst_cur & 0xFFFULL) + 1;
            if (chunk > src_in_page) chunk = src_in_page;
            if (chunk > dst_in_page) chunk = dst_in_page;
        } else {
            src_cur = src_va + done;
            dst_cur = dst_va + done;
            chunk = size - done;
            uint64_t src_left = 4096 - (src_cur & 0xFFFULL);
            uint64_t dst_left = 4096 - (dst_cur & 0xFFFULL);
            if (chunk > src_left) chunk = src_left;
            if (chunk > dst_left) chunk = dst_left;
        }

        uint64_t irq_flags = paging_lock_irqsave();
        uint64_t src_phys = paging_translate_in_cr3(src_cr3, src_cur);
        uint64_t dst_phys = paging_translate_in_cr3(dst_cr3, dst_cur);
        if (src_phys == UINT64_MAX || dst_phys == UINT64_MAX) {
            paging_unlock_irqrestore(irq_flags);
            if (bytes_copied) *bytes_copied = done;
            return -1;
        }

        volatile uint8_t *src = (volatile uint8_t *)PHYS_TO_VIRT(src_phys);
        volatile uint8_t *dst = (volatile uint8_t *)PHYS_TO_VIRT(dst_phys);
        if (backward) {
            for (uint64_t i = 0; i < chunk; i++)
                dst[-(int64_t)i] = src[-(int64_t)i];
        } else {
            for (uint64_t i = 0; i < chunk; i++) dst[i] = src[i];
        }
        paging_unlock_irqrestore(irq_flags);
        done += chunk;
    }

    if (bytes_copied) *bytes_copied = done;
    return 0;
}

/* Unmap a single 4KB page — clears PTE, invalidates TLB */
int paging_unmap_page(uint64_t virt)
{
    if (!kernel_pml4) return -1;

    uint64_t *pdpt, *pd, *pt;
    int idx;

    idx = PML4_INDEX(virt);
    if (!(kernel_pml4[idx] & PTE_PRESENT)) return -1;
    pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[idx] & PTE_ADDR_MASK);

    idx = PDPT_INDEX(virt);
    if (!(pdpt[idx] & PTE_PRESENT)) return -1;
    pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx] & PTE_ADDR_MASK);

    idx = PD_INDEX(virt);
    if (!(pd[idx] & PTE_PRESENT)) return -1;
    if (pd[idx] & PTE_LARGE) return -1;  /* Can't unmap within 2MB page */
    pt = (uint64_t *)PHYS_TO_VIRT(pd[idx] & PTE_ADDR_MASK);

    pt[PT_INDEX(virt)] = 0;
    invlpg(virt);
    return 0;
}

/* Restore the normal writable/global upper-half alias for a RAM frame.
 * Guard pages temporarily punch holes in the direct map; the hole must be
 * closed before the frame returns to the PMM or its next owner will receive
 * allocated physical memory that faults through PHYS_TO_VIRT(). */
int paging_restore_direct_map_page(uint64_t phys)
{
    phys &= PTE_ADDR_MASK;
    return paging_map_page(KERNEL_VBASE + phys, phys,
                           PTE_WRITABLE | PTE_GLOBAL);
}

/* Change protection flags on a 4KB page */
int paging_set_flags(uint64_t virt, uint64_t flags)
{
    if (!kernel_pml4) return -1;

    uint64_t *pdpt, *pd, *pt;
    int idx;

    idx = PML4_INDEX(virt);
    if (!(kernel_pml4[idx] & PTE_PRESENT)) return -1;
    pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[idx] & PTE_ADDR_MASK);

    idx = PDPT_INDEX(virt);
    if (!(pdpt[idx] & PTE_PRESENT)) return -1;
    pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx] & PTE_ADDR_MASK);

    idx = PD_INDEX(virt);
    if (!(pd[idx] & PTE_PRESENT)) return -1;
    if (pd[idx] & PTE_LARGE) return -1;  /* Can't change 2MB page flags */
    pt = (uint64_t *)PHYS_TO_VIRT(pd[idx] & PTE_ADDR_MASK);

    idx = PT_INDEX(virt);
    uint64_t phys = pt[idx] & PTE_ADDR_MASK;
    pt[idx] = phys | flags;
    invlpg(virt);
    return 0;
}

/* Change protection flags on a 4KB page in a specific process CR3. */
int paging_set_flags_in_cr3(uint64_t cr3, uint64_t virt, uint64_t flags)
{
    if (!cr3) return -1;

    uint64_t irq_flags = paging_lock_irqsave();
    int result = -1;
    uint64_t *pte = paging_get_pte_in_cr3(cr3, virt);
    /* PAGE_NOACCESS intentionally clears PRESENT while retaining the backing
     * frame in the PTE. Accept that state so VirtualProtect can restore access
     * without allocating or losing the original page. */
    if (!pte || !(*pte & PTE_ADDR_MASK))
        goto out;

    uint64_t phys = *pte & PTE_ADDR_MASK;
    *pte = phys | flags;
    result = 0;
out:
    paging_unlock_irqrestore(irq_flags);
    if (result == 0) invlpg(virt);
    return result;
}

/* Map an MMIO region (uncacheable) */
int paging_map_mmio(uint64_t phys, uint64_t size)
{
    if (!kernel_pml4) return -1;
    uint64_t flags = PTE_PWT | PTE_PCD;  /* uncacheable */
    /* Install at both the low identity VA (legacy callers that still
     * dereference phys) and at the upper-half mirror (PML4[256], shared
     * across every process's CR3 once the lower-half identity map is
     * dropped from user PML4s). */
    paging_identity_map_range(phys, phys + size, flags);
    paging_map_range_at(phys, phys + size, KERNEL_VBASE, flags);
    return 0;
}

/* Map a region as Write-Combining (for framebuffers).
 * Requires PAT entry 1 = WC (call paging_setup_pat first). */
int paging_map_wc(uint64_t phys, uint64_t size)
{
    if (!kernel_pml4) return -1;
    /* PWT=1, PCD=0, PAT=0 → selects PAT entry 1 = WC.
     * Install at both the low-identity VA (legacy callers + drivers
     * that haven't been migrated to PHYS_TO_VIRT) AND at the upper-
     * half mirror VA so the mapping is reachable from any CR3 once
     * the lower-half identity map disappears from user PML4s. */
    paging_identity_map_range(phys, phys + size, PTE_PWT);
    paging_map_range_at(phys, phys + size, KERNEL_VBASE, PTE_PWT);
    return 0;
}

/* Program PAT MSR: change entry 1 from WT to WC.
 * This enables Write-Combining via PWT=1,PCD=0 page flags. */
void paging_setup_pat(void)
{
    uint32_t lo, hi;
    /* Read current PAT MSR (0x277) */
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    serial_puts("[PAGE] PAT MSR: 0x");
    serial_puthex(((uint64_t)hi << 32) | lo, 16);
    serial_puts("\n");
    /* Change PAT1 (bits 15:8) from WT (0x04) to WC (0x01) */
    lo = (lo & ~0xFF00U) | 0x0100U;
    __asm__ volatile ("wrmsr" : : "c"(0x277), "a"(lo), "d"(hi));
    serial_puts("[PAGE] PAT1 set to WC\n");
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

/* ── Reserve active UEFI page table pages ─────────────────────
 *
 * After kexec (or even on first boot), the page allocator marks
 * EFI_BOOT_SERVICES_DATA as free — but the active page tables
 * (pointed to by CR3) live in that memory.  If pt_alloc_page()
 * hands out one of those pages and memsets it to zero, the active
 * address translation is destroyed → triple fault.
 *
 * Walk PML4 → PDPT → PD and reserve every page used by the
 * current page table tree.  After CR3 switch we free them.
 */

#define OLD_PT_MAX 32
static uint64_t old_pt_pages[OLD_PT_MAX];
static uint32_t old_pt_count;

static void reserve_old_page_tables(uint64_t cr3_phys)
{
    old_pt_count = 0;

    uint64_t pml4_page = cr3_phys & ~(PAGE_SIZE - 1);
    mem_reserve_range(pml4_page, 1);
    if (old_pt_count < OLD_PT_MAX)
        old_pt_pages[old_pt_count++] = pml4_page;

    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_page);
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;

        uint64_t pdpt_page = pml4[i] & PTE_ADDR_MASK;
        mem_reserve_range(pdpt_page, 1);
        if (old_pt_count < OLD_PT_MAX)
            old_pt_pages[old_pt_count++] = pdpt_page;

        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pdpt_page);
        for (int j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            if (pdpt[j] & PTE_LARGE) continue;  /* 1GB page, no PD */

            uint64_t pd_page = pdpt[j] & PTE_ADDR_MASK;
            mem_reserve_range(pd_page, 1);
            if (old_pt_count < OLD_PT_MAX)
                old_pt_pages[old_pt_count++] = pd_page;

            /* Don't descend into PT level — 2MB large pages don't
             * have PT entries, and 4KB PTs are unlikely in UEFI. */
        }
    }

    serial_puts("[PAGE] Reserved ");
    serial_putdec(old_pt_count);
    serial_puts(" old page-table pages\n");
}

static void free_old_page_tables(void)
{
    for (uint32_t i = 0; i < old_pt_count; i++)
        mem_free_pages((void *)old_pt_pages[i], 1);
    serial_puts("[PAGE] Freed ");
    serial_putdec(old_pt_count);
    serial_puts(" old page-table pages\n");
    old_pt_count = 0;
}

/* ── Initialize paging ───────────────────────────────────────── */

void __initk paging_init(void)
{
    serial_puts("[PAGE] Setting up kernel page tables...\n");
    paging_enable_nxe();

    uint64_t old_cr3 = read_cr3();
    serial_puts("[PAGE] Current CR3: 0x");
    serial_puthex(old_cr3, 16);
    serial_puts("\n");

    /* Reserve active UEFI/old page table pages so the allocator
     * doesn't hand them out while we're still using them. */
    reserve_old_page_tables(old_cr3);

    pt_pages_used = 0;

    /* Allocate PML4 */
    kernel_pml4 = pt_alloc_page();
    if (!kernel_pml4) {
        serial_puts("[PAGE] FATAL: Cannot allocate PML4\n");
        return;
    }

    /* ── Identity map memory regions ── */

    /* Determine the highest physical address that needs mapping.
     * With QEMU -m 4G, RAM may extend above the 4GB MMIO hole
     * (e.g., 0x100000000..0x17FFFFFFF). We must map all of it
     * so pt_alloc_page's PHYS_TO_VIRT works for any allocated page. */
    extern uint64_t mem_get_highest_address(void);
    uint64_t highest = mem_get_highest_address();
    /* Always cover at least 4GB (for MMIO: APIC, IOAPIC, PCI config) */
    if (highest < 4ULL * 1024 * 1024 * 1024)
        highest = 4ULL * 1024 * 1024 * 1024;
    /* Round up to GB boundary */
    highest = (highest + (1ULL << 30) - 1) & ~((1ULL << 30) - 1);

    serial_puts("[PAGE] Mapping ");
    serial_putdec(highest / (1024 * 1024 * 1024));
    serial_puts(" GB physical...\n");
    paging_identity_map_range(0, highest, 0);

    /* Upper-half direct map: mirror all of physical space at
     * VA = phys + KERNEL_VBASE. This populates PML4[256] so that
     * PHYS_TO_VIRT works for any physical address. */
    serial_puts("[PAGE] Mapping upper-half mirror at 0x");
    serial_puthex(KERNEL_VBASE, 16);
    serial_puts("...\n");
    paging_map_range_at(0, highest, KERNEL_VBASE, 0);

    serial_puts("[PAGE] Page tables built: ");
    serial_putdec(pt_pages_used);
    serial_puts(" pages (");
    serial_putdec(pt_pages_used * 4);
    serial_puts(" KB)\n");

    /* ── Switch CR3 ── */
    /* kernel_pml4 is now an upper-half virt pointer; the CR3 register
     * needs the underlying physical address. */
    kernel_cr3 = VIRT_TO_PHYS(kernel_pml4);

    serial_puts("[PAGE] Switching CR3 to 0x");
    serial_puthex(kernel_cr3, 16);
    serial_puts("...\n");

    /* Interrupts off during CR3 switch for safety */
    __asm__ volatile ("cli");
    write_cr3(kernel_cr3);
    __asm__ volatile ("sti");

    /* If we get here, paging is working — release old UEFI tables */
    serial_puts("[PAGE] CR3 switch successful — kernel paging active\n");
    free_old_page_tables();

    /* NULL guard: remap VA 0 to a fresh zeroed physical page.
     * Without this, VA 0 identity-maps to PA 0 (BIOS IVT) which
     * contains stale vectors like 0x20008 → #GP when dereferenced.
     * By pointing VA 0 at a fresh zeroed page:
     * - NULL reads return 0 (safe: terminates hash chains, vtable=0)
     * - NULL writes go to the fresh page (harmless, don't corrupt BIOS)
     * - Subsequent reads may see written data, but it's from PE code,
     *   not BIOS IVT garbage (no 0x8006-style vtable pointers). */
    {
        void *guard_phys = mem_alloc_pages(1);
        if (guard_phys) {
            memset(PHYS_TO_VIRT(guard_phys), 0, PAGE_SIZE);
            paging_map_4k(0, (uint64_t)guard_phys,
                          PTE_PRESENT | PTE_GLOBAL | PTE_NX); /* read-only + NX */
            invlpg(0);
            serial_puts("[PAGE] Page 0: remapped to fresh zeroed PA 0x");
            serial_puthex((uint64_t)guard_phys, 8);
            serial_puts("\n");
        }
    }

    fb_puts(" Paging: 4-level, ");
    fb_putdec(pt_pages_used * 4);
    fb_puts(" KB tables\n");
}

/* ── Win32 per-process page table ────────────────────────────────
 *
 * Creates a separate PML4 for Win32 PE32 processes. The key difference:
 * PDPT[1] (VAs 0x40000000-0x7FFFFFFF) has its own Page Directory so
 * VirtualAlloc mappings don't alias with the kernel's identity map.
 *
 * Layout:
 *   Win32 PML4 → shares kernel PML4 entries (0..511)
 *     EXCEPT PML4[0] → new PDPT
 *       PDPT[0] → shared with kernel (0x0-0x3FFFFFFF)
 *       PDPT[1] → OWN PD (0x40000000-0x7FFFFFFF, VirtualAlloc range)
 *       PDPT[2..3] → shared with kernel (0x80000000-0xFFFFFFFF)
 *       (rest shared)
 */

/* ── Per-process page tables ─────────────────────────────────── */
/* Creates a new PML4 that shares kernel mappings but has its own
 * PDPT[1] PD for user-space allocations (0x40000000-0x7FFFFFFF).
 * Kernel pages (heap, identity-map) are shared by reference,
 * so updates in the kernel PD are automatically visible.          */

/* Helper: clone a kernel PD into a private per-process PD.
 * If the kernel PD entry is PRESENT, copies its 4KB page contents
 * (which contains 512 PD-level entries — large pages or PT pointers).
 * Returns the new PD page, or NULL on failure. */
/* Recursively free the per-process lower-half page-table tree. Walks
 * PDPT[0..511] and for each present PDPT entry walks its PD freeing
 * any present PT pages, then the PD itself. The actual data pages
 * (the things the PTEs map to) are NOT freed here — those belong to
 * the VMA layer in syscall.c which has already cleaned them up by
 * the time this runs from proc_free. */
static void free_user_pdpt(uint64_t *pdpt)
{
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & PTE_PRESENT)) continue;
        if (pdpt[i] & PTE_LARGE) continue;  /* 1GB huge page — no PD */
        uint64_t pd_phys = pdpt[i] & PTE_ADDR_MASK;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pd_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT)) continue;
            if (pd[j] & PTE_LARGE) continue;  /* 2MB large page — no PT */
            uint64_t pt_phys = pd[j] & PTE_ADDR_MASK;
            mem_free_pages((void *)pt_phys, 1);
        }
        mem_free_pages((void *)pd_phys, 1);
    }
}

uint64_t paging_create_process_cr3(void)
{
    if (!kernel_pml4) return 0;

    /* pt_alloc_page() returns a zeroed PML4. Share only the canonical
     * upper half with the kernel; every lower-half slot must stay private.
     * Win64 images use lower-half PML4 slots far above slot zero. */
    uint64_t *pml4 = pt_alloc_page();
    if (!pml4) return 0;
    for (int i = 256; i < 512; i++)
        pml4[i] = kernel_pml4[i];

    /* Return phys for the CR3 register */
    return VIRT_TO_PHYS(pml4);
}

void paging_free_process_cr3(uint64_t cr3)
{
    if (!cr3 || cr3 == kernel_cr3) return;

    /* cr3 is a phys address; resolve it via the upper-half mirror. */
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT))
            continue;
        uint64_t pdpt_phys = pml4[i] & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);
        free_user_pdpt(pdpt);
        mem_free_pages((void *)pdpt_phys, 1);
    }
    mem_free_pages((void *)(cr3 & PTE_ADDR_MASK), 1);
}

static void paging_test_expect(int condition, const char *name,
                               int *checks, int *failures)
{
    (*checks)++;
    if (condition) return;
    (*failures)++;
    serial_puts("[CR3TEST] FAIL: ");
    serial_puts(name);
    serial_puts("\n");
}

int paging_process_cr3_selftest(void)
{
    const uint64_t test_va = 0x0000002000000000ULL;
    const uint64_t low_win64_va = 0x00000001F0000000ULL;
    const uint64_t value_a = 0x1122334455667788ULL;
    const uint64_t value_b = 0x8877665544332211ULL;
    uint64_t cr3_a = 0;
    uint64_t cr3_b = 0;
    void *phys_a_ptr = NULL;
    void *phys_b_ptr = NULL;
    int mapped_a = 0;
    int mapped_b = 0;
    int checks = 0;
    int failures = 0;

    serial_puts("[CR3TEST] starting process address-space isolation test\n");
    cr3_a = paging_create_process_cr3();
    cr3_b = paging_create_process_cr3();
    paging_test_expect(cr3_a && cr3_b && cr3_a != cr3_b,
                       "create distinct process roots", &checks, &failures);
    if (!cr3_a || !cr3_b) goto cleanup;

    paging_test_expect(paging_translate_in_cr3(cr3_a, test_va) == UINT64_MAX &&
                       paging_translate_in_cr3(cr3_b, test_va) == UINT64_MAX,
                       "private lower halves start unmapped", &checks,
                       &failures);
    paging_test_expect(
        paging_first_mapped_end_in_cr3(cr3_a, low_win64_va,
                                       0x0FFE0000ULL) == 0 &&
        paging_first_mapped_end_in_cr3(cr3_b, low_win64_va,
                                       0x0FFE0000ULL) == 0,
        "low Win64 reservation range starts unmapped", &checks, &failures);

    phys_a_ptr = mem_alloc_pages(1);
    phys_b_ptr = mem_alloc_pages(1);
    paging_test_expect(phys_a_ptr && phys_b_ptr && phys_a_ptr != phys_b_ptr,
                       "allocate distinct backing pages", &checks, &failures);
    if (!phys_a_ptr || !phys_b_ptr) goto cleanup;

    uint64_t phys_a = (uint64_t)(uintptr_t)phys_a_ptr;
    uint64_t phys_b = (uint64_t)(uintptr_t)phys_b_ptr;
    *(volatile uint64_t *)PHYS_TO_VIRT(phys_a) = value_a;
    *(volatile uint64_t *)PHYS_TO_VIRT(phys_b) = value_b;

    mapped_a = paging_map_page_in_cr3(
        cr3_a, test_va, phys_a, PTE_WRITABLE | PTE_USER | PTE_NX) == 0;
    mapped_b = paging_map_page_in_cr3(
        cr3_b, test_va, phys_b, PTE_WRITABLE | PTE_USER | PTE_NX) == 0;
    paging_test_expect(mapped_a && mapped_b, "map identical VA in both roots",
                       &checks, &failures);
    if (!mapped_a || !mapped_b) goto cleanup;

    paging_test_expect(
        paging_translate_in_cr3(cr3_a, test_va) == phys_a &&
        paging_translate_in_cr3(cr3_b, test_va) == phys_b,
        "identical VA resolves to owner backing", &checks, &failures);
    paging_test_expect(
        *(volatile uint64_t *)PHYS_TO_VIRT(
            paging_translate_in_cr3(cr3_a, test_va)) == value_a &&
        *(volatile uint64_t *)PHYS_TO_VIRT(
            paging_translate_in_cr3(cr3_b, test_va)) == value_b,
        "owner contents remain independent", &checks, &failures);

    uint64_t copied = 0;
    paging_test_expect(
        paging_copy_between_cr3(cr3_b, test_va, cr3_a, test_va,
                                sizeof(value_a), &copied) == 0 &&
        copied == sizeof(value_a) &&
        *(volatile uint64_t *)PHYS_TO_VIRT(phys_b) == value_a,
        "copy data between explicit roots", &checks, &failures);
    *(volatile uint64_t *)PHYS_TO_VIRT(phys_b) = value_b;
    copied = 0;
    paging_test_expect(
        paging_copy_between_cr3(cr3_b, test_va + 4088,
                                cr3_a, test_va + 4088, 16, &copied) != 0 &&
        copied == 8,
        "report partial copy at unmapped boundary", &checks, &failures);

    paging_test_expect(
        paging_set_flags_in_cr3(cr3_a, test_va,
                                PTE_PRESENT | PTE_USER | PTE_NX) == 0,
        "change protection in one root", &checks, &failures);
    uint64_t *pte_a = paging_get_pte_in_cr3(cr3_a, test_va);
    uint64_t *pte_b = paging_get_pte_in_cr3(cr3_b, test_va);
    paging_test_expect(pte_a && pte_b && !(*pte_a & PTE_WRITABLE) &&
                       (*pte_b & PTE_WRITABLE),
                       "protection changes stay owner-local", &checks,
                       &failures);

    uint64_t kernel_va = (uint64_t)(uintptr_t)&paging_process_cr3_selftest;
    uint64_t kernel_phys = paging_translate_in_cr3(kernel_cr3, kernel_va);
    paging_test_expect(kernel_phys != UINT64_MAX &&
                       paging_translate_in_cr3(cr3_a, kernel_va) == kernel_phys &&
                       paging_translate_in_cr3(cr3_b, kernel_va) == kernel_phys,
                       "kernel upper half is shared", &checks, &failures);

    paging_test_expect(paging_unmap_page_in_cr3(cr3_a, test_va) == 0 &&
                       paging_translate_in_cr3(cr3_a, test_va) == UINT64_MAX &&
                       paging_translate_in_cr3(cr3_b, test_va) == phys_b,
                       "unmap stays owner-local", &checks, &failures);
    mapped_a = 0;

cleanup:
    if (mapped_a) paging_unmap_page_in_cr3(cr3_a, test_va);
    if (mapped_b) paging_unmap_page_in_cr3(cr3_b, test_va);
    if (cr3_a) paging_free_process_cr3(cr3_a);
    if (cr3_b) paging_free_process_cr3(cr3_b);
    if (phys_a_ptr) mem_free_pages(phys_a_ptr, 1);
    if (phys_b_ptr) mem_free_pages(phys_b_ptr, 1);

    serial_puts("[CR3TEST] checks=");
    serial_putdec((uint64_t)checks);
    serial_puts(" failures=");
    serial_putdec((uint64_t)failures);
    serial_puts("\n");
    return failures;
}

/* ── Copy-on-Write (COW) support ─────────────────────────────── */

/* Walk current CR3's page tables to find the PTE for a virtual address.
 * Returns pointer to the PTE, or NULL if not mapped. */
static uint64_t *pte_walk(uint64_t *table, int index)
{
    if (!(table[index] & PTE_PRESENT)) return NULL;
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

uint64_t *paging_get_pte(uint64_t virt)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(cr3 & PTE_ADDR_MASK);

    uint64_t *pdpt = pte_walk(pml4, PML4_INDEX(virt));
    if (!pdpt) return NULL;
    uint64_t *pd = pte_walk(pdpt, PDPT_INDEX(virt));
    if (!pd) return NULL;
    if (pd[PD_INDEX(virt)] & PTE_LARGE) return NULL;
    uint64_t *pt = pte_walk(pd, PD_INDEX(virt));
    if (!pt) return NULL;
    return &pt[PT_INDEX(virt)];
}

/* Check if a virtual address is mapped as COW */
int paging_is_cow(uint64_t virt)
{
    uint64_t *pte = paging_get_pte(virt);
    if (!pte) return 0;
    return (*pte & PTE_COW) && (*pte & PTE_PRESENT) && !(*pte & PTE_WRITABLE);
}

/* Handle COW fault: copy the page, remap as writable, clear COW bit */
int paging_cow_copy(uint64_t virt)
{
    uint64_t *pte = paging_get_pte(virt);
    if (!pte) return -1;

    uint64_t old_phys = *pte & PTE_ADDR_MASK;
    uint64_t flags = *pte & ~PTE_ADDR_MASK;

    /* Allocate new page and copy contents via the upper-half mirror
     * (old_phys is still live under its own PTE so we copy it through
     * the kernel direct map, not the identity map). */
    void *new_phys = mem_alloc_pages(1);
    if (!new_phys) return -1;
    memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT(old_phys), PAGE_SIZE);

    /* Remap: new physical page, writable, no COW */
    *pte = (uint64_t)new_phys | (flags & ~PTE_COW) | PTE_WRITABLE;

    /* Flush TLB for this address */
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");

    return 0;
}

/* ── Win32 page tables (legacy wrapper) ─────────────────────── */

static uint64_t *win32_pml4;
static uint64_t  win32_cr3_val;
static uint64_t *win32_pdpt;    /* Our own PDPT for PML4[0] */
static uint64_t *win32_pd1;     /* Our own PD for PDPT[1] (VirtualAlloc) */

uint64_t paging_create_win32_cr3(void)
{
    if (!kernel_pml4) return 0;

    /* 1. Allocate new PML4 — SHARE all entries from kernel.
     * Instead of copying (which becomes stale when kernel heap grows),
     * share the same PML4 entries. Only PML4[0] gets a custom PDPT
     * so VirtualAlloc can have its own PD for 0x40000000-0x7FFFFFFF. */
    win32_pml4 = pt_alloc_page();
    if (!win32_pml4) return 0;
    memcpy(win32_pml4, kernel_pml4, PAGE_SIZE);

    /* 2. Get kernel's PDPT for PML4[0] */
    uint64_t *kernel_pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[0] & PTE_ADDR_MASK);

    /* 3. Allocate new PDPT — SHARE all entries from kernel's PDPT.
     * Only PDPT[1] is replaced with our own PD for VirtualAlloc VAs.
     * PDPT[0] points to the SAME PD as the kernel, so identity-map
     * updates (heap growth, new page mappings) are automatically
     * visible in both page tables. */
    win32_pdpt = pt_alloc_page();
    if (!win32_pdpt) return 0;
    /* Copy all PDPT entries — they point to SHARED PDs (not copies) */
    for (int i = 0; i < 512; i++)
        win32_pdpt[i] = kernel_pdpt[i];

    /* 4. PDPT[1] (0x40000000-0x7FFFFFFF): share kernel's PD by reference.
     * This ensures identity-mapped buffers (stack, thunk pool, PE image)
     * allocated with mem_alloc_pages() are visible under both CR3s.
     * VirtualAlloc creates private PTs under this shared PD via
     * paging_win32_map_page(), which splits 2MB pages as needed. */
    win32_pd1 = (kernel_pdpt[1] & PTE_PRESENT)
              ? (uint64_t *)PHYS_TO_VIRT(kernel_pdpt[1] & PTE_ADDR_MASK)
              : NULL;
    /* PDPT[1] already points to kernel's PD via the copy at line 636 */

    /* 5. Wire up: Win32 PML4[0] → our PDPT (all PDs shared by reference) */
    win32_pml4[0] = (uint64_t)VIRT_TO_PHYS(win32_pdpt) | PTE_PRESENT | PTE_WRITABLE;

    win32_cr3_val = VIRT_TO_PHYS(win32_pml4);

    serial_puts("[PAGE] Win32 CR3 created: PML4=0x");
    serial_puthex(win32_cr3_val, 8);
    serial_puts(" PDPT=0x");
    serial_puthex(VIRT_TO_PHYS(win32_pdpt), 8);
    serial_puts(" PD1=0x");
    serial_puthex(win32_pd1 ? VIRT_TO_PHYS(win32_pd1) : 0, 8);
    serial_puts("\n");

    return win32_cr3_val;
}

/* Map a 4KB page in the Win32 address space (for VirtualAlloc).
 * Now that Win32 runs under kernel CR3, this delegates directly
 * to the kernel page mapper. */
int paging_win32_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    return paging_map_page(virt, phys, flags);
}

uint64_t paging_get_win32_cr3(void) { return win32_cr3_val; }

/* Resolve a Win32 VirtualAlloc VA to its physical address.
 * Walks the Win32 page table (PDPT[1] → PD → PT → PA). */
uint64_t paging_win32_va_to_pa(uint64_t va)
{
    if (!win32_pd1) return 0;

    int pd_idx = PD_INDEX(va);
    uint64_t pde = win32_pd1[pd_idx];
    if (!(pde & PTE_PRESENT)) return 0;

    if (pde & PTE_LARGE) {
        /* 2MB large page */
        return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & PTE_ADDR_MASK);
    uint64_t pte = pt[PT_INDEX(va)];
    if (!(pte & PTE_PRESENT)) return 0;

    return (pte & PTE_ADDR_MASK) | (va & 0xFFF);
}
