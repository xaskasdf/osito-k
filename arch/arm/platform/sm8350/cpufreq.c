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
 * A domain is set to the lower of the level asked for (boot, shell
 * `cpufreq`) and its thermal cap, which thermal.c moves. What it runs at
 * can differ (REG_DOMAIN_STATE): the limits hardware (LMh) caps the clock
 * when the silicon gets hot, and the two big clusters share a voltage
 * rail. The rail follows the domain that needs the higher voltage, and
 * the EPSS runs the other one at its fastest level that voltage allows,
 * whatever level it was set to. Measured 2026-10-29: with the A78s set to
 * 710 MHz, the X1 at 1900 / 2496 / 2841 MHz kept them at 1440 / 1996 /
 * 2419 MHz; set to 1670 MHz, they kept the X1 at 2035. So only lowering
 * both makes either go down.
 */

#include "../../include/hal.h"
#include "../../include/aarch64.h"
#include "../../include/types.h"
#include "../../include/spinlock.h"
#include "cpufreq.h"

#define XO_KHZ          19200       /* L values count XO cycles */
#define ALT_KHZ         300000      /* LUT rows with source 0: GPLL0 / 2 */
#define LUT_MAX         40

#define REG_ENABLE          0x000
#define REG_DOMAIN_STATE    0x020   /* what it runs at, after limits */
#define STATE_MV(s)         ((s) >> 20)     /* matches the level's VOLT_LUT */
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
    int         floor;              /* lowest level off a PLL (see read_lut) */
    int         want;               /* level asked for */
    int         cap;                /* thermal limit, nlevels - 1 = none */
    int         fixed;              /* stays at its top level (see domains) */
    int         rail;               /* domains on one rail pull each other up */
    uint32_t    khz[LUT_MAX];
    uint16_t    mv[LUT_MAX];
    uint8_t     boost[LUT_MAX];
    uint8_t     alt[LUT_MAX];       /* source 0: the alternate clock */
};

/* The A55s never go below their top level. On 2026-10-29, lowering them
 * (to the alternate clock, then to 403 MHz off a PLL) hung the machine
 * within seconds, twice: the USB console (cpu0) stopped answering, with no
 * panic, and a photo of the hung screen had cpu0-3's heartbeat squares
 * dark and cpu4-7's lit, as if the whole cluster had stopped. Linux lowers
 * this domain all the time, so something it sets up is missing here (one
 * suspect: the L3 cache's frequency, which Linux votes on and Osito-K
 * leaves alone); until that is found, only the big clusters move. */
static struct domain domains[] = {
    { .base = 0x18591000UL, .first_cpu = 0, .ncpus = 4, .core = "Cortex-A55",
      .dmips_mhz = 1024, .fixed = 1, .rail = 0 },
    { .base = 0x18592000UL, .first_cpu = 4, .ncpus = 3, .core = "Cortex-A78",
      .dmips_mhz = 1946, .rail = 1 },
    { .base = 0x18593000UL, .first_cpu = 7, .ncpus = 1, .core = "Cortex-X1",
      .dmips_mhz = 2048, .rail = 1 },
};
#define NDOMAINS (int)(sizeof(domains) / sizeof(domains[0]))

/* The table ends where a row repeats the previous frequency (as in Linux).
 * Rows with source 0 run off the alternate clock, GPLL0 / 2, which Linux
 * votes on (the cpufreq node's "alternate" clock) and Osito-K doesn't: on
 * 2026-10-29 the A55s' level 0 left the shell and USB too slow to answer
 * at all. Nothing here asks for those rows. */
static void read_lut(struct domain *d)
{
    uint32_t prev = 0;
    d->nlevels = 0;
    d->top = -1;
    d->floor = -1;
    for (int i = 0; i < LUT_MAX; i++) {
        uint32_t f = mmio_rd32(d->base + REG_FREQ_LUT + i * 4);
        uint32_t v = mmio_rd32(d->base + REG_VOLT_LUT + i * 4);
        uint32_t khz = LUT_SRC(f) ? XO_KHZ * LUT_L_VAL(f) : ALT_KHZ;
        if (f == 0 || (i > 0 && khz == prev))
            break;
        d->khz[i] = khz;
        d->mv[i] = (uint16_t)LUT_VOLT(v);
        d->boost[i] = LUT_CORE_COUNT(f) == LUT_TURBO;
        d->alt[i] = LUT_SRC(f) == 0;
        if (!d->boost[i])
            d->top = i;
        if (!d->alt[i] && d->floor < 0)
            d->floor = i;
        d->nlevels = i + 1;
        prev = khz;
    }
    if (d->floor < 0 || d->floor > d->top)
        d->top = -1;                /* nothing usable: leave the domain alone */
    d->want = d->top;
    d->cap = d->nlevels - 1;
}

static uint32_t current_khz(const struct domain *d)
{
    return LUT_L_VAL(mmio_rd32(d->base + REG_DOMAIN_STATE)) * XO_KHZ;
}

/* The shell's `cpufreq` and the thermal task both change levels */
static spinlock_t freq_lock;

static int effective(const struct domain *d)
{
    return d->want < d->cap ? d->want : d->cap;
}

/* Program min(want, cap). The scheduler gets the new speed of each CPU
 * (per-MHz performance x MHz, as Linux computes capacity): A55 1.85M, A78
 * 4.71M, X1 5.82M at the top. Caller holds freq_lock. */
static int apply(struct domain *d)
{
    int level = effective(d);
    int per_core = mmio_rd32(d->base + REG_DCVS_CTRL) & 1;
    mmio_wr32(d->base + REG_PERF_STATE, (uint32_t)level);
    for (int c = 1; per_core && c < d->ncpus; c++)
        mmio_wr32(d->base + REG_PERF_STATE + c * 4, (uint32_t)level);
    for (int c = 0; c < d->ncpus; c++)
        sched_set_capacity(d->first_cpu + c, d->dmips_mhz * (d->khz[level] / 1000));
    return level;
}

/* Wait up to 10 ms for the domain to report the level's frequency (it
 * shows within a millisecond, unless the rail or LMh keep it elsewhere) */
static uint32_t settle(const struct domain *d, int level)
{
    uint64_t t0 = timer_ms();
    while (current_khz(d) != d->khz[level] && timer_ms() - t0 < 10)
        ;
    return current_khz(d);
}

/* Ask for a level (it may run lower while thermally capped); returns the
 * level programmed */
static int set_level(struct domain *d, int level)
{
    uint64_t flags = spin_lock_irqsave(&freq_lock);
    d->want = level;
    int eff = apply(d);
    spin_unlock_irqrestore(&freq_lock, flags);
    return eff;
}

int cpufreq_cap_step(int dom, int dir)
{
    struct domain *d = &domains[dom];
    if (d->top < 0 || d->fixed)
        return -1;
    uint64_t flags = spin_lock_irqsave(&freq_lock);
    int old = d->cap;
    if (dir < 0) {
        /* Below what it's set to now, or lowering the cap does nothing */
        int eff = effective(d);
        d->cap = eff > d->floor ? eff - 1 : d->floor;
    } else if (d->cap < d->nlevels - 1) {
        d->cap++;
    }
    if (d->cap != old)
        apply(d);
    int cap = d->cap;
    spin_unlock_irqrestore(&freq_lock, flags);
    return cap;
}

int cpufreq_capped(int dom)
{
    const struct domain *d = &domains[dom];
    return d->top >= 0 && d->cap < d->want;
}

uint32_t cpufreq_cap_khz(int dom)
{
    const struct domain *d = &domains[dom];
    return d->top >= 0 ? d->khz[effective(d)] : 0;
}

uint32_t cpufreq_now_khz(int dom)
{
    return current_khz(&domains[dom]);
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
        print_mhz(settle(d, set_level(d, d->top)));
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
        serial_puts(" at ");
        serial_putdec(STATE_MV(mmio_rd32(d->base + REG_DOMAIN_STATE)));
        serial_puts(" mV, set to level ");
        serial_putdec(mmio_rd32(d->base + REG_PERF_STATE));
        if (d->fixed)
            serial_puts(" (fixed at its top)");
        if (d->top >= 0 && d->cap < d->want) {
            serial_puts(" (asked ");
            serial_putdec(d->want);
            serial_puts(", thermal cap ");
            serial_putdec(d->cap);
            serial_puts(")");
        }
        serial_puts("\n  levels, MHz@mV:");
        for (int l = 0; l < d->nlevels; l++) {
            serial_puts(l % 5 ? " " : "\n   ");
            serial_putdec(l);
            serial_puts("=");
            serial_putdec(d->khz[l] / 1000);
            serial_puts("@");
            serial_putdec(d->mv[l]);
            if (d->boost[l])
                serial_puts("(boost)");
            if (d->alt[l])
                serial_puts("(alt, unused)");
        }
        serial_puts("\n");
    }
}

/* What a domain runs at, and what it's set to when that differs */
static void report(const struct domain *d)
{
    int eff = effective(d);
    uint32_t khz = settle(d, eff);
    print_cpus(d);
    serial_puts(": ");
    print_mhz(khz);
    if (khz != d->khz[eff]) {
        serial_puts(", set to ");
        print_mhz(d->khz[eff]);
        if (khz > d->khz[eff])
            serial_puts(" (the other cluster holds their shared rail up)");
    }
    if (eff < d->want)
        serial_puts(" (thermal cap)");
    serial_puts("\n");
}

/* Shell `cpufreq`: show the domains; `cpufreq max|min`: every domain to its
 * top regular / lowest PLL level; `cpufreq <domain> <level>`. Reports every
 * domain on the rail it changed. */
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
            if (d->top >= 0 && !d->fixed)
                set_level(d, argv[1][1] == 'a' ? d->top : d->floor);
        }
        for (int i = 0; i < NDOMAINS; i++)
            if (domains[i].top >= 0 && !domains[i].fixed)
                report(&domains[i]);
        return;
    }
    if (argc != 3 || parse_uint(argv[1], &dom) || parse_uint(argv[2], &level) ||
        dom >= NDOMAINS || domains[dom].top < 0 || level >= domains[dom].nlevels ||
        level < domains[dom].floor) {
        serial_puts("usage: cpufreq [max | min | <domain> <level>], levels from "
                    "the domain's first PLL one\n");
        return;
    }
    if (domains[dom].fixed) {
        print_cpus(&domains[dom]);
        serial_puts(" stay at their top level: lowering them hung the cluster\n");
        return;
    }
    set_level(&domains[dom], level);
    for (int i = 0; i < NDOMAINS; i++)
        if (domains[i].rail == domains[dom].rail && domains[i].top >= 0)
            report(&domains[i]);
}
