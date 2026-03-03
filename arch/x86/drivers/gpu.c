/*
 * OsitoK x86-64 — GPU MMIO Probe Driver (Phase 1 + 2)
 *
 * Phase 1: Read-only BAR0 probe — chip ID, engines, PTIMER, Falcons.
 * Phase 2: VRAM size, BAR1 read/write test, PRAMIN window read.
 *
 * NO WRITES to GPU control registers. BAR1 write test uses VRAM only.
 *
 * Reference: envytools (https://envytools.rtfd.io), nouveau driver.
 */

#include "../include/types.h"
#include "gpu.h"

/* ── External Functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_puthex(uint64_t val, int digits);
extern void fb_putdec(uint64_t val);

/* ── Driver State ────────────────────────────────────────────── */

typedef struct {
    volatile void *bar0;
    gpu_probe_t    probe;
} gpu_state_t;

static gpu_state_t gpu;

/* ── Register Access ─────────────────────────────────────────── */

static uint32_t gpu_read(uint32_t reg)
{
    return mmio_read32((volatile void *)((uint64_t)gpu.bar0 + reg));
}

/* ── Chip Name Lookup ────────────────────────────────────────── */
/* Uses if-chains (no switch) due to -fno-jump-tables constraint */

static const char *gpu_chip_name(uint32_t chip_id)
{
    /* Turing (RTX 20xx) */
    if (chip_id == 0x162) return "TU102 (RTX 2080 Ti)";
    if (chip_id == 0x164) return "TU104 (RTX 2080)";
    if (chip_id == 0x166) return "TU106 (RTX 2070)";
    if (chip_id == 0x168) return "TU116 (GTX 1660)";

    /* Ampere (RTX 30xx) */
    if (chip_id == 0x170) return "GA100";
    if (chip_id == 0x172) return "GA102 (RTX 3090)";
    if (chip_id == 0x174) return "GA104 (RTX 3070)";
    if (chip_id == 0x176) return "GA106 (RTX 3060)";
    if (chip_id == 0x177) return "GA107 (RTX 3050)";

    /* Ada Lovelace (RTX 40xx) */
    if (chip_id == 0x190) return "AD102 (RTX 4090)";
    if (chip_id == 0x192) return "AD103 (RTX 4080)";
    if (chip_id == 0x194) return "AD104 (RTX 4070 Ti)";
    if (chip_id == 0x196) return "AD106 (RTX 4060 Ti)";
    if (chip_id == 0x197) return "AD107 (RTX 4060)";

    /* Blackwell (RTX 50xx) */
    if (chip_id == 0x1B0) return "GB202 (RTX 5090)";
    if (chip_id == 0x1B2) return "GB203 (RTX 5080)";
    if (chip_id == 0x1B4) return "GB205 (RTX 5070 Ti)";
    if (chip_id == 0x1B6) return "GB206 (RTX 5070)";
    if (chip_id == 0x1B7) return "GB207 (RTX 5060)";

    return "Unknown";
}

/* ── Probe: Boot Registers ───────────────────────────────────── */

static int gpu_probe_boot(void)
{
    uint32_t boot0 = gpu_read(NV_PMC_BOOT_0);

    /* Validate BAR0 is accessible */
    if (boot0 == NV_DEAD_REG || boot0 == 0) {
        serial_puts("[GPU] BAR0 not accessible (boot0=");
        serial_puthex(boot0, 8);
        serial_puts(")\n");
        return -1;
    }

    gpu.probe.boot0    = boot0;
    gpu.probe.boot42   = gpu_read(NV_PMC_BOOT_42);
    gpu.probe.chip_id  = (boot0 >> 20) & 0xFFF;
    gpu.probe.chip_rev = boot0 & 0xF;

    return 0;
}

/* ── Probe: Active Engines ───────────────────────────────────── */

static void gpu_probe_engines(void)
{
    gpu.probe.engines = gpu_read(NV_PMC_ENABLE);
}

/* ── Probe: GPU Timer ────────────────────────────────────────── */

static void gpu_probe_timer(void)
{
    uint32_t hi1, lo, hi2;

    /* Read high-low-high to detect rollover */
    hi1 = gpu_read(NV_PTIMER_TIME_1);
    lo  = gpu_read(NV_PTIMER_TIME_0);
    hi2 = gpu_read(NV_PTIMER_TIME_1);

    /* If high changed between reads, re-read low */
    if (hi1 != hi2)
        lo = gpu_read(NV_PTIMER_TIME_0);

    gpu.probe.gpu_timer_ns = ((uint64_t)hi2 << 32) | lo;
}

/* ── Probe: Falcon Microcontrollers ──────────────────────────── */

static bool gpu_probe_falcon(uint32_t base)
{
    uint32_t hwcfg = gpu_read(base + NV_FALCON_HWCFG);
    return (hwcfg != NV_DEAD_REG && hwcfg != 0);
}

static void gpu_probe_falcons(void)
{
    gpu.probe.gsp_present  = gpu_probe_falcon(NV_PGSP_BASE);
    gpu.probe.sec2_present = gpu_probe_falcon(NV_PSEC_BASE);
    gpu.probe.pmu_present  = gpu_probe_falcon(NV_PPMU_BASE);
}

/* ── Phase 2: VRAM Discovery ─────────────────────────────────── */

static void gpu_probe_vram(void)
{
    uint32_t range = gpu_read(NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE);

    if (range == NV_DEAD_REG || range == 0) {
        gpu.probe.vram_size_mb = 0;
        return;
    }

    /* Bits 29:0 shifted left by 17 gives VRAM in bytes */
    uint64_t vram_bytes = (uint64_t)(range & 0x3FFFFFFF) << 17;
    gpu.probe.vram_size_mb = (uint32_t)(vram_bytes >> 20);
}

static void gpu_probe_bar1(uint64_t bar1_phys, uint64_t bar1_size)
{
    if (bar1_phys == 0 || bar1_size == 0) {
        serial_puts("[GPU] BAR1: not configured\n");
        return;
    }

    volatile uint32_t *bar1 = (volatile uint32_t *)bar1_phys;
    uint32_t d0, d1, d2, d3;

    rmb();
    d0 = bar1[0];
    d1 = bar1[1];
    d2 = bar1[2];
    d3 = bar1[3];

    serial_puts("[GPU] BAR1[0..3]: ");
    serial_puthex(d0, 8); serial_puts(" ");
    serial_puthex(d1, 8); serial_puts(" ");
    serial_puthex(d2, 8); serial_puts(" ");
    serial_puthex(d3, 8); serial_puts("\n");

    /* All 0xFFFFFFFF means BAR1 is not accessible */
    if (d0 == NV_DEAD_REG && d1 == NV_DEAD_REG &&
        d2 == NV_DEAD_REG && d3 == NV_DEAD_REG)
        return;

    gpu.probe.bar1_accessible = true;
}

#define BAR1_RW_TEST_OFFSET  (32 * 1024 * 1024)  /* 32MB past start */

static void gpu_probe_bar1_rw(uint64_t bar1_phys, uint64_t bar1_size)
{
    if (!gpu.probe.bar1_accessible)
        return;

    /* Need at least 32MB + 16 bytes of aperture */
    if (bar1_size < BAR1_RW_TEST_OFFSET + 16) {
        serial_puts("[GPU] BAR1 R/W test: aperture too small, skipped\n");
        return;
    }

    volatile uint32_t *test = (volatile uint32_t *)(bar1_phys + BAR1_RW_TEST_OFFSET);

    /* Save originals */
    rmb();
    uint32_t orig0 = test[0];
    uint32_t orig1 = test[1];

    /* Write test patterns */
    test[0] = 0xDEADBEEF;
    test[1] = 0x0517014B;  /* "OSITOK" */
    wmb();

    /* Read back */
    rmb();
    uint32_t rb0 = test[0];
    uint32_t rb1 = test[1];

    serial_puts("[GPU] BAR1 R/W test at +32MB: wrote DEADBEEF, read ");
    serial_puthex(rb0, 8);

    if (rb0 == 0xDEADBEEF && rb1 == 0x0517014B) {
        serial_puts(" OK\n");
        gpu.probe.bar1_rw_ok = true;
    } else {
        serial_puts(" FAIL (expected DEADBEEF/0517014B, got ");
        serial_puthex(rb0, 8);
        serial_puts("/");
        serial_puthex(rb1, 8);
        serial_puts(")\n");
    }

    /* Restore originals */
    test[0] = orig0;
    test[1] = orig1;
    wmb();
}

static void gpu_probe_pramin(void)
{
    uint32_t d0, d1, d2, d3;

    d0 = gpu_read(NV_PRAMIN_BASE + 0x00);
    d1 = gpu_read(NV_PRAMIN_BASE + 0x04);
    d2 = gpu_read(NV_PRAMIN_BASE + 0x08);
    d3 = gpu_read(NV_PRAMIN_BASE + 0x0C);

    serial_puts("[GPU] PRAMIN[0..3]: ");
    serial_puthex(d0, 8); serial_puts(" ");
    serial_puthex(d1, 8); serial_puts(" ");
    serial_puthex(d2, 8); serial_puts(" ");
    serial_puthex(d3, 8); serial_puts("\n");

    if (d0 != NV_DEAD_REG || d1 != NV_DEAD_REG ||
        d2 != NV_DEAD_REG || d3 != NV_DEAD_REG)
        gpu.probe.pramin_accessible = true;
}

/* ── Report: Print Results ───────────────────────────────────── */

static void gpu_report(uint64_t bar1_base)
{
    gpu_probe_t *p = &gpu.probe;

    /* Chip info */
    serial_puts("[GPU] Chip: ");
    serial_puts(gpu_chip_name(p->chip_id));
    serial_puts(", revision ");
    serial_puthex(p->chip_rev, 1);
    serial_puts(" (boot0=0x");
    serial_puthex(p->boot0, 8);
    serial_puts(")\n");

    fb_puts("\n GPU: ");
    fb_puts_color(gpu_chip_name(p->chip_id), 0x0000FF00);
    fb_puts(" rev ");
    fb_puthex(p->chip_rev, 1);
    fb_puts("\n");

    /* Engines */
    serial_puts("[GPU] Engines:");
    if (p->engines & NV_PMC_ENABLE_PGRAPH) serial_puts(" PGRAPH");
    if (p->engines & NV_PMC_ENABLE_PFIFO)  serial_puts(" PFIFO");
    if (p->engines & NV_PMC_ENABLE_PFB)    serial_puts(" PFB");
    if (p->engines & NV_PMC_ENABLE_PTIMER) serial_puts(" PTIMER");
    if (p->engines & NV_PMC_ENABLE_CE0)    serial_puts(" CE0");
    if (p->engines & NV_PMC_ENABLE_CE1)    serial_puts(" CE1");
    serial_puts("\n");

    fb_puts("  Engines:");
    if (p->engines & NV_PMC_ENABLE_PGRAPH) fb_puts(" PGRAPH");
    if (p->engines & NV_PMC_ENABLE_PFIFO)  fb_puts(" PFIFO");
    if (p->engines & NV_PMC_ENABLE_PFB)    fb_puts(" PFB");
    if (p->engines & NV_PMC_ENABLE_PTIMER) fb_puts(" PTIMER");
    if (p->engines & NV_PMC_ENABLE_CE0)    fb_puts(" CE0");
    if (p->engines & NV_PMC_ENABLE_CE1)    fb_puts(" CE1");
    fb_puts("\n");

    /* PTIMER */
    serial_puts("[GPU] PTIMER: ");
    serial_putdec(p->gpu_timer_ns);
    serial_puts(" ns\n");

    fb_puts("  PTIMER: ");
    fb_putdec(p->gpu_timer_ns);
    fb_puts(" ns\n");

    /* VRAM BAR1 */
    serial_puts("[GPU] VRAM: BAR1=0x");
    serial_puthex(bar1_base, 16);
    serial_puts("\n");

    fb_puts("  BAR1: ");
    fb_puthex(bar1_base, 16);
    fb_puts("\n");

    /* Falcons */
    serial_puts("[GPU] Falcon: GSP=");
    serial_puts(p->gsp_present ? "yes" : "no");
    serial_puts(" SEC2=");
    serial_puts(p->sec2_present ? "yes" : "no");
    serial_puts(" PMU=");
    serial_puts(p->pmu_present ? "yes" : "no");
    serial_puts("\n");

    fb_puts("  Falcon: GSP=");
    fb_puts(p->gsp_present ? "yes" : "no");
    fb_puts(" SEC2=");
    fb_puts(p->sec2_present ? "yes" : "no");
    fb_puts(" PMU=");
    fb_puts(p->pmu_present ? "yes" : "no");
    fb_puts("\n");

    /* ── Phase 2 report ── */

    /* VRAM size */
    serial_puts("[GPU] VRAM: ");
    if (p->vram_size_mb > 0) {
        serial_putdec(p->vram_size_mb);
        serial_puts(" MB\n");
    } else {
        serial_puts("unknown\n");
    }

    fb_puts("  VRAM: ");
    if (p->vram_size_mb > 0) {
        fb_putdec(p->vram_size_mb);
        fb_puts(" MB\n");
    } else {
        fb_puts("unknown\n");
    }

    /* BAR1 status */
    serial_puts("[GPU] BAR1: ");
    serial_puts(p->bar1_accessible ? "accessible" : "NOT accessible");
    if (p->bar1_accessible) {
        serial_puts(", R/W ");
        serial_puts(p->bar1_rw_ok ? "OK" : "FAIL");
    }
    serial_puts("\n");

    fb_puts("  BAR1: ");
    fb_puts(p->bar1_accessible ? "accessible" : "NOT accessible");
    if (p->bar1_accessible) {
        fb_puts(p->bar1_rw_ok ? " R/W OK" : " R/W FAIL");
    }
    fb_puts("\n");

    /* PRAMIN status */
    serial_puts("[GPU] PRAMIN: ");
    serial_puts(p->pramin_accessible ? "accessible" : "NOT accessible");
    serial_puts("\n");

    fb_puts("  PRAMIN: ");
    fb_puts(p->pramin_accessible ? "accessible" : "NOT accessible");
    fb_puts("\n");
}

/* ── Public API ──────────────────────────────────────────────── */

int gpu_init(uint64_t bar0_phys)
{
    /* Get BAR1 from the GPU device struct for reporting */
    extern gpu_device_t *pci_get_gpu(void);
    gpu_device_t *dev = pci_get_gpu();
    uint64_t bar1_base = dev ? dev->bar1_base : 0;

    serial_puts("[GPU] Probing NVIDIA GPU, BAR0=0x");
    serial_puthex(bar0_phys, 16);
    serial_puts("\n");

    memset(&gpu, 0, sizeof(gpu));

    /* BAR0 is identity-mapped (physical = virtual in our setup) */
    gpu.bar0 = (volatile void *)bar0_phys;

    /* Probe boot registers (chip ID, revision) */
    if (gpu_probe_boot() != 0) {
        serial_puts("[GPU] Probe failed — BAR0 not accessible\n");
        fb_puts("\n GPU: BAR0 not accessible\n");
        return -1;
    }

    gpu.probe.present = true;

    /* Phase 1: probe subsystems */
    gpu_probe_engines();
    gpu_probe_timer();
    gpu_probe_falcons();

    /* Phase 2: VRAM / BAR1 / PRAMIN */
    gpu_probe_vram();
    gpu_probe_bar1(bar1_base, dev ? dev->bar1_size : 0);
    gpu_probe_bar1_rw(bar1_base, dev ? dev->bar1_size : 0);
    gpu_probe_pramin();

    /* Report results */
    gpu_report(bar1_base);

    serial_puts("[GPU] Phase 2 probe complete\n");
    fb_puts("  Phase 2 probe complete\n");

    return 0;
}

gpu_probe_t *gpu_get_probe(void)
{
    return &gpu.probe;
}
