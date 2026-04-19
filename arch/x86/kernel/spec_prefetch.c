/*
 * OsitoK x86-64 — Speculative Pre-fetch via AP Cores
 *
 * When the BSP decides to continue running a process (no context switch),
 * an idle AP pre-fetches cache lines ahead of the current RIP. This brings
 * code and nearby data into the shared L3 cache before the BSP needs them.
 *
 * For userspace code: walks the process's page tables (without CR3 switch)
 * to resolve virtual→physical, then prefetches via the kernel direct map.
 * Pages that aren't present (demand-paged) are silently skipped.
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* Page table walk without CR3 switch (defined in paging.c) */
extern uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t virt);

/* ── Prefetch task ──────────────────────────────────────────── */

typedef struct {
    uint64_t rip;       /* Current instruction pointer */
    uint64_t cr3;       /* Process page tables */
    uint32_t lines;     /* Number of cache lines to prefetch */
} prefetch_task_t;

static prefetch_task_t prefetch_arg;
static uint64_t prefetch_count;
static uint64_t last_prefetch_tick;

/* Worker — runs on AP, prefetches code ahead of RIP.
 * For kernel code (upper-half): direct prefetch.
 * For userspace code: page-table walk → direct-map prefetch. */
static void prefetch_worker(void *arg, void *result)
{
    (void)result;
    prefetch_task_t *task = (prefetch_task_t *)arg;
    uint64_t rip = task->rip;
    uint64_t cr3 = task->cr3;

    if (rip >= 0xFFFF800000000000ULL) {
        /* Kernel code — already in direct map, prefetch directly */
        for (uint32_t i = 0; i < task->lines; i++) {
            uint64_t addr = rip + (uint64_t)i * 64;
            __asm__ volatile ("prefetcht0 (%0)" :: "r"(addr));
        }
        return;
    }

    /* Userspace code — walk page tables to get physical address,
     * then prefetch via the kernel direct map (PHYS_TO_VIRT).
     * No CR3 switch needed — we read the PTs through the direct map. */
    if (!cr3) return;

    for (uint32_t i = 0; i < task->lines; i++) {
        uint64_t va = rip + (uint64_t)i * 64;

        /* Check if we crossed a page boundary — only re-walk then */
        uint64_t *pte = paging_get_pte_in_cr3(cr3, va);
        if (!pte || !(*pte & 1 /* PTE_PRESENT */))
            continue;  /* Page not present (demand-paged) — skip */

        uint64_t phys = (*pte & 0x000FFFFFFFFFF000ULL) | (va & 0xFFF);
        uint64_t direct_va = (uint64_t)PHYS_TO_VIRT(phys);
        __asm__ volatile ("prefetcht0 (%0)" :: "r"(direct_va));
    }
}

/* Called from sched_tick when BSP decides NOT to context switch. */
void spec_prefetch_ahead(uint64_t rip, uint64_t cr3)
{
    extern uint64_t idt_get_ticks(void);
    extern int smp_submit_any(void (*)(void*, void*), void*, void*);
    extern int ap_worker_count;

    uint64_t now = idt_get_ticks();
    if (ap_worker_count <= 0 || (now - last_prefetch_tick) < 2)
        return;

    prefetch_arg.rip   = rip;
    prefetch_arg.cr3   = cr3;
    prefetch_arg.lines = 32;  /* 32 × 64 = 2KB ahead */

    __asm__ volatile ("mfence" ::: "memory");

    if (smp_submit_any(prefetch_worker, &prefetch_arg, 0) >= 0) {
        last_prefetch_tick = now;
        prefetch_count++;
    }
}

uint64_t spec_prefetch_get_count(void) { return prefetch_count; }
