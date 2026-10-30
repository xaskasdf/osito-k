/*
 * mmu.c -- SM8350 stage-1 translation, caches and the uncached DMA window
 *
 * The shim enters Osito-K with the MMU and D-cache off, so every access was
 * Device-nGnRnE: uncached, unmerged, and faulting when unaligned. This sets
 * up a flat map (VA == PA) with the caches on:
 *
 *   0x00000000-0x3FFFFFFF  Device-nGnRE  MMIO (one 1 GB block)
 *   0x80800000-0x809FFFFF  Device-nGnRE  holds the AOP's command DB
 *                                        (0x80860000, rpmh.c reads it)
 *   0xA0000000-0xB4DFFFFF  Normal WB     kernel, heap, kload buffers
 *   0xB4E00000-0xB4FFFFFF  Normal NC     DMA window (USB event buffer, TRBs)
 *   0xE5000000-0xE5BFFFFF  Normal NC     splash framebuffer (writes merge)
 *   high_ram[] below       Normal WB, XN the rest of the 16 GB: high memory
 *                                        for big allocations (models, put)
 *
 * High memory is only ever written through the cache and no device DMAs
 * into it, so the handoff (mmu_off_and_jump) needn't clean it: a line it
 * leaves dirty holds what the next kernel would read anyway.
 *
 * Nothing else is mapped. The firmware carve-outs must stay out: Normal
 * memory can be read speculatively and those regions belong to other
 * processors. Device memory is never speculated, so the holes in the MMIO
 * gigabyte are harmless, and so is mapping the command DB's block (the
 * rest of it is SMEM and the XBL log, never touched).
 *
 * 39-bit VA (T0SZ = 25) starts at level 1 (1 GB per entry); the DDR and
 * framebuffer gigabytes use level-2 tables of 2 MB blocks. Tables are static.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "mmu.h"

#define DESC_BLOCK      0x1ULL
#define DESC_TABLE      0x3ULL
#define DESC_ATTR(n)    ((uint64_t)(n) << 2)
#define DESC_SH_INNER   (3ULL << 8)
#define DESC_AF         (1ULL << 10)
#define DESC_XN         ((1ULL << 53) | (1ULL << 54))   /* PXN | UXN */

#define ATTR_DEVICE     0           /* MAIR_EL1 indices */
#define ATTR_NORMAL_WB  1
#define ATTR_NORMAL_NC  2
#define MAIR_VALUE      ((0x04ULL << (8 * ATTR_DEVICE)) |       /* Device-nGnRE */ \
                         (0xFFULL << (8 * ATTR_NORMAL_WB)) |    /* WB, RA/WA */ \
                         (0x44ULL << (8 * ATTR_NORMAL_NC)))     /* non-cacheable */

#define TCR_VALUE       (25ULL |                /* T0SZ: 39-bit VA */ \
                         (1ULL << 8) |          /* IRGN0: WB RA/WA */ \
                         (1ULL << 10) |         /* ORGN0: WB RA/WA */ \
                         (3ULL << 12) |         /* SH0: inner shareable */ \
                         (0ULL << 14) |         /* TG0: 4 KB */ \
                         (1ULL << 23) |         /* EPD1: no TTBR1 walks */ \
                         (2ULL << 32))          /* IPS: 40-bit PA */

#define SCTLR_M         (1ULL << 0)
#define SCTLR_C         (1ULL << 2)
#define SCTLR_I         (1ULL << 12)

#define L2_DRAM_BASE    0x80000000UL
#define CMD_DB_BLOCK    0x80800000UL            /* the 2 MB with cmd-db */
#define L2_FB_BASE      0xC0000000UL
#define FB_BASE         0xE5000000UL
#define FB_MAPPED       0x00C00000UL            /* 12 MB >= 1080 x 2448 x 4 */
#define BLOCK_2M        0x200000UL
#define BLOCK_1G        0x40000000UL

/* The RAM beyond bank 0 (the device tree's memory node: 8 GB at 32 GB, 7 GB
 * at 3 GB), less bank 2's carve-outs: removed_region 0xD8800000-0xE3000000,
 * splash_region 0xE5000000-0xE7300000 (the framebuffer), dfps_data_region
 * 0xE7400000-0xE7500000. 2 MB blocks in the framebuffer's gigabyte, 1 GB
 * blocks elsewhere. The 8 GB bank first: it holds the biggest runs. */
static const struct {
    uint64_t base, size;
} high_ram[] = {
    { 0x800000000UL, 0x200000000UL },   /* bank 1, 8 GB, no carve-outs */
    { 0x0C0000000UL, 0x018800000UL },   /* bank 2: 392 MB to removed_region */
    { 0x0E3000000UL, 0x002000000UL },   /* 32 MB up to the splash */
    { 0x0E7600000UL, 0x018A00000UL },   /* 394 MB from past dfps_data to 4 GB */
    { 0x100000000UL, 0x180000000UL },   /* 6 GB above 4 GB */
};

static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2_dram[512] __attribute__((aligned(4096)));
static uint64_t l2_fb[512] __attribute__((aligned(4096)));

static uint32_t dma_used;

static uint64_t block_desc(uint64_t pa, int attr)
{
    uint64_t d = pa | DESC_BLOCK | DESC_ATTR(attr) | DESC_AF;
    if (attr == ATTR_DEVICE || attr == ATTR_NORMAL_NC)
        d |= DESC_XN;
    if (attr != ATTR_DEVICE)
        d |= DESC_SH_INNER;
    return d;
}

static void map_2m(uint64_t *l2, uint64_t l2_base, uint64_t start, uint64_t end, int attr)
{
    for (uint64_t pa = start; pa < end; pa += BLOCK_2M)
        l2[(pa - l2_base) / BLOCK_2M] = block_desc(pa, attr);
}

static uint32_t dcache_line(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4U << ((ctr >> 16) & 0xF);
}

static uint64_t cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

void platform_mmu_init(void)
{
    l1[0] = block_desc(0, ATTR_DEVICE);
    l1[2] = (uint64_t)(uintptr_t)l2_dram | DESC_TABLE;
    l1[3] = (uint64_t)(uintptr_t)l2_fb | DESC_TABLE;
    map_2m(l2_dram, L2_DRAM_BASE, CMD_DB_BLOCK, CMD_DB_BLOCK + BLOCK_2M, ATTR_DEVICE);
    map_2m(l2_dram, L2_DRAM_BASE, RAM_CACHED_BASE, DMA_WINDOW_BASE, ATTR_NORMAL_WB);
    map_2m(l2_dram, L2_DRAM_BASE, DMA_WINDOW_BASE, RAM_END, ATTR_NORMAL_NC);
    map_2m(l2_fb, L2_FB_BASE, FB_BASE, FB_BASE + FB_MAPPED, ATTR_NORMAL_NC);
    for (uint32_t r = 0; r < sizeof(high_ram) / sizeof(high_ram[0]); r++) {
        uint64_t pa = high_ram[r].base, end = pa + high_ram[r].size;
        for (; pa < end; ) {
            if (!(pa & (BLOCK_1G - 1)) && end - pa >= BLOCK_1G) {
                l1[pa / BLOCK_1G] = block_desc(pa, ATTR_NORMAL_WB) | DESC_XN;
                pa += BLOCK_1G;
            } else {                    /* only in the framebuffer's gigabyte */
                l2_fb[(pa - L2_FB_BASE) / BLOCK_2M] = block_desc(pa, ATTR_NORMAL_WB) | DESC_XN;
                pa += BLOCK_2M;
            }
        }
    }

    /* The page allocator must not hand out the DMA window */
    mem_reserve_range(DMA_WINDOW_BASE, DMA_WINDOW_SIZE / 4096);

    /* Everything here was written with the D-cache off. Drop any line the
     * cache may still hold for it (from ABL, or from a previous kernel if
     * kload's clean missed something) before turning the cache on. */
    uint64_t t0 = cntpct();
    uint32_t line = dcache_line();
    for (uint64_t va = RAM_CACHED_BASE; va < RAM_END; va += line)
        __asm__ volatile("dc ivac, %0" : : "r"(va) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
    uint64_t t1 = cntpct();

    __asm__ volatile(
        "msr  mair_el1, %0\n"
        "msr  tcr_el1, %1\n"
        "msr  ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb  ish\n"
        "isb\n"
        : : "r"(MAIR_VALUE), "r"(TCR_VALUE), "r"((uint64_t)(uintptr_t)l1) : "memory");

    uint64_t sctlr_old, sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr_old));
    sctlr = sctlr_old | SCTLR_M | SCTLR_C | SCTLR_I;
    __asm__ volatile("msr sctlr_el1, %0\nisb" : : "r"(sctlr) : "memory");

    serial_puts("[MMU ] on: sctlr ");
    serial_puthex(sctlr_old, 8);
    serial_puts(" -> ");
    serial_puthex(sctlr, 8);
    serial_puts(", D-cache invalidate ");
    serial_putdec((t1 - t0) * 1000 / cntfrq());
    serial_puts(" ms\n");

    /* Hand each range over as far as it reads back: a word at its start and
     * at each gigabyte boundary in it, written and read back */
    for (uint32_t r = 0; r < sizeof(high_ram) / sizeof(high_ram[0]); r++) {
        uint64_t base = high_ram[r].base, end = base + high_ram[r].size, good = base;
        while (good < end) {
            volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)good;
            uint64_t pattern = 0x05170C0DE0000000ULL | good;
            *w = pattern;
            if (*w != pattern)
                break;
            uint64_t next = (good & ~(BLOCK_1G - 1)) + BLOCK_1G;
            good = next < end ? next : end;
        }
        if (good > base)
            mem_add_high(base, good - base);
        if (good < end) {
            serial_puts("[MMU ] high memory at ");
            serial_puthex(good, 10);
            serial_puts(" doesn't read back, stopping there\n");
        }
    }
}

void *dma_alloc(uint32_t bytes)
{
    uint32_t off = (dma_used + 63) & ~63U;
    if (off + bytes > DMA_WINDOW_SIZE)
        return (void *)0;
    dma_used = off + bytes;
    void *p = (void *)(uintptr_t)(DMA_WINDOW_BASE + off);
    memset(p, 0, bytes);
    return p;
}

void mmu_off_and_jump(uint64_t entry)
{
    register uint64_t target __asm__("x1") = entry;
    register uint64_t va __asm__("x2") = RAM_CACHED_BASE;
    register uint64_t end __asm__("x3") = DMA_WINDOW_BASE;
    register uint64_t line __asm__("x4") = dcache_line();
    /* One asm block: no memory access may come between the clean and the
     * MMU going off, or it could leave a dirty line behind. */
    __asm__ volatile(
        "msr  daifset, #0xf\n"
        "msr  cntv_ctl_el0, xzr\n"      /* stop the scheduler tick */
        "1:   dc civac, x2\n"           /* the new Image, and every dirty line */
        "add  x2, x2, x4\n"
        "cmp  x2, x3\n"
        "b.lo 1b\n"
        "dsb  sy\n"
        "mrs  x5, sctlr_el1\n"
        "bic  x5, x5, #1\n"             /* M */
        "bic  x5, x5, #4\n"             /* C */
        "msr  sctlr_el1, x5\n"
        "isb\n"
        "ic   iallu\n"
        "tlbi vmalle1\n"
        "dsb  sy\n"
        "isb\n"
        "mov  x0, xzr\n"
        "br   x1\n"
        : "+r"(va) : "r"(target), "r"(end), "r"(line) : "x0", "x5", "memory");
    __builtin_unreachable();
}
