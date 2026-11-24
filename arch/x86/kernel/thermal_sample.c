#include "thermal.h"
#include "../include/msr_safe.h"

#define IA32_MPERF               0xe7
#define IA32_APERF               0xe8
#define IA32_PERF_CTL            0x199
#define MSR_PKG_CST_CONFIG       0xe2
#define MSR_POWER_CTL            0x1fc
#define MSR_RAPL_POWER_UNIT      0x606
#define MSR_PKG_ENERGY_STATUS    0x611

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t n);
extern void serial_puthex(uint64_t n, int digits);
extern uint64_t idt_get_tsc_freq(void);
extern int sched_sleep_ticks(uint64_t ticks);

#ifdef THERMAL_SAMPLE_HOST_TEST
extern void sample_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d);
extern uint64_t sample_tsc(void);
#else
static void sample_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static uint64_t sample_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
#endif

/* Intel SDM vol. 4 tables 2-20/21/29/35/39, client Skylake/Kaby/Coffee Lake.
 * Do not probe model-specific addresses on unrelated or hybrid processors. */
static bool sample_model_supported(uint32_t signature)
{
    uint32_t family = (signature >> 8) & 15;
    uint32_t model = ((signature >> 4) & 15) | ((signature >> 12) & 0xf0);
    return family == 6 && (model == 0x4e || model == 0x5e ||
                           model == 0x8e || model == 0x9e);
}

static const struct { uint32_t msr; const char *name; } residency[] = {
    {0x3fc, "Core C3"}, {0x3fd, "Core C6"}, {0x3fe, "Core C7"},
    {0x60d, "Package C2"}, {0x3f8, "Package C3"},
    {0x3f9, "Package C6"}, {0x3fa, "Package C7"}
};
#define RESIDENCY_COUNT (sizeof(residency) / sizeof(residency[0]))

typedef struct {
    uint32_t signature;
    uint64_t tsc_hz;
    bool supported, aperf;
} sample_caps_t;

typedef struct {
    uint64_t tsc, span, aperf, mperf, units, energy;
    uint64_t state[RESIDENCY_COUNT];
    uint64_t perf_ctl, cstate_config, power_ctl;
    uint32_t apic_id;
    bool cpu_valid, perf_valid, energy_valid, state_valid[RESIDENCY_COUNT];
    bool perf_ctl_valid, cstate_config_valid, power_ctl_valid;
} sample_counters_t;

typedef struct {
    bool valid, perf_valid, active_valid, energy_valid, state_valid[RESIDENCY_COUNT];
    uint64_t elapsed_us, c0_bp, active_mhz, mean_mhz, package_mw;
    uint64_t state_bp[RESIDENCY_COUNT];
} sample_result_t;

static void sample_caps(sample_caps_t *s)
{
    memset(s, 0, sizeof(*s));
    uint32_t a, b, c, d;
    sample_cpuid(0, &a, &b, &c, &d);
    if (a < 6 || b != 0x756e6547 || d != 0x49656e69 || c != 0x6c65746e) return;
    sample_cpuid(1, &a, &b, &c, &d);
    s->signature = a;
    if ((d & 0x30) != 0x30 || !sample_model_supported(a)) return;
    sample_cpuid(6, &a, &b, &c, &d);
    s->aperf = (c & 1) != 0;
    sample_cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000007) return;
    sample_cpuid(0x80000007, &a, &b, &c, &d);
    if (!(d & (1U << 8))) return;
    s->tsc_hz = idt_get_tsc_freq();
    s->supported = s->tsc_hz >= 1000000 && s->tsc_hz <= 10000000000ULL;
}

static void sample_read(const sample_caps_t *caps, sample_counters_t *s)
{
    memset(s, 0, sizeof(*s));
    if (!caps->supported) return;
    uint32_t a, b, c, d;
    sample_cpuid(1, &a, &b, &c, &d);
    s->apic_id = b >> 24;
    uint64_t start = sample_tsc();
    if (caps->aperf) {
        bool m = x86_read_msr_safe(IA32_MPERF, &s->mperf);
        bool p = x86_read_msr_safe(IA32_APERF, &s->aperf);
        s->perf_valid = m && p;
    }
    if (x86_read_msr_safe(MSR_RAPL_POWER_UNIT, &s->units))
        s->energy_valid = x86_read_msr_safe(MSR_PKG_ENERGY_STATUS, &s->energy);
    for (unsigned i = 0; i < RESIDENCY_COUNT; i++)
        s->state_valid[i] = x86_read_msr_safe(residency[i].msr, &s->state[i]);
    s->perf_ctl_valid = x86_read_msr_safe(IA32_PERF_CTL, &s->perf_ctl);
    s->cstate_config_valid = x86_read_msr_safe(MSR_PKG_CST_CONFIG, &s->cstate_config);
    s->power_ctl_valid = x86_read_msr_safe(MSR_POWER_CTL, &s->power_ctl);
    uint64_t end = sample_tsc();
    s->span = end - start;
    s->tsc = start + s->span / 2;
    sample_cpuid(1, &a, &b, &c, &d);
    s->cpu_valid = (b >> 24) == s->apic_id && end >= start;
}

static bool sample_scale(uint64_t n, uint64_t scale, uint64_t divisor, uint64_t *out)
{
    if (!divisor || !scale || n > ~0ULL / scale) return false;
    *out = n * scale / divisor;
    return true;
}

static void sample_calculate(const sample_caps_t *caps, const sample_counters_t *a,
                             const sample_counters_t *b, sample_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!caps->supported || !caps->tsc_hz || !a->cpu_valid || !b->cpu_valid ||
        a->apic_id != b->apic_id || b->tsc <= a->tsc ||
        a->span > caps->tsc_hz / 1000 || b->span > caps->tsc_hz / 1000) return;
    uint64_t dt = b->tsc - a->tsc;
    if (!sample_scale(dt, 1000000, caps->tsc_hz, &r->elapsed_us) ||
        r->elapsed_us < 500000 || r->elapsed_us > 5000000) return;
    r->valid = true;
    /* APERF/MPERF overflow clears both counters. Reject reset/backwards pairs. */
    if (a->perf_valid && b->perf_valid && b->mperf >= a->mperf && b->aperf >= a->aperf) {
        uint64_t m = b->mperf - a->mperf, p = b->aperf - a->aperf;
        if (m <= dt && (m || !p)) {
            r->perf_valid = sample_scale(m, 10000, dt, &r->c0_bp) &&
                sample_scale(p, caps->tsc_hz / 1000000, dt, &r->mean_mhz);
            r->active_valid = r->perf_valid && m &&
                sample_scale(p, caps->tsc_hz / 1000000, m, &r->active_mhz);
        }
    }
    for (unsigned i = 0; i < RESIDENCY_COUNT; i++) {
        if (!a->state_valid[i] || !b->state_valid[i] || b->state[i] < a->state[i]) continue;
        uint64_t delta = b->state[i] - a->state[i];
        r->state_valid[i] = delta <= dt && sample_scale(delta, 10000, dt, &r->state_bp[i]);
    }
    if (a->energy_valid && b->energy_valid && a->units == b->units) {
        /* Package energy is a 32-bit modulo counter, in 2^-ESU joules. */
        uint32_t delta = (uint32_t)b->energy - (uint32_t)a->energy;
        unsigned shift = (b->units >> 8) & 31;
        uint64_t uj = ((uint64_t)delta * 1000000) >> shift;
        r->energy_valid = sample_scale(uj, 1000, r->elapsed_us, &r->package_mw);
    }
}

static void sample_percent(uint64_t bp)
{
    serial_putdec(bp / 100); serial_puts(".");
    if (bp % 100 < 10) serial_puts("0");
    serial_putdec(bp % 100); serial_puts("%");
}

static void sample_control(const char *name, bool valid, uint64_t raw)
{
    serial_puts(name); serial_puts(": ");
    if (valid) serial_puthex(raw, 16);
    else serial_puts("unavailable");
    serial_puts("\n");
}

void thermal_sample_dump(void)
{
    sample_caps_t caps;
    sample_counters_t a, b;
    sample_result_t r;
    sample_caps(&caps);
    serial_puts("Power sample (read-only; current CPU/core/package)\n");
    serial_puts("CPU signature: "); serial_puthex(caps.signature, 8);
    serial_puts("; TSC Hz: "); serial_putdec(caps.tsc_hz); serial_puts("\n");
    if (!caps.supported) {
        serial_puts("Sampling unavailable: unsupported model, MSRs or invariant TSC\n");
        return;
    }
    sample_read(&caps, &a);
    /* No busy wait: without a schedulable peer this diagnostic must decline. */
    if (sched_sleep_ticks(100) < 0) {
        serial_puts("Sampling unavailable: scheduler could not sleep\n");
        return;
    }
    sample_read(&caps, &b);
    sample_calculate(&caps, &a, &b, &r);
    if (!r.valid) {
        serial_puts("Sampling invalid: CPU changed, clock interval or read latency\n");
        return;
    }
    serial_puts("CPU APIC "); serial_putdec(b.apic_id);
    serial_puts("; elapsed us: "); serial_putdec(r.elapsed_us); serial_puts("\n");
    serial_puts("CPU C0 (MPERF/TSC): ");
    if (r.perf_valid) sample_percent(r.c0_bp);
    else serial_puts("unavailable");
    serial_puts("; active MHz: ");
    if (r.active_valid) serial_putdec(r.active_mhz);
    else serial_puts("unavailable");
    serial_puts("; wall-average MHz: ");
    if (r.perf_valid) serial_putdec(r.mean_mhz);
    else serial_puts("unavailable");
    serial_puts("\nPackage RAPL average: ");
    if (r.energy_valid) { serial_putdec(r.package_mw); serial_puts(" mW"); }
    else serial_puts("unavailable");
    serial_puts("\n");
    for (unsigned i = 0; i < RESIDENCY_COUNT; i++) {
        serial_puts(residency[i].name); serial_puts(": ");
        if (r.state_valid[i]) sample_percent(r.state_bp[i]);
        else serial_puts("unavailable");
        serial_puts("\n");
    }
    sample_control("Legacy PERF_CTL (unchanged)", b.perf_ctl_valid, b.perf_ctl);
    sample_control("Package C-state config (unchanged)", b.cstate_config_valid, b.cstate_config);
    sample_control("POWER_CTL (unchanged)", b.power_ctl_valid, b.power_ctl);
    serial_puts("Scope: one logical CPU, its core and package; not whole-system power.\n");
}
