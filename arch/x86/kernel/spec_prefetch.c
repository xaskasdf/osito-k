/*
 * OsitoK x86-64 — Speculative Pre-fetch via AP Cores
 *
 * When the BSP decides to continue running a process (no context switch),
 * an idle AP pre-fetches cache lines ahead of the current RIP. This brings
 * code and nearby data into the shared L3 cache before the BSP needs them.
 *
 * Simple approach: prefetch N cache lines forward from the current RIP.
 * No CFG analysis needed — just linear prefetch of the instruction stream.
 * Works well for sequential code; branches will miss but the cost is just
 * wasted prefetch bandwidth (no correctness issue).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Prefetch task submitted to AP ──────────────────────────── */

typedef struct {
    uint64_t rip;       /* Current instruction pointer of target process */
    uint64_t cr3;       /* Process page tables (for address translation) */
    uint32_t lines;     /* Number of cache lines to prefetch */
} prefetch_task_t;

static prefetch_task_t prefetch_arg;
static uint64_t prefetch_count;      /* Total prefetches issued */
static uint64_t last_prefetch_tick;  /* Rate limiter */

/* Worker function — runs on AP, prefetches code ahead of RIP */
static void prefetch_worker(void *arg, void *result)
{
    (void)result;
    prefetch_task_t *task = (prefetch_task_t *)arg;

    /* Prefetch code stream: 64 bytes per cache line, N lines forward.
     * Uses prefetcht0 (temporal, all cache levels).
     * We stay in the kernel CR3 — the process's code is mapped via
     * the upper-half direct map if it's a kernel thread, or we need
     * the process CR3 for userspace code. */

    /* Only prefetch if the address is in upper-half (kernel code) or
     * if the process CR3 is the kernel CR3 (kernel thread). Switching
     * CR3 on an AP for userspace prefetch is dangerous: demand-paged
     * pages may fault, and the AP's #PF handler isn't set up for it. */
    if (task->rip >= 0xFFFF800000000000ULL) {
        for (uint32_t i = 0; i < task->lines; i++) {
            uint64_t addr = task->rip + (uint64_t)i * 64;
            __asm__ volatile ("prefetcht0 (%0)" :: "r"(addr));
        }
    }
    /* Userspace prefetch: skip for now (needs AP-safe page fault handling) */
}

/* Called from sched_tick when BSP decides NOT to context switch.
 * Fires a prefetch on an idle AP for the current process's code. */
void spec_prefetch_ahead(uint64_t rip, uint64_t cr3)
{
    extern uint64_t idt_get_ticks(void);
    extern int smp_submit_any(void (*)(void*, void*), void*, void*);
    extern int ap_worker_count;

    /* Rate limit: at most once per 2 ticks (20ms) to avoid flooding */
    uint64_t now = idt_get_ticks();
    if (ap_worker_count <= 0 || (now - last_prefetch_tick) < 2)
        return;

    /* Only prefetch userspace code running in kernel context (syscall).
     * Pure kernel code (RIP in upper-half) is already hot from ISR.
     * For now, only kernel threads with kernel RIP benefit. */

    prefetch_arg.rip   = rip;
    prefetch_arg.cr3   = cr3;
    prefetch_arg.lines = 32;  /* 32 × 64 = 2KB ahead */

    __asm__ volatile ("mfence" ::: "memory");

    if (smp_submit_any(prefetch_worker, &prefetch_arg, 0) >= 0) {
        last_prefetch_tick = now;
        prefetch_count++;
    }
}
