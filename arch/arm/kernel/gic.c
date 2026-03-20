/*
 * gic.c -- GICv3 Interrupt Controller for OsitoK AArch64
 *
 * Initializes distributor (GICD) and redistributor (GICR),
 * enables timer PPI (INTID 30) and yield SGI (INTID 0).
 * Uses ICC system registers for CPU interface (not MMIO GICC).
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

/* MMIO accessors using platform-provided base addresses */
static inline uint32_t gicd_read(uint32_t off) {
    return *(volatile uint32_t *)(platform_gicd_base() + off);
}
static inline void gicd_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(platform_gicd_base() + off) = val;
}
static inline uint32_t gicr_read(uint32_t off) {
    return *(volatile uint32_t *)(platform_gicr_base() + off);
}
static inline void gicr_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(platform_gicr_base() + off) = val;
}

/* GICD offsets */
#define GICD_CTLR           0x000
#define GICD_TYPER          0x004
#define GICD_ISENABLER(n)   (0x100 + (n)*4)
#define GICD_ICENABLER(n)   (0x180 + (n)*4)
#define GICD_IPRIORITYR(n)  (0x400 + (n)*4)
#define GICD_IGROUPR(n)     (0x080 + (n)*4)
#define GICD_IGRPMODR(n)    (0xD00 + (n)*4)

/* GICR offsets (redistributor, per-CPU) */
#define GICR_WAKER          0x014
#define GICR_SGI_BASE       0x10000
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x180)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x400 + (n)*4)
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x080)
#define GICR_IGRPMODR0      (GICR_SGI_BASE + 0xD00)

#define TIMER_PPI       30  /* Non-secure physical timer */
#define YIELD_SGI       0   /* SGI for task_yield */

void gic_init(void) {
    serial_puts("[GIC ] Initializing GICv3...\n");

    /* ── Distributor ──────────────────────────────────── */
    gicd_write(GICD_CTLR, 0);  /* Disable */
    __asm__ volatile("isb");

    uint32_t typer = gicd_read(GICD_TYPER);
    uint32_t max_irq = ((typer & 0x1F) + 1) * 32;
    serial_puts("[GIC ] Max IRQs: ");
    serial_putdec(max_irq);
    serial_puts("\n");

    /* Configure SPIs: disable all, Group 1 NS, priority 0xA0 */
    for (uint32_t i = 1; i < max_irq / 32; i++) {
        gicd_write(GICD_ICENABLER(i), 0xFFFFFFFF);
        gicd_write(GICD_IGROUPR(i), 0xFFFFFFFF);
        gicd_write(GICD_IGRPMODR(i), 0x00000000);
    }
    for (uint32_t i = 8; i < max_irq / 4; i++)
        gicd_write(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* Enable distributor: ARE_NS + EnableGrp1NS */
    gicd_write(GICD_CTLR, (1 << 4) | (1 << 1));
    __asm__ volatile("isb");

    /* ── Redistributor (CPU 0) ────────────────────────── */
    uint32_t waker = gicr_read(GICR_WAKER);
    waker &= ~(1 << 1);  /* Clear ProcessorSleep */
    gicr_write(GICR_WAKER, waker);
    while (gicr_read(GICR_WAKER) & (1 << 2))
        ;  /* Wait ChildrenAsleep to clear */

    /* SGIs + PPIs: Group 1 NS, priority 0xA0 */
    gicr_write(GICR_IGROUPR0, 0xFFFFFFFF);
    gicr_write(GICR_IGRPMODR0, 0x00000000);
    for (uint32_t i = 0; i < 8; i++)
        gicr_write(GICR_IPRIORITYR(i), 0xA0A0A0A0);

    /* Enable timer PPI 30 and SGI 0 */
    gicr_write(GICR_ISENABLER0, (1 << TIMER_PPI) | (1 << YIELD_SGI));
    __asm__ volatile("isb");

    /* ── CPU interface (ICC system registers) ─────────── */
    write_icc_sre_el1(read_icc_sre_el1() | 1);  /* Enable SRE */
    __asm__ volatile("isb");
    write_icc_pmr_el1(0xFF);         /* Allow all priorities */
    write_icc_igrpen1_el1(1);        /* Enable Group 1 */
    __asm__ volatile("isb");

    serial_puts("[GIC ] GICv3 ready (PPI ");
    serial_putdec(TIMER_PPI);
    serial_puts(" + SGI ");
    serial_putdec(YIELD_SGI);
    serial_puts(" enabled)\n");
}

uint32_t gic_ack_irq(void) {
    return (uint32_t)read_icc_iar1_el1();
}

void gic_end_irq(uint32_t irqnr) {
    write_icc_eoir1_el1(irqnr);
}

void gic_send_sgi_self(uint32_t sgi_id) {
    /* ICC_SGI1R_EL1: IRM=0, Aff3/2/1=0, TargetList=1 (self), INTID in bits [27:24] */
    uint64_t val = ((uint64_t)(sgi_id & 0xF) << 24) | 1UL;
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(val));
    __asm__ volatile("isb");
}
