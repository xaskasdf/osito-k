/*
 * usb.c -- SM8350 USB controllers (usb_1 at 0x0A600000, usb_2 at 0x0A800000)
 *
 * usb_1 drives the side USB-C port, the one wired to the dev PC (the bottom
 * port, usb_2, is damaged on our unit). ABL leaves usb_1 powered, clocked and
 * in device mode after fastboot; usb_2 is off. This file reports that state,
 * checks that the controller's DMA reaches memory 1:1 through the apps SMMU,
 * and starts the CDC-ACM gadget (dwc3_acm.c).
 *
 * The DWC3 DMAs through the SMMU as stream 0x0 (usb_1). ABL maps that stream
 * to a stage-1 context bank; the gadget only starts if that context bank has
 * translation off or maps the DMA buffer onto itself.
 *
 * DWC3 registers are only read when the power domain and master clock are
 * on; an unclocked access can hang the bus.
 *
 * Offsets: Linux drivers/clk/qcom/gcc-sm8350.c, sm8350.dtsi, dwc3-qcom.c,
 * drivers/iommu/arm/arm-smmu/arm-smmu.h.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "usb.h"

#define GCC_BASE            0x00100000UL
#define GCC_USB_PRIM        0x0F000     /* usb_1 block, BCR at +0 */
#define GCC_USB_SEC         0x10000     /* usb_2 block */
#define GCC_USB_GDSCR       0x004
#define GCC_USB_MASTER_CBCR 0x010
#define GCC_USB_SLEEP_CBCR  0x018
#define GCC_USB_UTMI_CBCR   0x01C
#define GCC_USB_NOC_CBCR    0x07C       /* cfg_noc_usb3_*_axi */
#define GCC_USB_AGGRE_CBCR  0x080       /* aggre_usb3_*_axi */

#define GDSC_PWR_ON         (1U << 31)
#define CBCR_CLK_OFF        (1U << 31)
#define CBCR_CLK_ENABLE     (1U << 0)

#define USB1_BASE           0x0A600000UL
#define USB1_SID            0x00
#define DWC3_GCTL           0xC110
#define DWC3_GSNPSID        0xC120
#define DWC3_DCTL           0xC704
#define DWC3_DSTS           0xC70C

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

#define USB_DMA_PAGES       4           /* 16 KB, see dwc3_acm.c */

static uint32_t gcc_read(uint32_t off)
{
    return mmio_read32(GCC_BASE + off);
}

static int clk_on(uint32_t cbcr)
{
    return !(gcc_read(cbcr) & CBCR_CLK_OFF);
}

static uint32_t smmu_read(uint32_t off)
{
    return mmio_read32(APPS_SMMU_BASE + off);
}

static uint64_t read64(uint64_t pa)
{
    return mmio_read32(pa) | ((uint64_t)mmio_read32(pa + 4) << 32);
}

static void put_bit(const char *name, int on)
{
    serial_puts(" ");
    serial_puts(name);
    serial_putc('=');
    serial_putc(on ? '1' : '0');
}

static void put_reg(const char *name, uint32_t val)
{
    serial_puts(" ");
    serial_puts(name);
    serial_puts("=");
    serial_puthex(val, 8);
}

/* Returns 1 if the controller is powered and its master clock runs */
static int report_controller(const char *name, uint32_t blk)
{
    int gdsc   = (gcc_read(blk + GCC_USB_GDSCR) & GDSC_PWR_ON) != 0;
    int master = clk_on(blk + GCC_USB_MASTER_CBCR);

    serial_puts("[USB ] ");
    serial_puts(name);
    put_bit("gdsc", gdsc);
    put_bit("master", master);
    put_bit("sleep", clk_on(blk + GCC_USB_SLEEP_CBCR));
    put_bit("utmi", clk_on(blk + GCC_USB_UTMI_CBCR));
    put_bit("noc", clk_on(blk + GCC_USB_NOC_CBCR));
    put_bit("aggre", clk_on(blk + GCC_USB_AGGRE_CBCR));
    serial_puts("\n");
    return gdsc && master;
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

/* Can the controller with stream `sid` DMA to [va, va + len) 1:1? */
static int smmu_dma_identity(uint32_t sid, uint64_t va, uint64_t len)
{
    uint32_t idr0 = smmu_read(SMMU_IDR0), idr1 = smmu_read(SMMU_IDR1);
    uint32_t nsmr = idr0 & 0xFF, i;

    for (i = 0; i < nsmr; i++) {
        uint32_t smr = smmu_read(SMMU_SMR(i));
        uint32_t mask = (smr >> 16) & 0x7FFF;
        if ((smr & (1U << 31)) && !((smr ^ sid) & 0x7FFF & ~mask))
            break;
    }
    if (i == nsmr) {
        int fault = (smmu_read(SMMU_SCR0) & SCR0_USFCFG) != 0;
        serial_puts("[SMMU] usb_1: no SMR, unmatched streams ");
        if (fault)
            serial_puts("fault\n");
        else
            serial_puts("bypass\n");
        return !fault;
    }

    uint32_t s2cr = smmu_read(SMMU_S2CR(i));
    uint32_t type = (s2cr >> 16) & 3, cb = s2cr & 0xFF;
    if (type == 1) {
        serial_puts("[SMMU] usb_1: S2CR bypass\n");
        return 1;
    }
    if (type != 0) {
        serial_puts("[SMMU] usb_1: S2CR fault\n");
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

    serial_puts("[SMMU] usb_1: cb ");
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

void usb_probe(void)
{
    int up = report_controller("usb_1", GCC_USB_PRIM);
    report_controller("usb_2", GCC_USB_SEC);
    if (!up) {
        serial_puts("[USB ] usb_1 powered down, no gadget\n");
        return;
    }
    serial_puts("[USB ] ");
    put_reg("id", mmio_read32(USB1_BASE + DWC3_GSNPSID));
    put_reg("gctl", mmio_read32(USB1_BASE + DWC3_GCTL));
    put_reg("dsts", mmio_read32(USB1_BASE + DWC3_DSTS));
    serial_puts("\n");

    /* Linux runs the controller with mock_utmi on; ABL leaves it off */
    uint32_t utmi = GCC_BASE + GCC_USB_PRIM + GCC_USB_UTMI_CBCR;
    mmio_write32(utmi, mmio_read32(utmi) | CBCR_CLK_ENABLE);
    udelay(10);
    serial_puts("[USB ] mock_utmi");
    put_bit("on", clk_on(GCC_USB_PRIM + GCC_USB_UTMI_CBCR));
    serial_puts("\n");

    void *buf = mem_alloc_pages(USB_DMA_PAGES);
    if (!buf) {
        serial_puts("[USB ] no memory for DMA\n");
        return;
    }
    uint64_t dma = (uint64_t)(uintptr_t)buf;
    memset(buf, 0, USB_DMA_PAGES * 4096);

    if (!smmu_dma_identity(USB1_SID, dma, USB_DMA_PAGES * 4096)) {
        serial_puts("[USB ] DMA not 1:1 through the SMMU, gadget not started\n");
        return;
    }
    acm_start(USB1_BASE, dma);
}
