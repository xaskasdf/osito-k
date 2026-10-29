/*
 * cpufreq.c -- CPU frequency on SM8350 through the EPSS hardware
 *
 * Each cluster is an EPSS frequency domain (the phone's device tree:
 * qcom,cpufreq-hw-epss, LUT rows of 4 bytes): 0 = cpu0-3 (A55), 1 = cpu4-6
 * (A78), 2 = cpu7 (X1). The hardware keeps a table of performance levels
 * and sequences the voltage itself; software only writes the index of the
 * level it wants, as Linux's qcom-cpufreq-hw does. The bootloader leaves
 * the big clusters low (cpubench: A78 ~710 MHz, X1 ~845 MHz against A55
 * ~1.5 GHz), so boot moves every domain to its top regular level.
 *
 * Nothing here manages temperature: the limits hardware (LMh) still caps
 * the clock when the silicon gets hot, which REG_DOMAIN_STATE reflects.
 */

#include "../../include/hal.h"
#include "../../include/aarch64.h"
#include "../../include/types.h"

#define XO_KHZ          19200       /* L values count XO cycles */
#define ALT_KHZ         300000      /* LUT rows with source 0: GPLL0 / 2 */
#define LUT_MAX         40

#define REG_ENABLE          0x000
#define REG_DOMAIN_STATE    0x020   /* L value it runs at, after limits */
#define REG_DCVS_CTRL       0x0b0   /* bit 0: a perf-state register per core */
#define REG_FREQ_LUT        0x100
#define REG_VOLT_LUT        0x200
#define REG_PERF_STATE      0x320

#define LUT_SRC(d)          ((d) >> 30)
#define LUT_L_VAL(d)        ((d) & 0xFF)
#define LUT_CORE_COUNT(d)   (((d) >> 16) & 0x7)
#define LUT_TURBO           1       /* core count 1: a boost level */
#define LUT_VOLT(d)         ((d) & 0xFFF)

struct domain {
    uint64_t    base;
    int         first_cpu, ncpus;
    const char *core;
    uint32_t    dmips_mhz;          /* capacity-dmips-mhz in the device tree */
    int         nlevels;
    int         top;                /* highest level that isn't boost */
    uint32_t    khz[LUT_MAX];
    uint16_t    mv[LUT_MAX];
    uint8_t     boost[LUT_MAX];
};

static struct domain domains[] = {
    { .base = 0x18591000UL, .first_cpu = 0, .ncpus = 4, .core = "Cortex-A55",
      .dmips_mhz = 1024 },
    { .base = 0x18592000UL, .first_cpu = 4, .ncpus = 3, .core = "Cortex-A78",
      .dmips_mhz = 1946 },
    { .base = 0x18593000UL, .first_cpu = 7, .ncpus = 1, .core = "Cortex-X1",
      .dmips_mhz = 2048 },
};
#define NDOMAINS (int)(sizeof(domains) / sizeof(domains[0]))

/* The table ends where a row repeats the previous frequency (as in Linux) */
static void read_lut(struct domain *d)
{
    uint32_t prev = 0;
    d->nlevels = 0;
    d->top = -1;
    for (int i = 0; i < LUT_MAX; i++) {
        uint32_t f = mmio_rd32(d->base + REG_FREQ_LUT + i * 4);
        uint32_t v = mmio_rd32(d->base + REG_VOLT_LUT + i * 4);
        uint32_t khz = LUT_SRC(f) ? XO_KHZ * LUT_L_VAL(f) : ALT_KHZ;
        if (f == 0 || (i > 0 && khz == prev))
            break;
        d->khz[i] = khz;
        d->mv[i] = (uint16_t)LUT_VOLT(v);
        d->boost[i] = LUT_CORE_COUNT(f) == LUT_TURBO;
        if (!d->boost[i])
            d->top = i;
        d->nlevels = i + 1;
        prev = khz;
    }
}

static uint32_t current_khz(const struct domain *d)
{
    return LUT_L_VAL(mmio_rd32(d->base + REG_DOMAIN_STATE)) * XO_KHZ;
}

/* Request a level, then wait up to 10 ms for the domain to report it. The
 * scheduler gets the new speed of each CPU (per-MHz performance x MHz, as
 * Linux computes capacity): A55 1.85M, A78 4.71M, X1 5.82M at the top. */
static uint32_t set_level(struct domain *d, int level)
{
    int per_core = mmio_rd32(d->base + REG_DCVS_CTRL) & 1;
    mmio_wr32(d->base + REG_PERF_STATE, (uint32_t)level);
    for (int c = 1; per_core && c < d->ncpus; c++)
        mmio_wr32(d->base + REG_PERF_STATE + c * 4, (uint32_t)level);
    for (int c = 0; c < d->ncpus; c++)
        sched_set_capacity(d->first_cpu + c, d->dmips_mhz * (d->khz[level] / 1000));
    uint64_t t0 = timer_ms();
    while (current_khz(d) != d->khz[level] && timer_ms() - t0 < 10)
        ;
    return current_khz(d);
}

static void print_mhz(uint32_t khz)
{
    serial_putdec(khz / 1000);
    serial_puts(" MHz");
}

static void print_cpus(const struct domain *d)
{
    serial_puts("cpu");
    serial_putdec(d->first_cpu);
    if (d->ncpus > 1) {
        serial_puts("-");
        serial_putdec(d->first_cpu + d->ncpus - 1);
    }
}

void platform_cpufreq_init(void)
{
    for (int i = 0; i < NDOMAINS; i++) {
        struct domain *d = &domains[i];
        read_lut(d);
        serial_puts("[FREQ] ");
        print_cpus(d);
        serial_puts(" ");
        serial_puts(d->core);
        serial_puts(": ");
        if (d->top < 0) {
            serial_puts("no frequency table, left alone\n");
            continue;
        }
        print_mhz(current_khz(d));
        serial_puts(" -> ");
        print_mhz(set_level(d, d->top));
        serial_puts(" (level ");
        serial_putdec(d->top);
        serial_puts(" of ");
        serial_putdec(d->nlevels);
        serial_puts(")\n");
    }
}

static int parse_uint(const char *s, int *out)
{
    int v = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

static void show(void)
{
    for (int i = 0; i < NDOMAINS; i++) {
        struct domain *d = &domains[i];
        serial_puts("domain ");
        serial_putdec(i);
        serial_puts("  ");
        print_cpus(d);
        serial_puts(" ");
        serial_puts(d->core);
        serial_puts("  now ");
        print_mhz(current_khz(d));
        serial_puts(", level ");
        serial_putdec(mmio_rd32(d->base + REG_PERF_STATE));
        serial_puts("\n  levels:");
        for (int l = 0; l < d->nlevels; l++) {
            serial_puts(l % 8 ? " " : "\n   ");
            serial_putdec(l);
            serial_puts("=");
            serial_putdec(d->khz[l] / 1000);
            if (d->boost[l])
                serial_puts("(boost)");
        }
        serial_puts("\n");
    }
}

/* Shell `cpufreq`: show the domains; `cpufreq max|min`: every domain to its
 * top regular / lowest level; `cpufreq <domain> <level>` */
void platform_cpufreq_cmd(int argc, char **argv)
{
    int dom, level;
    if (argc < 2) {
        show();
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "max") == 0 || strcmp(argv[1], "min") == 0)) {
        for (int i = 0; i < NDOMAINS; i++) {
            struct domain *d = &domains[i];
            if (d->top < 0)
                continue;
            print_cpus(d);
            serial_puts(": ");
            print_mhz(set_level(d, argv[1][1] == 'a' ? d->top : 0));
            serial_puts("\n");
        }
        return;
    }
    if (argc != 3 || parse_uint(argv[1], &dom) || parse_uint(argv[2], &level) ||
        dom >= NDOMAINS || level >= domains[dom].nlevels) {
        serial_puts("usage: cpufreq [max | min | <domain> <level>]\n");
        return;
    }
    print_cpus(&domains[dom]);
    serial_puts(": ");
    print_mhz(set_level(&domains[dom], level));
    serial_puts("\n");
}
