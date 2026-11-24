/*
 * OsitoK x86-64 — Speculative Pre-fetch via AP Cores
 *
 * When the BSP decides to continue running a process (no context switch),
 * an idle AP pre-fetches cache lines ahead of the current RIP. This brings
 * code and nearby data into the shared L3 cache before the BSP needs them.
 *
 * The BSP snapshots direct-map targets while the process is still current.
 * Workers issue non-faulting prefetch hints without retaining an address
 * space or walking page tables after the owning process may have exited.
 */

#include "../include/types.h"
#include "../include/paging.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── Prefetch task ──────────────────────────────────────────── */

#define PREFETCH_LINES 32U

typedef struct {
    uint64_t targets[PREFETCH_LINES];
    uint32_t count;
} prefetch_task_t;

static prefetch_task_t prefetch_arg;
static volatile int prefetch_pending;
static uint64_t prefetch_count;
static uint64_t last_prefetch_tick;

/* Reclaimed target frames are harmless: PREFETCH is only a cache hint.
 * Never dereference a saved PTE or CR3 from asynchronous work. */
static void prefetch_worker(void *arg, void *result)
{
    (void)result;
    prefetch_task_t *task = (prefetch_task_t *)arg;
    for (uint32_t i = 0; i < task->count; i++)
        __asm__ volatile ("prefetcht0 (%0)" :: "r"(task->targets[i]));
    __atomic_store_n(&prefetch_pending, 0, __ATOMIC_RELEASE);
}

/* Called from sched_tick when BSP decides NOT to context switch. */
void spec_prefetch_ahead(uint64_t rip, uint64_t cr3)
{
    extern uint64_t idt_get_ticks(void);
    extern int smp_submit_ff(void (*)(void*, void*), void*, void*);
    extern int ap_worker_count;

    uint64_t now = idt_get_ticks();
    if (ap_worker_count <= 0 || (now - last_prefetch_tick) < 2)
        return;

    if (__atomic_exchange_n(&prefetch_pending, 1, __ATOMIC_ACQ_REL))
        return;

    /* Called with BSP IRQs masked in sched_tick, before leaving the current
     * process. Resolve at most two pages, not one walk per cache line. */
    uint64_t va = rip & ~63ULL;
    uint64_t last_page = UINT64_MAX, phys_page = UINT64_MAX;
    prefetch_arg.count = 0;
    last_prefetch_tick = now;
    for (uint32_t i = 0; i < PREFETCH_LINES; i++) {
        uint64_t target = UINT64_MAX;
        if (va >= 0xFFFF800000000000ULL) {
            target = va;
        } else if (va < 0x0000800000000000ULL && cr3) {
            uint64_t page = va & ~4095ULL;
            if (page != last_page) {
                phys_page = paging_translate_in_cr3(cr3, page);
                last_page = page;
            }
            if (phys_page != UINT64_MAX)
                target = (uint64_t)PHYS_TO_VIRT(phys_page + (va & 4095ULL));
        }
        if (target != UINT64_MAX)
            prefetch_arg.targets[prefetch_arg.count++] = target;
        if (va > UINT64_MAX - 64)
            break;
        va += 64;
    }

    if (prefetch_arg.count &&
        smp_submit_ff(prefetch_worker, &prefetch_arg, 0) >= 0) {
        prefetch_count++;
    } else {
        __atomic_store_n(&prefetch_pending, 0, __ATOMIC_RELEASE);
    }
}

uint64_t spec_prefetch_get_count(void) { return prefetch_count; }
