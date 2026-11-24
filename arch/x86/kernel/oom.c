/*
 * OsitoK x86-64 — OOM Killer + Swap Stub
 *
 * Out-of-memory handler: selects and kills the process using
 * the most memory when allocation fails. Swap file support
 * is stubbed for future NVMe-backed paging.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t mem_get_free(void);
extern uint64_t mem_get_total(void);

/* Process table access */
extern void *proc_get_by_index(int idx) __attribute__((weak));
extern const char *proc_get_name(void *p) __attribute__((weak));
extern uint32_t proc_get_pid(void *p) __attribute__((weak));
extern uint64_t proc_get_mem_usage(void *p) __attribute__((weak));
extern void proc_kill(void *p, int sig) __attribute__((weak));

#define SIGKILL 9

/* ── OOM Score Calculation ───────────────────────────────────── */

/* Score a process: higher score = more likely to be killed.
 * Based on Linux's badness heuristic:
 *   - Memory usage is the primary factor
 *   - PID 1 (init/kernel) is protected
 *   - Processes using >50% of RAM get boosted score */
static uint64_t oom_score(void *proc)
{
    if (!proc || !proc_get_mem_usage) return 0;

    uint32_t pid = proc_get_pid ? proc_get_pid(proc) : 0;
    if (pid <= 1) return 0;  /* Never kill PID 0 or 1 */

    uint64_t mem = proc_get_mem_usage(proc);
    uint64_t total = mem_get_total();

    /* Base score = memory in KB */
    uint64_t score = mem / 1024;

    /* Boost if using >50% of total RAM */
    if (total > 0 && mem > total / 2)
        score *= 2;

    return score;
}

/* ── OOM Killer ──────────────────────────────────────────────── */

/* Select and kill the worst offender. Returns PID killed, or 0. */
uint32_t oom_kill(void)
{
    if (!proc_get_by_index || !proc_kill) return 0;

    serial_puts("\n[OOM] === OUT OF MEMORY ===\n");
    serial_puts("[OOM] Free: ");
    serial_putdec(mem_get_free() / 1024);
    serial_puts(" KB / ");
    serial_putdec(mem_get_total() / (1024 * 1024));
    serial_puts(" MB\n");

    /* Find process with highest OOM score */
    void *victim = NULL;
    uint64_t worst_score = 0;

    for (int i = 0; i < 256; i++) {
        void *p = proc_get_by_index(i);
        if (!p) continue;

        uint64_t score = oom_score(p);
        if (score > worst_score) {
            worst_score = score;
            victim = p;
        }
    }

    if (!victim) {
        serial_puts("[OOM] No killable process found!\n");
        return 0;
    }

    uint32_t pid = proc_get_pid ? proc_get_pid(victim) : 0;
    const char *name = proc_get_name ? proc_get_name(victim) : "?";
    uint64_t mem = proc_get_mem_usage ? proc_get_mem_usage(victim) : 0;

    serial_puts("[OOM] Killing PID ");
    serial_putdec(pid);
    serial_puts(" (");
    serial_puts(name);
    serial_puts(") using ");
    serial_putdec(mem / 1024);
    serial_puts(" KB (score=");
    serial_putdec(worst_score);
    serial_puts(")\n");

    proc_kill(victim, SIGKILL);
    return pid;
}

/* ── Swap Stub ───────────────────────────────────────────────── */

/* Swap is not implemented yet. These stubs allow future integration
 * with NVMe-backed page swapping. */

static uint64_t swap_total;
static uint64_t swap_used;
static bool     swap_enabled;

int swap_init(uint64_t swap_size_bytes)
{
    swap_total = swap_size_bytes;
    swap_used = 0;
    swap_enabled = (swap_size_bytes > 0);
    if (swap_enabled) {
        serial_puts("[SWAP] Swap enabled: ");
        serial_putdec(swap_size_bytes / (1024 * 1024));
        serial_puts(" MB\n");
    }
    return 0;
}

/* Page out: write a page to swap, return swap offset */
int64_t swap_out(uint64_t page_phys)
{
    (void)page_phys;
    if (!swap_enabled || swap_used >= swap_total) return -1;
    /* TODO: write page to NVMe swap partition */
    swap_used += 4096;
    return (int64_t)(swap_used - 4096);  /* Return swap offset */
}

/* Page in: read a page from swap */
int swap_in(int64_t swap_offset, uint64_t page_phys)
{
    (void)swap_offset; (void)page_phys;
    if (!swap_enabled) return -1;
    /* TODO: read page from NVMe swap partition */
    if (swap_used >= 4096) swap_used -= 4096;
    return 0;
}

uint64_t swap_get_total(void) { return swap_total; }
uint64_t swap_get_used(void) { return swap_used; }
uint64_t swap_get_free(void) { return swap_total - swap_used; }
