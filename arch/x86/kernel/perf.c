/*
 * arch/x86/kernel/perf.c — Hardware PMU integration
 *
 * Programs the architectural PMU (fixed counters + 4 programmable PMCs)
 * with L1d / L2 / TLB / branch miss events and exposes RDPMC-based
 * snapshot + phase accumulator API.
 */

#include "../include/perf.h"
#include "../include/cpu_features.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);
extern void serial_puthex(uint64_t v, int d);

/* ── MSR numbers ─────────────────────────────────────── */
#define MSR_PERF_GLOBAL_CTRL     0x38F
#define MSR_PERF_FIXED_CTR_CTRL  0x38D
#define MSR_PERFEVTSEL(n)        (0x186 + (n))
#define MSR_PMC(n)               (0x0C1 + (n))
#define MSR_FIXED_CTR(n)         (0x309 + (n))
#define MSR_PERF_GLOBAL_STATUS   0x38E
#define MSR_PERF_GLOBAL_OVF_CTRL 0x390

/* PERFEVTSEL bits */
#define PERFEVT_USR      (1ULL << 16)
#define PERFEVT_OS       (1ULL << 17)
#define PERFEVT_ENABLE   (1ULL << 22)

/* ── MSR access ─────────────────────────────────────── */
static inline uint64_t perf_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void perf_wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" :: "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── RDPMC wrappers ─────────────────────────────────── */
static inline uint64_t rdpmc_raw(uint32_t c)
{
    uint32_t lo, hi;
    __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(c));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdpmc_fixed(uint32_t c)
{
    /* Bit 30 of ECX selects fixed counter */
    return rdpmc_raw(c | (1U << 30));
}

/* ── Global state ───────────────────────────────────── */
bool         perf_enabled;
perf_phase_t perf_phases[PERF_PHASE_MAX];

/* ── Programming helpers ────────────────────────────── */
static void perf_program_counters(void)
{
    /* Stop all counters first so we can reset safely */
    perf_wrmsr(MSR_PERF_GLOBAL_CTRL, 0);

    /* Zero PMC values */
    for (int i = 0; i < 4; i++) perf_wrmsr(MSR_PMC(i), 0);
    for (int i = 0; i < 3; i++) perf_wrmsr(MSR_FIXED_CTR(i), 0);

    /* Enable all 3 fixed counters at both CPL=0 and CPL>0 (0x333 = 0b001100110011) */
    perf_wrmsr(MSR_PERF_FIXED_CTR_CTRL, 0x333ULL);

    /* Program programmable counters */
    uint64_t evt_common = PERFEVT_USR | PERFEVT_OS | PERFEVT_ENABLE;
    perf_wrmsr(MSR_PERFEVTSEL(0), PERF_EVT_L1D_MISS    | evt_common);
    perf_wrmsr(MSR_PERFEVTSEL(1), PERF_EVT_L2_MISS     | evt_common);
    perf_wrmsr(MSR_PERFEVTSEL(2), PERF_EVT_TLB_MISS    | evt_common);
    perf_wrmsr(MSR_PERFEVTSEL(3), PERF_EVT_BRANCH_MISS | evt_common);

    /* Global enable: fixed (bits 32,33,34) + PMC 0,1,2,3 (bits 0-3) */
    perf_wrmsr(MSR_PERF_GLOBAL_CTRL, 0x70000000FULL);
}

static void perf_enable_user_rdpmc(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 8);  /* CR4.PCE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
}

/* ── Public API ─────────────────────────────────────── */
void perf_init(void)
{
    if (!cpu_features.pmu_available) {
        serial_puts("[PERF] PMU unavailable (cpu_features.pmu_version=0)\n");
        return;
    }

    perf_enable_user_rdpmc();
    perf_program_counters();

    perf_enabled = true;

    serial_puts("[PERF] PMU enabled (3 fixed + 4 PMC) ver=");
    serial_putdec(cpu_features.pmu_version);
    serial_puts("\n");
}

void perf_init_ap(void)
{
    if (!cpu_features.pmu_available) return;
    perf_enable_user_rdpmc();
    perf_program_counters();
}

void perf_snapshot(perf_snapshot_t *s)
{
    if (!perf_enabled) {
        s->instructions = s->cycles = s->ref_cycles = 0;
        s->l1d_misses = s->l2_misses = s->tlb_misses = s->branch_misses = 0;
        return;
    }
    s->instructions  = rdpmc_fixed(0);
    s->cycles        = rdpmc_fixed(1);
    s->ref_cycles    = rdpmc_fixed(2);
    s->l1d_misses    = rdpmc_raw(0);
    s->l2_misses     = rdpmc_raw(1);
    s->tlb_misses    = rdpmc_raw(2);
    s->branch_misses = rdpmc_raw(3);
}

uint64_t perf_diff(uint64_t a, uint64_t b)
{
    /* Handle wraparound on a programmable counter. Counter width is
     * typically 48 bits on modern Intel but we're conservative: if b<a,
     * assume single wrap at 48 bits. */
    if (b >= a) return b - a;
    return (1ULL << 48) - a + b;
}

void perf_phase_enter(int slot, const char *name)
{
    if (!perf_enabled || slot < 0 || slot >= PERF_PHASE_MAX) return;
    perf_phase_t *p = &perf_phases[slot];
    p->name = name;
    perf_snapshot(&p->entry_snap);
}

void perf_phase_exit(int slot)
{
    if (!perf_enabled || slot < 0 || slot >= PERF_PHASE_MAX) return;
    perf_phase_t *p = &perf_phases[slot];
    perf_snapshot_t now;
    perf_snapshot(&now);
    p->total.instructions  += perf_diff(p->entry_snap.instructions,  now.instructions);
    p->total.cycles        += perf_diff(p->entry_snap.cycles,        now.cycles);
    p->total.ref_cycles    += perf_diff(p->entry_snap.ref_cycles,    now.ref_cycles);
    p->total.l1d_misses    += perf_diff(p->entry_snap.l1d_misses,    now.l1d_misses);
    p->total.l2_misses     += perf_diff(p->entry_snap.l2_misses,     now.l2_misses);
    p->total.tlb_misses    += perf_diff(p->entry_snap.tlb_misses,    now.tlb_misses);
    p->total.branch_misses += perf_diff(p->entry_snap.branch_misses, now.branch_misses);
    p->calls++;
}

void perf_reset(void)
{
    for (int i = 0; i < PERF_PHASE_MAX; i++) {
        perf_phase_t *p = &perf_phases[i];
        p->calls = 0;
        p->total.instructions = 0;
        p->total.cycles = 0;
        p->total.ref_cycles = 0;
        p->total.l1d_misses = 0;
        p->total.l2_misses = 0;
        p->total.tlb_misses = 0;
        p->total.branch_misses = 0;
    }
}

/* ── Shell integration ─────────────────────────────── */
static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}

static void dump_phase(int slot)
{
    perf_phase_t *p = &perf_phases[slot];
    if (p->calls == 0) return;
    serial_puts("  [");
    serial_puts(p->name ? p->name : "?");
    serial_puts("] calls=");
    serial_putdec(p->calls);
    serial_puts(" cycles=");
    serial_putdec(p->total.cycles);
    serial_puts(" insns=");
    serial_putdec(p->total.instructions);

    /* IPC */
    if (p->total.cycles > 0) {
        serial_puts(" IPC=");
        uint64_t ipc_x100 = (p->total.instructions * 100) / p->total.cycles;
        serial_putdec(ipc_x100 / 100);
        serial_puts(".");
        uint64_t frac = ipc_x100 % 100;
        if (frac < 10) serial_puts("0");
        serial_putdec(frac);
    }

    serial_puts("\n    L1d_miss=");
    serial_putdec(p->total.l1d_misses);
    serial_puts(" L2_miss=");
    serial_putdec(p->total.l2_misses);
    serial_puts(" TLB_miss=");
    serial_putdec(p->total.tlb_misses);
    serial_puts(" BR_miss=");
    serial_putdec(p->total.branch_misses);
    serial_puts("\n");
}

void cmd_perf(int argc, char **argv)
{
    if (!perf_enabled) {
        serial_puts("[PERF] PMU not enabled on this CPU\n");
        return;
    }

    if (argc >= 2 && streq(argv[1], "reset")) {
        perf_reset();
        serial_puts("[PERF] phase counters reset\n");
        return;
    }

    if (argc >= 2 && streq(argv[1], "snapshot")) {
        perf_snapshot_t s;
        perf_snapshot(&s);
        serial_puts("[PERF] snapshot: cycles=");
        serial_putdec(s.cycles);
        serial_puts(" insns=");
        serial_putdec(s.instructions);
        serial_puts(" L1d_miss=");
        serial_putdec(s.l1d_misses);
        serial_puts(" TLB_miss=");
        serial_putdec(s.tlb_misses);
        serial_puts("\n");
        return;
    }

    /* Default: dump all active phases */
    serial_puts("[PERF] Phase profiling:\n");
    for (int i = 0; i < PERF_PHASE_MAX; i++) dump_phase(i);
}
