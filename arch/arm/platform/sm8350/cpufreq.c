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

/* The L3 cache's own EPSS block ("qcom,lahaina-epss-l3-cpu", 0x4000 long;
 * its level table is the devfreq "ftbl-base" at 0x100). Android votes on
 * it per CPU; Osito-K only reads it (`cpufreq l3`). */
#define L3_BASE             0x18590000UL

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
    int         rail;               /* domains on one rail pull each other up */
    uint32_t    khz[LUT_MAX];
    uint16_t    mv[LUT_MAX];
    uint8_t     boost[LUT_MAX];
    uint8_t     alt[LUT_MAX];       /* source 0: the alternate clock */
};

static struct domain domains[] = {
    { .base = 0x18591000UL, .first_cpu = 0, .ncpus = 4, .core = "Cortex-A55",
      .dmips_mhz = 1024, .rail = 0 },
    { .base = 0x18592000UL, .first_cpu = 4, .ncpus = 3, .core = "Cortex-A78",
      .dmips_mhz = 1946, .rail = 1 },
    { .base = 0x18593000UL, .first_cpu = 7, .ncpus = 1, .core = "Cortex-X1",
      .dmips_mhz = 2048, .rail = 1 },
};
#define NDOMAINS (int)(sizeof(domains) / sizeof(domains[0]))

/* The table ends where a row repeats the previous frequency (as in Linux).
 * Rows with source 0 run off the alternate clock, GPLL0 / 2, which Linux
 * votes on (the cpufreq node's "alternate" clock) and Osito-K doesn't.
 * The one time the A55s were asked for level 0 (2026-10-29), the USB
 * console died, most likely from the gadget's missing barriers (dwc3_acm.c)
 * rather than the clock; untested since, so nothing here asks for those
 * rows. */
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
    if (d->top < 0)
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

/* `cpufreq l3`: the L3 block, read-only, to work out its registers: the
 * level table, then the words where the CPU domains keep their state
 * (0x020) and votes (0x090-0x0ac on newer EPSS, 0x320) */
static void l3_show(void)
{
    uint32_t prev = 0;
    serial_puts("L3 levels, MHz:");
    for (int i = 0; i < LUT_MAX; i++) {
        uint32_t f = mmio_rd32(L3_BASE + REG_FREQ_LUT + i * 4);
        uint32_t khz = LUT_SRC(f) ? XO_KHZ * LUT_L_VAL(f) : ALT_KHZ;
        if (f == 0 || (i > 0 && khz == prev))
            break;
        serial_puts(i % 8 ? " " : "\n   ");
        serial_putdec(i);
        serial_puts("=");
        serial_putdec(khz / 1000);
        prev = khz;
    }
    static const uint16_t from[] = { 0x000, 0x300 }, to[] = { 0x100, 0x340 };
    for (int r = 0; r < 2; r++)
        for (uint32_t off = from[r]; off < to[r]; off += 4) {
            if (off % 16 == 0) {
                serial_puts("\n  ");
                serial_puthex(off, 3);
                serial_puts(":");
            }
            serial_puts(" ");
            serial_puthex(mmio_rd32(L3_BASE + off), 8);
        }
    serial_puts("\n");
}

/* ── `cpufreq try`: run a domain at a level for a few seconds, watched ──
 *
 * For a level that might take its CPUs down with it. A task pinned outside
 * the domain samples every 50 ms, with its IRQs off: the domain's state,
 * the L3 block's 0x020, and the timer ticks each of the domain's CPUs took.
 * The first CPU that goes TRY_QUIET_MS without a tick puts the domain back
 * at once; it then watches one more second, to see whether the CPUs come
 * back. Unless told to stay, the shell (and with it the USB console) moves
 * out of the domain too. This is how the A55 "hang" turned out to be the
 * USB gadget alone (dwc3_acm.c, rd/wr). */
#define TRY_STEP_MS     50
#define TRY_QUIET_MS    1000    /* `dmesg` alone keeps IRQs off ~150 ms at 403 MHz */
#define TRY_MAX_S       20
#define TRY_SAMPLES     ((TRY_MAX_S + 1) * 1000 / TRY_STEP_MS + 2)
#define TRY_ROW         10              /* samples per report row: 0.5 s */
/* A bar under the heartbeat squares, drawn without the console lock (a
 * stopped CPU may hold it): yellow while it runs, then red if a CPU missed
 * its ticks, green if none did */
#define TRY_BAR(color)  fb_fill(0, 460, 560, 20, (color))

static struct try_sample {
    uint32_t ms;
    uint32_t state;                     /* the domain's REG_DOMAIN_STATE */
    uint32_t l3;                        /* L3_BASE + REG_DOMAIN_STATE */
    uint8_t  ticks[4];                  /* per CPU, since the sample before */
} samples[TRY_SAMPLES];

static struct {
    volatile int running;
    int dom, level, secs;
    int shell, shell_cpu;               /* the shell's task and home, if moved */
} trial = { .shell = -1 };

static void put_pad(uint64_t v, int width)
{
    uint64_t p = 10;
    for (int digits = 1; digits < width; digits++, p *= 10)
        if (v < p)
            serial_puts(" ");
    serial_putdec(v);
}

static void try_row(const struct domain *d, int from, int to)
{
    const struct try_sample *s = &samples[to - 1];
    put_pad(s->ms, 7);
    put_pad(LUT_L_VAL(s->state) * XO_KHZ / 1000, 6);
    put_pad(STATE_MV(s->state), 6);
    put_pad((s->state >> 8) & 0x1F, 4);
    serial_puts("  ");
    serial_puthex(s->l3, 8);
    serial_puts(" ");
    for (int c = 0; c < d->ncpus; c++) {
        uint32_t sum = 0;
        for (int i = from; i < to; i++)
            sum += samples[i].ticks[c];
        put_pad(sum, 5);
    }
    serial_puts("\n");
}

static void try_task(void *arg)
{
    (void)arg;
    struct domain *d = &domains[trial.dom];
    uint64_t last[4];
    int quiet[4] = { 0 };
    int n = 0, back_at = 0, stalled = -1, stalled_cpu = -1;
    int back = d->want;

    TRY_BAR(0x00FFFF00);
    uint64_t flags = irq_save();
    for (int c = 0; c < d->ncpus; c++)
        last[c] = smp_cpu_ticks(d->first_cpu + c);
    uint64_t t0 = timer_ms(), end = t0 + (uint64_t)trial.secs * 1000;
    set_level(d, trial.level);
    for (uint64_t next = t0 + TRY_STEP_MS; n < TRY_SAMPLES; next += TRY_STEP_MS) {
        while (timer_ms() < next)
            ;
        struct try_sample *s = &samples[n++];
        s->ms = (uint32_t)(next - t0);
        s->state = mmio_rd32(d->base + REG_DOMAIN_STATE);
        s->l3 = mmio_rd32(L3_BASE + REG_DOMAIN_STATE);
        for (int c = 0; c < d->ncpus; c++) {
            uint64_t t = smp_cpu_ticks(d->first_cpu + c);
            s->ticks[c] = (uint8_t)(t - last[c] > 255 ? 255 : t - last[c]);
            last[c] = t;
            quiet[c] = s->ticks[c] ? 0 : quiet[c] + 1;
            if (quiet[c] >= TRY_QUIET_MS / TRY_STEP_MS && stalled < 0) {
                stalled = (int)s->ms;
                stalled_cpu = d->first_cpu + c;
            }
        }
        if (!back_at && (stalled >= 0 || next >= end)) {
            set_level(d, back);
            back_at = n;                /* samples before it ran at the level tried */
            end = next + 1000;
        } else if (back_at && next >= end) {
            break;
        }
    }
    irq_restore(flags);
    TRY_BAR(stalled >= 0 ? 0x00FF0000 : 0x0000FF00);

    serial_puts("[TRY ] ticks per 0.5 s, 50 when none is missed\n");
    serial_puts("     ms   MHz    mV lvl  L3 0x020   ");
    for (int c = 0; c < d->ncpus; c++) {
        serial_puts(" cpu");
        serial_putdec(d->first_cpu + c);
    }
    serial_puts("\n");
    for (int i = 0; i < n; ) {
        int j = i + TRY_ROW < n ? i + TRY_ROW : n;
        if (i < back_at && j > back_at)
            j = back_at;
        try_row(d, i, j);
        if (j == back_at)
            serial_puts("        -- back to the level before --\n");
        i = j;
    }
    if (stalled >= 0) {
        serial_puts("[TRY ] cpu");
        serial_putdec(stalled_cpu);
        serial_puts(" missed its ticks by ");
        serial_putdec(stalled);
        serial_puts(" ms\n");
    } else {
        serial_puts("[TRY ] every CPU kept ticking\n");
    }

    /* The shell goes home only to a CPU that ticks */
    if (trial.shell >= 0) {
        int home = trial.shell_cpu < 0 ? smp_cpu_id() : trial.shell_cpu;
        uint64_t t = smp_cpu_ticks(home);
        mdelay(100);
        if (smp_cpu_ticks(home) != t) {
            task_pin(trial.shell, trial.shell_cpu);
        } else {
            serial_puts("[TRY ] cpu");
            serial_putdec(trial.shell_cpu);
            serial_puts(" doesn't tick: the shell stays where it is\n");
        }
        trial.shell = -1;
    }
    serial_puts("[TRY ] done\n");
    trial.running = 0;
}

static int in_domain(const struct domain *d, int cpu)
{
    return cpu >= d->first_cpu && cpu < d->first_cpu + d->ncpus;
}

/* `cpufreq try <domain> <level> <seconds> [stay | busy]`: stay leaves the
 * shell where it is, busy keeps the domain's CPUs busy meanwhile */
static void try_cmd(int argc, char **argv)
{
    int dom, level, secs;
    const char *opt = argc == 6 ? argv[5] : "";
    if (argc < 5 || argc > 6 || parse_uint(argv[2], &dom) ||
        parse_uint(argv[3], &level) || parse_uint(argv[4], &secs) ||
        dom >= NDOMAINS || domains[dom].top < 0 || level < domains[dom].floor ||
        level >= domains[dom].nlevels || secs < 1 || secs > TRY_MAX_S ||
        (*opt && strcmp(opt, "stay") != 0 && strcmp(opt, "busy") != 0)) {
        serial_puts("usage: cpufreq try <domain> <level> <seconds, up to 20> [stay | busy]\n");
        return;
    }
    if (trial.running) {
        serial_puts("cpufreq try: one is running\n");
        return;
    }
    struct domain *d = &domains[dom];

    /* The watcher and the shell take the highest CPUs outside the domain */
    int watch = -1, away = -1;
    for (int c = MAX_CPUS - 1; c >= 0; c--) {
        if (in_domain(d, c) || smp_cpu_ticks(c) == 0)
            continue;
        if (watch < 0)
            watch = c;
        else if (away < 0)
            away = c;
    }
    int here = smp_cpu_id();
    int move = in_domain(d, here) && strcmp(opt, "stay") != 0;
    if (watch < 0 || (move && away < 0)) {
        serial_puts("cpufreq try: no CPU outside the domain to watch from\n");
        return;
    }

    trial.running = 1;
    trial.dom = dom;
    trial.level = level;
    trial.secs = secs;
    if (strcmp(opt, "busy") == 0)
        for (int c = 0; c < d->ncpus; c++)
            smp_burn(d->first_cpu + c, secs + 2);
    serial_puts("[TRY ] ");
    print_cpus(d);
    serial_puts(" at ");
    print_mhz(d->khz[level]);
    serial_puts(" for ");
    serial_putdec(secs);
    serial_puts(" s, watched from cpu");
    serial_putdec(watch);
    if (move) {
        int id = task_self(), was = task_pin(id, away);
        if (was >= TASK_ANY_CPU) {
            trial.shell = id;
            trial.shell_cpu = was;
        }
        serial_puts(", shell on cpu");
        serial_putdec(smp_cpu_id());
    }
    serial_puts("\n");
    if (task_create_on("cpufreq-try", try_task, (void *)0, 2, watch) < 0) {
        if (trial.shell >= 0)
            task_pin(trial.shell, trial.shell_cpu);
        trial.shell = -1;
        trial.running = 0;
    }
}

/* Shell `cpufreq`: show the domains; `cpufreq max|min`: every domain to its
 * top regular / lowest PLL level; `cpufreq <domain> <level>`. Reports every
 * domain on the rail it changed. `cpufreq l3`, `cpufreq try`: see above. */
void platform_cpufreq_cmd(int argc, char **argv)
{
    int dom, level;
    if (argc < 2) {
        show();
        return;
    }
    if (argc == 2 && strcmp(argv[1], "l3") == 0) {
        l3_show();
        return;
    }
    if (strcmp(argv[1], "try") == 0) {
        try_cmd(argc, argv);
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "max") == 0 || strcmp(argv[1], "min") == 0)) {
        for (int i = 0; i < NDOMAINS; i++) {
            struct domain *d = &domains[i];
            if (d->top >= 0)
                set_level(d, argv[1][1] == 'a' ? d->top : d->floor);
        }
        for (int i = 0; i < NDOMAINS; i++)
            if (domains[i].top >= 0)
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
    set_level(&domains[dom], level);
    for (int i = 0; i < NDOMAINS; i++)
        if (domains[i].rail == domains[dom].rail && domains[i].top >= 0)
            report(&domains[i]);
}
