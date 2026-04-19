/*
 * arch/x86/include/perf.h — Hardware Performance Monitoring Unit
 *
 * Programs the x86 PMU (3 fixed counters + up to 4 programmable) with
 * events for L1d / L2 / TLB / branch misses and exposes them via a
 * zero-overhead RDPMC-based snapshot API (~40 cycles per snapshot).
 *
 * Also exports a per-phase accumulator so inference, network, fs code
 * can tag hot regions and read aggregate counters from shell.
 *
 * Requires cpu_features_detect() to have run (for pmu_available flag).
 */
#ifndef OSITOK_PERF_H
#define OSITOK_PERF_H

#include "types.h"
#include "stdint.h"

/* Single snapshot — captured via RDPMC, safe from any context. */
typedef struct {
    uint64_t instructions;    /* fixed 0 */
    uint64_t cycles;          /* fixed 1 */
    uint64_t ref_cycles;      /* fixed 2 */
    uint64_t l1d_misses;      /* PMC 0 */
    uint64_t l2_misses;       /* PMC 1 */
    uint64_t tlb_misses;      /* PMC 2 */
    uint64_t branch_misses;   /* PMC 3 */
} perf_snapshot_t;

/* Per-phase accumulator (diff between enter/exit snapshots). */
typedef struct {
    const char     *name;
    uint64_t        calls;
    perf_snapshot_t total;
    perf_snapshot_t entry_snap;  /* scratch; used by enter/exit pair */
} perf_phase_t;

/* Phase slots — caller passes a stable index. */
#define PERF_PHASE_FORWARD       0
#define PERF_PHASE_QKV           1
#define PERF_PHASE_ATTN          2
#define PERF_PHASE_FFN           3
#define PERF_PHASE_SAMPLE        4
#define PERF_PHASE_NET_RX        5
#define PERF_PHASE_NET_TX        6
#define PERF_PHASE_FS_READ       7
#define PERF_PHASE_SYS_INFERENCE 8
#define PERF_PHASE_USER          9   /* generic user-defined */
#define PERF_PHASE_MAX           16

extern perf_phase_t perf_phases[PERF_PHASE_MAX];
extern bool         perf_enabled;

/* Init — called from main.c after cpu_features_detect(). */
void perf_init(void);
void perf_init_ap(void);     /* Per-AP CR4.PCE + counter enable */

/* Snapshot all counters. ~40 cycles. Safe in ISR context. */
void perf_snapshot(perf_snapshot_t *out);

/* Compute delta b - a, handling counter wraparound. */
uint64_t perf_diff(uint64_t a, uint64_t b);

/* Per-phase tagging. Paired calls; nesting is NOT supported. */
void perf_phase_enter(int slot, const char *name);
void perf_phase_exit(int slot);

/* Reset all phase counters. */
void perf_reset(void);

/* Shell command — registered by shell.c. */
void cmd_perf(int argc, char **argv);

/* Event encodings (public so other subsystems can reprogram counters). */
#define PERF_EVT_L1D_MISS      0x41CBULL   /* MEM_LOAD_UOPS_RETIRED.L1_MISS (generic) */
#define PERF_EVT_L2_MISS       0x10F1ULL   /* MEM_LOAD_UOPS_RETIRED.L2_MISS */
#define PERF_EVT_TLB_MISS      0x0149ULL   /* DTLB_LOAD_MISSES.MISS_CAUSES_A_WALK */
#define PERF_EVT_BRANCH_MISS   0x00C5ULL   /* BR_MISP_RETIRED */

#endif /* OSITOK_PERF_H */
