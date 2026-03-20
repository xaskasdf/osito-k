/*
 * paging.c -- AArch64 Virtual Memory / MMU
 *
 * 3-level page tables (39-bit VA, 4KB granule): L1 → L2 → L3
 * Identity maps all RAM + MMIO regions, then enables MMU.
 *
 * Table layout (T0SZ=25, 39-bit VA):
 *   L1 (TTBR0): 512 entries, each covers 1GB  (bits 38:30)
 *   L2:         512 entries, each covers 2MB   (bits 29:21) — block descriptors
 *   L3:         512 entries, each covers 4KB   (bits 20:12) — page descriptors
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "../include/types.h"

/* ── Constants ──────────────────────────────────────────── */

#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define LARGE_PAGE_SIZE (2ULL * 1024 * 1024)     /* 2MB */
#define LARGE_PAGE_SHIFT 21
#define ENTRIES_PER_TABLE 512

/* AArch64 descriptor bits */
#define DESC_VALID      (1ULL << 0)
#define DESC_TABLE      (1ULL << 1)     /* L1/L2: next-level table */
#define DESC_PAGE       (1ULL << 1)     /* L3: page descriptor */
#define DESC_ATTRINDX(n) ((uint64_t)(n) << 2)
#define DESC_AP_RW_EL1  (0ULL << 6)     /* EL1 RW, EL0 none */
#define DESC_AP_RW_ALL  (1ULL << 6)     /* EL1+EL0 RW */
#define DESC_AP_RO_EL1  (2ULL << 6)     /* EL1 RO, EL0 none */
#define DESC_SH_ISH     (3ULL << 8)     /* Inner shareable */
#define DESC_AF         (1ULL << 10)    /* Access Flag (must set) */
#define DESC_PXN        (1ULL << 53)    /* Privileged execute never */
#define DESC_UXN        (1ULL << 54)    /* Unprivileged execute never */

/* Address mask for table/block/page descriptors */
#define DESC_ADDR_MASK  0x0000FFFFFFFFF000ULL    /* bits 47:12 */
#define BLOCK_ADDR_MASK 0x0000FFFFFFC00000ULL    /* bits 47:21 (2MB aligned) */

/* Composite flags */
#define NORMAL_BLOCK    (DESC_VALID | DESC_ATTRINDX(0) | DESC_AP_RW_EL1 | \
                         DESC_SH_ISH | DESC_AF)
#define NORMAL_PAGE     (DESC_VALID | DESC_PAGE | DESC_ATTRINDX(0) | \
                         DESC_AP_RW_EL1 | DESC_SH_ISH | DESC_AF)
#define DEVICE_BLOCK    (DESC_VALID | DESC_ATTRINDX(1) | DESC_AP_RW_EL1 | \
                         DESC_AF | DESC_PXN | DESC_UXN)
#define DEVICE_PAGE     (DESC_VALID | DESC_PAGE | DESC_ATTRINDX(1) | \
                         DESC_AP_RW_EL1 | DESC_AF | DESC_PXN | DESC_UXN)
#define TABLE_DESC      (DESC_VALID | DESC_TABLE)

/* Index extraction from 39-bit VA */
#define L1_INDEX(va)    (((va) >> 30) & 0x1FF)
#define L2_INDEX(va)    (((va) >> 21) & 0x1FF)
#define L3_INDEX(va)    (((va) >> 12) & 0x1FF)

/* TCR_EL1 configuration (39-bit VA, 4KB granule) */
#define TCR_T0SZ_39BIT  25ULL           /* 64 - 39 = 25 */
#define TCR_IRGN0_WB    (1ULL << 8)     /* Inner write-back RA/WA */
#define TCR_ORGN0_WB    (1ULL << 10)    /* Outer write-back RA/WA */
#define TCR_SH0_ISH     (3ULL << 12)    /* Inner shareable */
#define TCR_TG0_4K      (0ULL << 14)    /* 4KB granule */
#define TCR_EPD1        (1ULL << 23)    /* Disable TTBR1_EL1 walks */
#define TCR_IPS_40BIT   (2ULL << 32)    /* 40-bit PA (1TB) */

#define TCR_VALUE       (TCR_T0SZ_39BIT | TCR_IRGN0_WB | TCR_ORGN0_WB | \
                         TCR_SH0_ISH | TCR_TG0_4K | TCR_EPD1 | TCR_IPS_40BIT)

/* MAIR_EL1: Attr0 = normal WB cacheable, Attr1 = device-nGnRnE */
#define MAIR_NORMAL     0xFFULL         /* Inner/outer WB, RA/WA */
#define MAIR_DEVICE     0x00ULL         /* Device-nGnRnE */
#define MAIR_VALUE      (MAIR_NORMAL | (MAIR_DEVICE << 8))

/* ── State ──────────────────────────────────────────────── */

static uint64_t *kernel_l1;            /* Top-level (L1) table */
static uint64_t  kernel_ttbr0;         /* Physical address of L1 */
static uint32_t  pt_pages_used;

/* ── Allocate a zeroed 4KB-aligned page for tables ──────── */

static uint64_t *pt_alloc_page(void)
{
    uint64_t *page = (uint64_t *)mem_alloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (page)
        pt_pages_used++;
    return page;  /* already zeroed by mem_alloc_aligned */
}

/* ── Get or create next-level table ─────────────────────── */

static uint64_t *pt_get_or_create(uint64_t *table, int index)
{
    if (table[index] & DESC_VALID) {
        if (table[index] & DESC_TABLE)
            return (uint64_t *)(table[index] & DESC_ADDR_MASK);
        /* Entry is a block descriptor — can't descend */
        return (void *)0;
    }

    uint64_t *new_table = pt_alloc_page();
    if (!new_table) return (void *)0;

    table[index] = (uint64_t)new_table | TABLE_DESC;
    return new_table;
}

/* ── Map a 2MB block (L2 block descriptor) ──────────────── */

static void map_2m_block(uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *l2 = pt_get_or_create(kernel_l1, L1_INDEX(va));
    if (!l2) return;
    l2[L2_INDEX(va)] = (pa & BLOCK_ADDR_MASK) | flags;
}

/* ── Map a 4KB page (L3 page descriptor) ────────────────── */

static void map_4k_page(uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *l2 = pt_get_or_create(kernel_l1, L1_INDEX(va));
    if (!l2) return;
    uint64_t *l3 = pt_get_or_create(l2, L2_INDEX(va));
    if (!l3) return;
    l3[L3_INDEX(va)] = (pa & DESC_ADDR_MASK) | flags;
}

/* ── Identity map a range (auto-selects 2MB blocks or 4KB pages) ── */

static void identity_map_range(uint64_t start, uint64_t end, uint64_t block_flags, uint64_t page_flags)
{
    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t addr = start;
    while (addr < end) {
        if ((addr & (LARGE_PAGE_SIZE - 1)) == 0 && (end - addr) >= LARGE_PAGE_SIZE) {
            map_2m_block(addr, addr, block_flags);
            addr += LARGE_PAGE_SIZE;
        } else {
            map_4k_page(addr, addr, page_flags);
            addr += PAGE_SIZE;
        }
    }
}

/* ── Public API ──────────────────────────────────────────── */

int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (!kernel_l1) return -1;
    map_4k_page(virt, phys, flags | DESC_VALID | DESC_PAGE | DESC_AF);
    tlb_invalidate_all();
    return 0;
}

int paging_unmap_page(uint64_t virt)
{
    if (!kernel_l1) return -1;

    int l1i = L1_INDEX(virt);
    if (!(kernel_l1[l1i] & DESC_VALID)) return -1;
    uint64_t *l2 = (uint64_t *)(kernel_l1[l1i] & DESC_ADDR_MASK);

    int l2i = L2_INDEX(virt);
    if (!(l2[l2i] & DESC_VALID)) return -1;
    if (!(l2[l2i] & DESC_TABLE)) return -1;  /* Can't unmap within 2MB block */
    uint64_t *l3 = (uint64_t *)(l2[l2i] & DESC_ADDR_MASK);

    l3[L3_INDEX(virt)] = 0;
    tlb_invalidate_all();
    return 0;
}

int paging_map_mmio(uint64_t phys, uint64_t size)
{
    if (!kernel_l1) return -1;
    identity_map_range(phys, phys + size, DEVICE_BLOCK, DEVICE_PAGE);
    tlb_invalidate_all();
    return 0;
}

uint64_t paging_get_kernel_ttbr0(void)
{
    return kernel_ttbr0;
}

/* ── Initialize paging + enable MMU ──────────────────────── */

void paging_init(void)
{
    serial_puts("[PAGE] Setting up AArch64 page tables...\n");

    pt_pages_used = 0;

    /* Allocate L1 table (top-level for 39-bit VA) */
    kernel_l1 = pt_alloc_page();
    if (!kernel_l1) {
        serial_puts("[PAGE] FATAL: Cannot allocate L1 table\n");
        return;
    }

    /* ── Identity map first 1GB as device memory (2MB blocks) ──
     * Covers GIC (0x08000000), UART (0x09000000), VirtIO (0x0a000000),
     * PCI ECAM/MMIO (0x10000000-0x3f000000).
     * Using 2MB granularity only — no L3 tables for initial bring-up. */
    {
        uint64_t *l2_dev = pt_alloc_page();
        if (!l2_dev) { serial_puts("[PAGE] FATAL: L2 alloc\n"); return; }
        kernel_l1[0] = (uint64_t)l2_dev | TABLE_DESC;
        for (uint64_t i = 0; i < 512; i++)
            l2_dev[i] = (i * LARGE_PAGE_SIZE) | DEVICE_BLOCK;
    }

    /* ── Identity map RAM (normal memory, 2MB blocks) ── */
    uint64_t ram_start = platform_ram_base();
    uint64_t ram_end = ram_start + platform_ram_size();
    serial_puts("[PAGE] RAM: ");
    serial_puthex(ram_start, 8);
    serial_puts("-");
    serial_puthex(ram_end, 8);
    serial_puts("\n");
    {
        uint64_t l1_idx = L1_INDEX(ram_start);
        uint64_t *l2_ram = pt_alloc_page();
        if (!l2_ram) { serial_puts("[PAGE] FATAL: L2 alloc\n"); return; }
        kernel_l1[l1_idx] = (uint64_t)l2_ram | TABLE_DESC;
        /* Map all 2MB blocks covering RAM */
        uint64_t base_1g = l1_idx * (1ULL << 30);  /* 1GB base for this L1 entry */
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t addr = base_1g + i * LARGE_PAGE_SIZE;
            if (addr >= ram_start && addr < ram_end)
                l2_ram[i] = addr | NORMAL_BLOCK;
            /* else: leave as 0 (invalid) */
        }
    }

    /* ── High PCI ECAM (0x4010000000, 256MB) ── */
    {
        uint64_t l1_idx = L1_INDEX(0x4010000000ULL);
        uint64_t *l2_pci = pt_alloc_page();
        if (!l2_pci) { serial_puts("[PAGE] FATAL: L2 alloc\n"); return; }
        kernel_l1[l1_idx] = (uint64_t)l2_pci | TABLE_DESC;
        uint64_t base_1g = l1_idx * (1ULL << 30);
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t addr = base_1g + i * LARGE_PAGE_SIZE;
            if (addr >= 0x4010000000ULL && addr < 0x4020000000ULL)
                l2_pci[i] = addr | DEVICE_BLOCK;
        }
    }

    serial_puts("[PAGE] Tables built: ");
    serial_putdec(pt_pages_used);
    serial_puts(" pages (");
    serial_putdec(pt_pages_used * 4);
    serial_puts(" KB)\n");

    /* ── Configure MMU registers and enable ── */

    kernel_ttbr0 = (uint64_t)kernel_l1;

    irq_disable();

    /* MAIR: Attr0 = normal cacheable, Attr1 = device */
    write_mair_el1(MAIR_VALUE);
    __asm__ volatile("isb");

    /* TCR: 39-bit VA, 4KB granule, inner shareable, WB cacheable */
    write_tcr_el1(TCR_VALUE);
    __asm__ volatile("isb");

    /* TTBR0: point to our L1 table */
    write_ttbr0_el1(kernel_ttbr0);
    __asm__ volatile("isb");

    /* Invalidate all TLB entries */
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");

    serial_puts("[PAGE] Enabling MMU...\n");

    /* Ensure page table writes are visible to table walker */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Enable MMU (SCTLR_EL1.M=1), keep caches on */
    uint64_t sctlr = read_sctlr_el1();
    sctlr |= (1ULL << 0);   /* M: MMU enable */
    sctlr |= (1ULL << 2);   /* C: Data cache enable */
    sctlr |= (1ULL << 12);  /* I: Instruction cache enable */
    write_sctlr_el1(sctlr);
    __asm__ volatile("isb");

    irq_enable();

    serial_puts("[PAGE] MMU enabled — identity mapping active\n");
}
