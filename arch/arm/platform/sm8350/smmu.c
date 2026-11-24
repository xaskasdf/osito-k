/*
 * smmu.c -- the apps SMMU (0x15000000) as ABL leaves it, for DMA masters
 *
 * Osito-K doesn't program the SMMU. A master that DMAs (USB as stream 0x0,
 * UFS as 0xE0) only works if ABL left its stream bypassing, in a context
 * bank with stage-1 translation off, or mapped 1:1 over the buffers we use.
 * smmu_dma_identity checks which, walking AArch64 stage-1 tables if it must.
 *
 * Offsets: Linux drivers/iommu/arm/arm-smmu/arm-smmu.h.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "smmu.h"

#define APPS_SMMU_BASE      0x15000000UL
#define SMMU_SCR0           0x000
#define SMMU_IDR0           0x020
#define SMMU_IDR1           0x024
#define SMMU_SMR(n)         (0x800 + 4 * (n))
#define SMMU_S2CR(n)        (0xC00 + 4 * (n))
#define SMMU_GR1_CBAR(n)    (4 * (n))
#define SMMU_GR1_CBA2R(n)   (0x800 + 4 * (n))
#define SMMU_CB_SCTLR       0x000
#define SMMU_CB_TTBR0       0x020
#define SMMU_CB_TCR         0x030
#define SCR0_USFCFG         (1U << 10)  /* unmatched streams fault */

static uint32_t smmu_read(uint32_t off)
{
    return mmio_read32(APPS_SMMU_BASE + off);
}

static uint64_t read64(uint64_t pa)
{
    return mmio_read32(pa) | ((uint64_t)mmio_read32(pa + 4) << 32);
}

static void put_reg(const char *name, uint32_t val)
{
    serial_puts(" ");
    serial_puts(name);
    serial_puts("=");
    serial_puthex(val, 8);
}

/* Stage-1 walk of an AArch64-format context bank with a 4 KB granule */
static int smmu_walk(uint64_t ttbr0, uint32_t tcr, uint64_t va, uint64_t *pa)
{
    if (((tcr >> 14) & 3) != 0)
        return 0;                               /* TG0 not 4 KB */
    uint32_t va_bits = 64 - (tcr & 0x3F);
    int level = va_bits > 39 ? 0 : (va_bits > 30 ? 1 : 2);
    uint64_t table = ttbr0 & 0x0000FFFFFFFFFFF0ULL;

    for (; level <= 3; level++) {
        uint32_t shift = 12 + 9 * (3 - level);
        uint64_t desc = read64(table + ((va >> shift) & 0x1FF) * 8);
        if (!(desc & 1))
            return 0;
        uint64_t out = desc & 0x0000FFFFFFFFF000ULL;
        if (level == 3 || !(desc & 2)) {        /* page, or block at L1/L2 */
            if (level == 0 || (level == 3 && !(desc & 2)))
                return 0;
            uint64_t mask = (1ULL << shift) - 1;
            *pa = (out & ~mask) | (va & mask);
            return 1;
        }
        table = out;
    }
    return 0;
}

static int check_va(uint64_t ttbr0, uint32_t tcr, uint64_t va)
{
    uint64_t pa = 0;
    int ok = smmu_walk(ttbr0, tcr, va, &pa);
    serial_puts("[SMMU]   ");
    serial_puthex(va, 10);
    if (!ok) {
        serial_puts(" -> unmapped\n");
        return 0;
    }
    serial_puts(" -> ");
    serial_puthex(pa, 10);
    serial_puts("\n");
    return pa == va;
}

int smmu_dma_identity(const char *name, uint32_t sid, uint64_t va, uint64_t len)
{
    uint32_t idr0 = smmu_read(SMMU_IDR0), idr1 = smmu_read(SMMU_IDR1);
    uint32_t nsmr = idr0 & 0xFF, i;

    for (i = 0; i < nsmr; i++) {
        uint32_t smr = smmu_read(SMMU_SMR(i));
        uint32_t mask = (smr >> 16) & 0x7FFF;
        if ((smr & (1U << 31)) && !((smr ^ sid) & 0x7FFF & ~mask))
            break;
    }
    serial_puts("[SMMU] ");
    serial_puts(name);
    if (i == nsmr) {
        int fault = (smmu_read(SMMU_SCR0) & SCR0_USFCFG) != 0;
        serial_puts(": no SMR, unmatched streams ");
        serial_puts(fault ? "fault\n" : "bypass\n");
        return !fault;
    }

    uint32_t s2cr = smmu_read(SMMU_S2CR(i));
    uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xFF;
    if (type == 1) {
        serial_puts(": S2CR bypass\n");
        return 1;
    }
    if (type != 0) {
        serial_puts(": S2CR fault\n");
        return 0;
    }

    uint32_t pgshift = (idr1 >> 31) ? 16 : 12;
    uint64_t numpage = 1ULL << (((idr1 >> 28) & 7) + 1);
    uint64_t gr1 = APPS_SMMU_BASE + (1ULL << pgshift);
    uint64_t cbb = APPS_SMMU_BASE + ((numpage + cb) << pgshift);
    uint32_t cbar = mmio_read32(gr1 + SMMU_GR1_CBAR(cb));
    uint32_t cba2r = mmio_read32(gr1 + SMMU_GR1_CBA2R(cb));
    uint32_t sctlr = mmio_read32(cbb + SMMU_CB_SCTLR);
    uint32_t tcr = mmio_read32(cbb + SMMU_CB_TCR);
    uint64_t ttbr0 = read64(cbb + SMMU_CB_TTBR0);

    serial_puts(": cb ");
    serial_putdec(cb);
    put_reg("cbar", cbar);
    put_reg("cba2r", cba2r);
    serial_puts("\n[SMMU]  ");
    put_reg("sctlr", sctlr);
    put_reg("tcr", tcr);
    serial_puts(" ttbr0=");
    serial_puthex(ttbr0, 10);
    serial_puts("\n");

    if (((cbar >> 16) & 3) == 0) {
        serial_puts("[SMMU] stage-2-only context bank, not walking it\n");
        return 0;
    }
    if (!(sctlr & 1)) {
        serial_puts("[SMMU] stage-1 translation off: 1:1\n");
        return 1;
    }
    if (!(cba2r & 1)) {
        serial_puts("[SMMU] AArch32 page-table format, not walking it\n");
        return 0;
    }
    return check_va(ttbr0, tcr, va) && check_va(ttbr0, tcr, va + len - 1);
}
