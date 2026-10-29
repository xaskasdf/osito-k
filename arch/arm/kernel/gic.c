/*
 * gic.c -- GICv3 Interrupt Controller for OsitoK AArch64
 *
 * Initializes distributor (GICD) and, on each CPU, its redistributor (GICR):
 * enables the timer PPI (INTID 27) and the yield and stop SGIs (INTIDs 0, 1).
 * Uses ICC system registers for CPU interface (not MMIO GICC).
 */

#include "../include/hal.h"
#include "../include/aarch64.h"

/* GIC MMIO as single ldr/str on a base register, in asm so the compiler
 * can't pick another form. The SM8350 hypervisor traps GIC accesses and
 * emulates them from the abort syndrome, which a write-back or pair access
 * doesn't have: a post-indexed `str w0, [x1], #4` that gcc made of the
 * IPRIORITYR loop came back as a synchronous external abort (ESR
 * 0x96000010). Linux's readl/writel are asm for the same reason. */
static inline uint32_t mmio_rd32(uint64_t addr) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}
static inline uint64_t mmio_rd64(uint64_t addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}
static inline void mmio_wr32(uint64_t addr, uint32_t v) {
    __asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(addr) : "memory");
}

/* MMIO accessors using platform-provided base addresses */
static inline uint32_t gicd_read(uint32_t off) {
    return mmio_rd32(platform_gicd_base() + off);
}
static inline void gicd_write(uint32_t off, uint32_t val) {
    mmio_wr32(platform_gicd_base() + off, val);
}
static inline uint32_t gicr_read(uint64_t rd, uint32_t off) {
    return mmio_rd32(rd + off);
}
static inline void gicr_write(uint64_t rd, uint32_t off, uint32_t val) {
    mmio_wr32(rd + off, val);
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
#define GICR_TYPER          0x008       /* 64-bit */
#define GICR_TYPER_VLPIS    (1ULL << 1) /* 4 frames per CPU instead of 2 */
#define GICR_TYPER_LAST     (1ULL << 4)
#define GICR_SGI_BASE       0x10000
#define GICR_ISENABLER0     (GICR_SGI_BASE + 0x100)
#define GICR_ICENABLER0     (GICR_SGI_BASE + 0x180)
#define GICR_ICPENDR0       (GICR_SGI_BASE + 0x280)
#define GICR_ICACTIVER0     (GICR_SGI_BASE + 0x380)
#define GICR_IPRIORITYR(n)  (GICR_SGI_BASE + 0x400 + (n)*4)
#define GICR_IGROUPR0       (GICR_SGI_BASE + 0x080)
#define GICR_IGRPMODR0      (GICR_SGI_BASE + 0xD00)

#define TIMER_PPI       27  /* Virtual timer (CNTV) — EL2 traps physical PPI 30 */

/* This CPU's redistributor: the frame whose GICR_TYPER affinity matches
 * MPIDR (Aff3.Aff2.Aff1.Aff0). 0 if none. */
static uint64_t gicr_this_cpu(void) {
    uint64_t mpidr = read_mpidr_el1();
    uint32_t aff = (uint32_t)(((mpidr >> 8) & 0xFF000000) | (mpidr & 0xFFFFFF));
    uint64_t rd = platform_gicr_base();
    for (int n = 0; n < 64; n++) {
        uint64_t typer = mmio_rd64(rd + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff)
            return rd;
        if (typer & GICR_TYPER_LAST)
            break;
        rd += (typer & GICR_TYPER_VLPIS) ? 0x40000 : 0x20000;
    }
    return 0;
}

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

    int err = gic_init_cpu();
    if (err < 0) {
        serial_puts(err == -1 ? "[GIC ] no redistributor for this CPU!\n"
                              : "[GIC ] redistributor didn't wake!\n");
        return;
    }
    serial_puts("[GIC ] GICv3 ready (redistributor ");
    serial_puthex(gicr_this_cpu(), 8);
    serial_puts(", PPI ");
    serial_putdec(TIMER_PPI);
    serial_puts(", SGIs 0-1 enabled)\n");
}

/* Redistributor + CPU interface of the calling CPU. Clears what a previous
 * kernel may have left pending or active (kload). */
int gic_init_cpu(void) {
    uint64_t rd = gicr_this_cpu();
    if (!rd)
        return -1;

    uint32_t waker = gicr_read(rd, GICR_WAKER);
    waker &= ~(1 << 1);  /* Clear ProcessorSleep */
    gicr_write(rd, GICR_WAKER, waker);
    for (int i = 0; gicr_read(rd, GICR_WAKER) & (1 << 2); i++) {
        if (i == 100000)
            return -2;   /* ChildrenAsleep never cleared */
        udelay(1);
    }

    /* SGIs + PPIs: Group 1 NS, priority 0xA0 */
    gicr_write(rd, GICR_ICENABLER0, 0xFFFFFFFF);
    gicr_write(rd, GICR_ICPENDR0, 0xFFFFFFFF);
    gicr_write(rd, GICR_ICACTIVER0, 0xFFFFFFFF);
    gicr_write(rd, GICR_IGROUPR0, 0xFFFFFFFF);
    gicr_write(rd, GICR_IGRPMODR0, 0x00000000);
    for (uint32_t i = 0; i < 8; i++)
        gicr_write(rd, GICR_IPRIORITYR(i), 0xA0A0A0A0);

    gicr_write(rd, GICR_ISENABLER0,
               (1 << TIMER_PPI) | (1 << SGI_YIELD) | (1 << SGI_STOP));
    __asm__ volatile("isb");

    /* ── CPU interface (ICC system registers) ─────────── */
    write_icc_sre_el1(read_icc_sre_el1() | 1);  /* Enable SRE */
    __asm__ volatile("isb");
    write_icc_pmr_el1(0xFF);         /* Allow all priorities */
    write_icc_igrpen1_el1(1);        /* Enable Group 1 */
    __asm__ volatile("isb");
    return 0;
}

uint32_t gic_ack_irq(void) {
    return (uint32_t)read_icc_iar1_el1();
}

void gic_end_irq(uint32_t irqnr) {
    write_icc_eoir1_el1(irqnr);
}

/* ICC_SGI1R_EL1 addresses one cluster (Aff3.Aff2.Aff1) and a target list
 * of Aff0 values 0-15 */
void gic_send_sgi(uint64_t mpidr, uint32_t sgi_id) {
    uint64_t val = (((mpidr >> 32) & 0xFF) << 48) |     /* Aff3 */
                   (((mpidr >> 16) & 0xFF) << 32) |     /* Aff2 */
                   ((uint64_t)(sgi_id & 0xF) << 24) |
                   (((mpidr >> 8) & 0xFF) << 16) |      /* Aff1 */
                   (1ULL << (mpidr & 0xF));             /* Aff0 */
    __asm__ volatile("dsb ishst" ::: "memory");   /* stores before the SGI */
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(val));
    __asm__ volatile("isb");
}

void gic_send_sgi_self(uint32_t sgi_id) {
    gic_send_sgi(read_mpidr_el1(), sgi_id);
}
