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
 * Offsets: Linux drivers/clk/qcom/gcc-sm8350.c, sm8350.dtsi, dwc3-qcom.c.
 * The SMMU check lives in smmu.c.
 */

#include "../../include/hal.h"
#include "../../include/types.h"
#include "mmu.h"
#include "usb.h"
#include "smmu.h"

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

#define USB_DMA_PAGES       10          /* 40 KB, see dwc3_acm.c (DMA_SIZE) */

static uint32_t gcc_read(uint32_t off)
{
    return mmio_read32(GCC_BASE + off);
}

static int clk_on(uint32_t cbcr)
{
    return !(gcc_read(cbcr) & CBCR_CLK_OFF);
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

    /* Uncached: the controller's event writes must be visible to the CPU
     * and its TRB reads must see the CPU's stores, with no cache maintenance */
    void *buf = dma_alloc(USB_DMA_PAGES * 4096);
    if (!buf) {
        serial_puts("[USB ] no memory for DMA\n");
        return;
    }
    uint64_t dma = (uint64_t)(uintptr_t)buf;

    if (!smmu_dma_identity("usb_1", USB1_SID, dma, USB_DMA_PAGES * 4096)) {
        serial_puts("[USB ] DMA not 1:1 through the SMMU, gadget not started\n");
        return;
    }
    acm_start(USB1_BASE, dma);
}
