/*
 * OsitoK x86-64 — Minimal Process Abstraction
 *
 * X-OS6: Process table, exec/exit lifecycle, per-process FD table.
 * Currently single-process (no preemptive scheduling yet — that's
 * a future enhancement using the APIC timer from X-OS1).
 *
 * Design:
 *   - process_t holds PID, state, name, FD table, memory regions
 *   - exec() creates a process, loads ELF, runs it
 *   - exit() cleans up and returns to kernel
 *   - waitpid() is synchronous (single process, no scheduler yet)
 */

#include "../include/types.h"
#include "../include/fd.h"
#include "../include/paging.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern uint64_t idt_get_ticks(void);
extern void fb_putdec(uint64_t val);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern uint64_t paging_get_kernel_cr3(void);

/* MSR access for per-thread TLS (X-THREAD) */
#define MSR_FS_BASE 0xC0000100
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr),
                      "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ELF loader */
extern int elf_exec(const char *filename, int argc, const char **argv);

/* Fork RW data restore (elf.c) — called after reaping forked child */
extern void elf_fork_restore(void);

/* Syscall state cleanup */
extern void syscall_reset_process(void);

/* Brk save/restore for fork+execve (syscall.c) */
extern void syscall_save_brk(void);
extern void syscall_restore_brk(void);

/* ── Constants ───────────────────────────────────────────────── */

#define MAX_PROCESSES   64  /* Increased from 16; further scaling via sys_caps planned */
#define MAX_NAME_LEN    64
#define MAX_REGIONS     32

/* Process states */
#define PROC_FREE       0
#define PROC_READY      1
#define PROC_RUNNING    2
#define PROC_BLOCKED    3
#define PROC_ZOMBIE     4

/* Scheduler constants (X-SCHED) */
#define SCHED_QUANTUM       5       /* default ticks per time slice (50ms @ 100Hz) */
#define KERNEL_STACK_SIZE   32768   /* 32KB per kernel thread */

/* QoS priority classes — higher value = higher priority */
#define QOS_IDLE            0
#define QOS_BACKGROUND      1
#define QOS_DEFAULT         2
#define QOS_INTERACTIVE     3
#define QOS_REALTIME        4
#define QOS_NUM_CLASSES     5

/* Quantum per QoS class (in timer ticks @ 100Hz):
 *   IDLE        = 20 ticks (200ms) — runs rarely, big slices when it does
 *   BACKGROUND  = 10 ticks (100ms) — batch work
 *   DEFAULT     =  5 ticks  (50ms) — normal processes
 *   INTERACTIVE =  2 ticks  (20ms) — low-latency UI/input
 *   REALTIME    =  1 tick   (10ms) — preempts everything, minimal slice
 */
static const uint32_t qos_quantum[QOS_NUM_CLASSES] = { 20, 10, 5, 2, 1 };

/* ── Memory region tracking ──────────────────────────────────── */

typedef struct {
    void    *base;
    uint64_t pages;
} mem_region_t;

/* ── Process structure ───────────────────────────────────────── */

typedef struct {
    uint32_t    pid;
    uint32_t    ppid;           /* parent PID (for wait4) */
    uint32_t    state;
    char        name[MAX_NAME_LEN];
    int32_t     exit_code;

    /* Memory regions (for cleanup) */
    mem_region_t regions[MAX_REGIONS];
    int          region_count;

    /* Address space (for future per-process paging) */
    uint64_t    cr3;

    /* Scheduler context (X-SCHED) */
    void    *kernel_stack;       /* allocated kernel stack (NULL for kernel proc) */
    uint64_t kernel_rsp;         /* saved RSP pointing to interrupt frame */
    uint32_t quantum;            /* ticks remaining in time slice */
    uint8_t  qos_class;          /* QOS_IDLE..QOS_REALTIME */

    /* Thread support (X-THREAD) */
    uint32_t tgid;               /* thread group ID (= leader's PID) */
    bool     is_thread;          /* true if created via CLONE_THREAD */
    uint64_t fs_base;            /* per-thread FS_BASE (TLS) */
    uint64_t *clear_child_tid;   /* set_tid_address / CLONE_CHILD_CLEARTID */

    /* Memory compression (macOS-style) */
    uint64_t last_active_tick;   /* tick when process last ran */
    bool     pages_compressed;   /* true if RW pages are compressed */

    /* Per-process file descriptor table. 8 KB. Last member so
     * any additions go above and the struct layout stays stable. */
    fd_entry_t fds[MAX_FDS];
} process_t;

/* ── Process table ───────────────────────────────────────────── */

static process_t proctab[MAX_PROCESSES];
static process_t *current_proc;
static uint32_t next_pid = 1;

/* Kernel return context — saved before exec, restored on exit */
extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int val);

uint64_t exec_jmpbuf[8];   /* setjmp/longjmp buffer — non-static for win32_exec */
int32_t  last_exit_code;

/* Target process for region registration during exec.
 * Preemptive scheduler can change current_proc mid-exec; anchor to
 * the newly-spawned process so proc_add_region() registers ELF
 * segments to the right slot, preventing a page-free miss on exit. */
static process_t *exec_target_proc;


/* Console I/O (shared with syscall.c) */
/* ── Allocate a process slot ─────────────────────────────────── */

static process_t *proc_alloc(const char *name)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) {
            process_t *p = &proctab[i];
            memset(p, 0, sizeof(*p));
            p->pid = next_pid++;
            p->ppid = current_proc ? current_proc->pid : 0;
            p->state = PROC_READY;
            p->cr3 = paging_get_kernel_cr3();

            /* Copy name */
            int j = 0;
            while (name[j] && j < MAX_NAME_LEN - 1) {
                p->name[j] = name[j];
                j++;
            }
            p->name[j] = '\0';

            /* QoS: default priority */
            p->qos_class = QOS_DEFAULT;

            /* Thread group = own PID by default (changed for CLONE_THREAD) */
            p->tgid = p->pid;
            p->is_thread = false;
            p->fs_base = 0;
            p->clear_child_tid = NULL;

            /* Note: file descriptors live in the global fd_table[] in
             * syscall.c, not per-process. syscall_init() sets up stdio. */

            return p;
        }
    }
    return NULL;
}

/* ── Free a process ──────────────────────────────────────────── */

static void proc_free(process_t *p)
{
    /* Free memory regions (ELF segments + stack) */
    for (int i = 0; i < p->region_count; i++) {
        if (p->regions[i].base && p->regions[i].pages > 0)
            mem_free_pages(p->regions[i].base, p->regions[i].pages);
    }

    /* Clean up compositor windows and SHM regions so the desktop
     * remains intact after the process exits (even on crash). */
    extern void compositor_cleanup_process(uint32_t pid);
    extern void shm_cleanup_process(uint32_t pid);
    compositor_cleanup_process(p->pid);
    shm_cleanup_process(p->pid);

    extern void kbd_flush(void);
    kbd_flush();

    /* Reset per-process syscall state (file FDs, brk heap) */
    syscall_reset_process();

    /* FDs live in the global fd_table[] in syscall.c — closed via
     * syscall_reset_process() which is called from compositor_cleanup_process. */

    /* X-PGTBL: release the per-process page tables if this process owns
     * one. Must run *after* syscall_reset_process because VMA cleanup
     * needs to walk the per-process PML4 to free faulted pages. */
    if (p->cr3 && p->cr3 != paging_get_kernel_cr3()) {
        extern void paging_free_process_cr3(uint64_t cr3);
        paging_free_process_cr3(p->cr3);
        p->cr3 = 0;
    }

    p->state = PROC_FREE;
}

/* ── Register memory region with current process (for cleanup) ── */

void proc_add_region(void *base, uint64_t pages)
{
    process_t *p = exec_target_proc ? exec_target_proc : current_proc;
    if (!p) return;
    if (p->region_count >= MAX_REGIONS) return;
    p->regions[p->region_count].base = base;
    p->regions[p->region_count].pages = pages;
    p->region_count++;
}

/* ── Public API ──────────────────────────────────────────────── */

/* Get current process */
process_t *proc_current(void)
{
    return current_proc;
}

/* Per-process fd_table accessor — used by syscall.c's `fd_table` macro.
 * Returns the current process's inline fds[] array. Every user-mode
 * syscall dispatch guarantees current_proc != NULL, so no null check. */
fd_entry_t *syscall_fds(void)
{
    return current_proc->fds;
}

/* X-PGTBL: CR3 of the currently-running process (0 if none). */
uint64_t proc_current_cr3(void)
{
    return current_proc ? current_proc->cr3 : 0;
}

/* Get current PID */
int32_t proc_current_pid(void)
{
    return current_proc ? (int32_t)current_proc->pid : 0;
}

/* Return the PID of the exec target if mid-exec, else current process.
 * The preemptive scheduler can change current_proc at any time; during
 * exec the newly-spawned process is pinned in exec_target_proc. */
uint32_t proc_exec_pid(void)
{
    if (exec_target_proc)
        return exec_target_proc->pid;
    return current_proc ? current_proc->pid : 0;
}

/* Get current parent PID */
int32_t proc_current_ppid(void)
{
    return current_proc ? (int32_t)current_proc->ppid : 0;
}

/* Get current thread group ID (X-THREAD) */
int32_t proc_current_tgid(void)
{
    return current_proc ? (int32_t)current_proc->tgid : 0;
}

/* Set clear_child_tid address (set_tid_address syscall) */
void proc_set_clear_child_tid(uint64_t *addr)
{
    if (current_proc)
        current_proc->clear_child_tid = addr;
}

/* Save FS_BASE to current process (called from arch_prctl SET_FS) */
void proc_set_fs_base(uint64_t addr)
{
    if (current_proc)
        current_proc->fs_base = addr;
}

/* Get current process name */
const char *proc_current_name(void)
{
    return current_proc ? current_proc->name : "kernel";
}

/* Check if any live process is currently executing a binary with this name.
 * Used by sys_open to enforce ETXTBSY on writes to executing binaries. */
bool proc_is_executing(const char *name)
{
    if (!name || !*name) return false;
    /* Skip leading "/" if present */
    if (*name == '/') name++;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) continue;
        if (proctab[i].state == PROC_ZOMBIE) continue;
        const char *pname = proctab[i].name;
        if (!*pname) continue;
        /* Compare basenames */
        int j = 0;
        while (name[j] && pname[j] && name[j] == pname[j]) j++;
        if (name[j] == '\0' && pname[j] == '\0') return true;
    }
    return false;
}

/* Get process by PID */
process_t *proc_find(uint32_t pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].pid == pid && proctab[i].state != PROC_FREE)
            return &proctab[i];
    }
    return NULL;
}

/* Forward declaration for thread exit cleanup (X-THREAD) */
static void thread_exit_cleanup(process_t *p);

/* Exception kill — safe to call from ISR context.
 * Marks the current process as ZOMBIE and enters HLT loop.
 * Uses no SSE/XMM instructions (compiled with -O0 for safety).
 * Returns 0 if the process was killed, -1 if not possible (kernel process). */
__attribute__((optimize("O0")))
int proc_exception_kill(int32_t code)
{
    process_t *p = current_proc;
    if (!p) return -1;

    /* Forked/spawned process — mark ZOMBIE, scheduler will switch away */
    if (p->kernel_stack) {
        thread_exit_cleanup(p);
        p->exit_code = code;
        p->state = PROC_ZOMBIE;
        __asm__ volatile ("sti");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Shell-started process — longjmp back to proc_exec */
    last_exit_code = code;
    kern_longjmp(exec_jmpbuf, 1);
    return 0; /* unreachable */
}

/* Process exit — called from sys_exit().
 * Two cases:
 *   1. Process started via proc_exec (shell) → longjmp back to shell
 *   2. Process started via fork/execve → mark ZOMBIE, halt for scheduler */
void proc_exit(int32_t code)
{
    process_t *p = current_proc;

    /* If this process has a kernel_stack, it was created by fork/sched_spawn.
     * Mark as ZOMBIE and let the scheduler switch away. Parent reaps via wait4. */
    if (p && p->kernel_stack) {
        thread_exit_cleanup(p);  /* X-THREAD: clear_child_tid + futex wake */
        p->exit_code = code;
        p->state = PROC_ZOMBIE;
        /* DON'T free memory regions here — we're still running on the
         * user stack (SYSCALL doesn't switch stacks in ring-0 OS).
         * proc_wait4 handles all cleanup after the process is reaped. */
        /* Halt — scheduler will pick another process on next tick */
        __asm__ volatile ("sti");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Normal exit — process started by proc_exec (shell's exec command).
     * longjmp back to proc_exec which cleans up. */
    last_exit_code = code;

    /* Restore IST1 to pristine value before longjmp. If called from INT 0x2E
     * context (ExitProcess → NtTerminateProcess → proc_exit), IST1 was modified
     * by int2e_stub. kern_longjmp bypasses the stub's IST1 restore (popq).
     * Without this, next INT 0x2E uses stale IST1 → stack corruption. */
    {
        extern uint64_t *tss_ist1_ptr;
        extern uint8_t ist1_stack[];
        if (tss_ist1_ptr)
            *tss_ist1_ptr = (uint64_t)(ist1_stack + 65536);
    }

    kern_longjmp(exec_jmpbuf, 1);
}

/* Wait for process to finish (synchronous) */
int proc_waitpid(uint32_t pid, int32_t *status)
{
    process_t *p = proc_find(pid);
    if (!p) return -1;

    /* In single-process mode, the process has already run to completion
     * by the time we call waitpid. */
    if (p->state == PROC_ZOMBIE) {
        if (status) *status = p->exit_code;
        proc_free(p);
        return 0;
    }

    return -1;  /* Process still running (shouldn't happen in single-proc mode) */
}

/* Exec: create process, load ELF, run synchronously.
 * Returns exit code, or -1 on error. */
int proc_exec(const char *filename, int argc, const char **argv)
{
    serial_puts("[PROC] exec '");
    serial_puts(filename);
    serial_puts("'\n");

    process_t *p = proc_alloc(filename);
    if (!p) {
        serial_puts("[PROC] Process table full\n");
        return -1;
    }

    /* X-PGTBL: give the new process its own PML4. Cloned from the
     * kernel's PML4 with private PDPT[0] PD[0] (covers vaddr 0..1 GB,
     * where ELF binaries typically load) and PDPT[1] PD (user mmap
     * range). Modifications to per-process page tables don't affect
     * the kernel or sibling processes. */
    extern uint64_t paging_create_process_cr3(void);
    extern void     paging_switch(uint64_t cr3);
    uint64_t new_cr3 = paging_create_process_cr3();
    if (new_cr3) {
        p->cr3 = new_cr3;
        paging_switch(new_cr3);  /* activate before elf_exec maps pages */
    }

    /* Set as current process and pin region registration target */
    process_t *prev = current_proc;
    current_proc = p;
    exec_target_proc = p;
    p->state = PROC_RUNNING;

    /* Seed stdio — proc_alloc zeros the fd table, so the new process
     * has no fds until we give it 0/1/2 = console. */
    extern void syscall_seed_stdio(fd_entry_t *fds);
    syscall_seed_stdio(p->fds);

    fb_puts_color(" [PID ", 0x0000AAFF);
    fb_putdec(p->pid);
    fb_puts("] ");
    fb_puts(filename);
    fb_puts("\n");

    /* Save kernel context so proc_exit() can longjmp back here */
    if (kern_setjmp(exec_jmpbuf) != 0) {
        /* Returned from proc_exit via longjmp.
         * SYSCALL disables interrupts (FMASK clears IF) and the longjmp
         * bypasses SYSRET which would re-enable them. Re-enable now. */
        __asm__ volatile ("sti");
        exec_target_proc = NULL;
        int code = last_exit_code;
        current_proc = prev;
        /* Switch back to the parent's CR3 (kernel CR3 if no parent). */
        if (prev && prev->cr3)
            paging_switch(prev->cr3);
        else
            paging_switch(paging_get_kernel_cr3());
        proc_free(p);
        return code;
    }

    /* Reset brk heap for the new process — prevents stale brk_current
     * from a previous process giving malloc a corrupted heap pointer. */
    extern void sys_brk_reset(void);
    sys_brk_reset();

    /* Execute ELF — this calls elf_exec which does not return
     * on success (jumps to ELF entry). On failure, returns here. */
    int ret = elf_exec(filename, argc, argv);

    /* If we get here, exec failed */
    serial_puts("[PROC] exec failed for '");
    serial_puts(filename);
    serial_puts("'\n");

    exec_target_proc = NULL;
    current_proc = prev;
    proc_free(p);

    return ret;
}

/* Process list (for shell `ps` command) — outputs to serial + framebuffer */
void proc_list(void)
{
    serial_puts("  PID  STATE    NAME\n");
    fb_puts("  PID  STATE    NAME\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) continue;

        serial_puts("  ");
        serial_putdec(proctab[i].pid);
        fb_puts("  ");
        fb_putdec(proctab[i].pid);

        const char *state_str;
        switch (proctab[i].state) {
        case PROC_READY:   state_str = "  READY    "; break;
        case PROC_RUNNING: state_str = "  RUNNING  "; break;
        case PROC_BLOCKED: state_str = "  BLOCKED  "; break;
        case PROC_ZOMBIE:  state_str = "  ZOMBIE   "; break;
        default:           state_str = "  ???      "; break;
        }
        serial_puts(state_str);
        fb_puts(state_str);

        serial_puts(proctab[i].name);
        serial_puts("\n");
        fb_puts(proctab[i].name);
        fb_puts("\n");
    }
}

/* ════════════════════════════════════════════════════════════════
 * X-SCHED: Preemptive Scheduler
 *
 * Timer-based round-robin context switching via APIC timer (100Hz).
 * The ISR stub in isr_stubs.S checks sched_switch_rsp after each
 * timer tick — if non-zero, it swaps RSP before popping GPRs,
 * effectively switching to the next process's saved interrupt frame.
 *
 * Each kernel thread gets a 16KB stack with a fake interrupt frame
 * at the top for the first context switch. After that, real frames
 * are saved/restored by the timer ISR.
 * ════════════════════════════════════════════════════════════════ */

/* ISR stub sets RSP to this value when non-zero (defined in isr_stubs.S) */
extern volatile uint64_t sched_switch_rsp;

static int      sched_current_idx = -1;
static bool     sched_enabled = false;
static uint64_t sched_switches = 0;

/* Thread exit trampoline — if a kernel thread's entry function returns,
 * execution lands here (the return address was placed below the fake frame). */
static void __attribute__((noreturn)) sched_thread_exit(void)
{
    if (sched_current_idx >= 0)
        proctab[sched_current_idx].state = PROC_ZOMBIE;
    for (;;) __asm__ volatile ("hlt");
}

/* ── sched_tick: called from ISR on every APIC timer tick ────── */

/* Read LAPIC ID from APIC_ID register (bits 31:24) */
extern volatile uint32_t *idt_get_apic_base(void);

static inline uint32_t sched_get_lapic_id(void)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return 0;
    return apic[0x020 / 4] >> 24;
}

void sched_tick(void *frame_ptr)
{
    if (!sched_enabled || sched_current_idx < 0)
        return;

    /* Only BSP (LAPIC ID 0) runs the scheduler — APs have their own
     * timer interrupts but must not touch single-CPU scheduler state */
    if (sched_get_lapic_id() != 0)
        return;

    process_t *cur = &proctab[sched_current_idx];

    /* Check if a higher-priority process is READY (preemption).
     * ZOMBIE/BLOCKED processes always force-switch immediately.
     * Otherwise, only switch if quantum expired or preempted. */
    bool force_switch = (cur->state == PROC_ZOMBIE || cur->state == PROC_BLOCKED);
    bool quantum_expired = false;

    if (!force_switch) {
        if (cur->quantum > 1) {
            cur->quantum--;
        } else {
            quantum_expired = true;
        }
    }

    /* Find best READY process: highest QoS class, round-robin within same class */
    int next_idx = -1;
    uint8_t best_qos = 0;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
        int idx = (sched_current_idx + i) % MAX_PROCESSES;
        if (proctab[idx].state == PROC_READY) {
            if (proctab[idx].qos_class >= best_qos) {
                best_qos = proctab[idx].qos_class;
                next_idx = idx;
            }
        }
    }

    if (next_idx < 0) {
        /* No other runnable process — reset quantum, continue */
        if (quantum_expired)
            cur->quantum = qos_quantum[cur->qos_class];
        return;
    }

    /* Decide whether to actually switch:
     *  - force_switch (ZOMBIE/BLOCKED): always switch
     *  - quantum_expired: switch to best_qos candidate
     *  - preemption: higher-priority READY process preempts current */
    if (!force_switch && !quantum_expired) {
        /* Still have quantum — only preempt if candidate is strictly higher priority */
        if (best_qos <= cur->qos_class)
            return;
        /* Preemption: higher priority process is waiting */
    }

    /* ── Context switch ────────────────────────────────────────── */

    /* Save current process: frame_ptr is RSP pointing to the saved
     * GPRs on this process's stack (set by ISR stub before calling
     * isr_handler). Store it so we can restore later. */
    cur->kernel_rsp = (uint64_t)frame_ptr;
    cur->fs_base = rdmsr(MSR_FS_BASE);  /* save per-thread TLS */
    /* Only mark as READY if currently RUNNING.
     * ZOMBIE processes must stay ZOMBIE — proc_wait4 relies on this. */
    if (cur->state == PROC_RUNNING)
        cur->state = PROC_READY;

    /* ── Memory compression: track idle time ── */
    {
        uint64_t now = idt_get_ticks();

        /* Update departing process's activity timestamp */
        cur->last_active_tick = now;

        /* Compress pages of processes idle too long (macOS-style).
         * Only do this occasionally (every 100 ticks = 1s) to avoid
         * overhead in the hot scheduler path. */
        extern uint32_t memcompress_idle_threshold(void);
        extern int memcompress_process_pages(uint32_t, void*, uint64_t);
        extern int memcompress_restore_process(uint32_t);

        if ((now & 0xFF) == 0) {  /* ~every 2.5 seconds */
            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t *p = &proctab[i];
                if (p->state == PROC_BLOCKED && !p->pages_compressed &&
                    p->last_active_tick > 0 &&
                    (now - p->last_active_tick) > memcompress_idle_threshold()) {
                    /* Compress this idle process's pages */
                    for (int r = 0; r < p->region_count; r++) {
                        if (p->regions[r].base && p->regions[r].pages > 0) {
                            memcompress_process_pages(p->pid,
                                p->regions[r].base, p->regions[r].pages);
                        }
                    }
                    p->pages_compressed = true;
                }
            }
        }
    }

    /* Load next process */
    process_t *next = &proctab[next_idx];

    /* Decompress pages if needed before running */
    if (next->pages_compressed) {
        extern int memcompress_restore_process(uint32_t);
        memcompress_restore_process(next->pid);
        next->pages_compressed = false;
    }

    next->state = PROC_RUNNING;
    next->quantum = qos_quantum[next->qos_class];
    next->last_active_tick = idt_get_ticks();
    current_proc = next;
    sched_current_idx = next_idx;
    wrmsr(MSR_FS_BASE, next->fs_base);  /* restore per-thread TLS */

    /* X-PGTBL: switch CR3 to the new process's PML4 if it has one.
     * Threads of the same process share cr3 (proc_clone_thread copies
     * parent->cr3). Processes started before X-PGTBL was wired (or by
     * sched_spawn for kernel threads) have cr3 == kernel_cr3, in which
     * case paging_switch is a no-op load of the same value. */
    if (next->cr3) {
        extern void paging_switch(uint64_t cr3);
        paging_switch(next->cr3);
    }

    /* Tell ISR stub to switch RSP before popping GPRs.
     * The stub will: mov sched_switch_rsp → RSP, then pop + iretq
     * using the new process's saved interrupt frame. */
    sched_switch_rsp = next->kernel_rsp;

    sched_switches++;
}

/* ── sched_spawn: create a preemptively-scheduled kernel thread ── */

int sched_spawn(const char *name, void (*entry)(void))
{
    process_t *p = proc_alloc(name);
    if (!p) {
        serial_puts("[SCHED] No free process slot\n");
        return -1;
    }

    /* Allocate kernel stack via the upper-half direct map. The stack
     * top RSP needs to be reachable from any process's CR3, and only
     * PML4[256] (the kernel mirror) is shared across all CR3s once the
     * lower-half identity map disappears. Storing the upper-half view
     * in p->kernel_stack also means writes to the fake interrupt frame
     * below go through PML4[256]. */
    void *stack_phys = mem_alloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!stack_phys) {
        p->state = PROC_FREE;
        serial_puts("[SCHED] Stack allocation failed\n");
        return -1;
    }
    void *stack = PHYS_TO_VIRT(stack_phys);
    p->kernel_stack = stack;

    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;

    /* Place fake interrupt frame at top of stack (176 bytes = 22 × uint64_t).
     * When the scheduler first switches to this process, the ISR stub
     * pops GPRs from this frame and iretq jumps to the entry function. */
    uint64_t frame_addr = (stack_top - 176) & ~0xFULL;  /* 16-byte aligned */
    uint64_t *frame = (uint64_t *)frame_addr;
    memset(frame, 0, 176);

    /* Place return address below frame — if entry() returns, it lands
     * in sched_thread_exit() which marks the process as ZOMBIE. */
    uint64_t ret_addr = frame_addr - 8;
    *(uint64_t *)ret_addr = (uint64_t)sched_thread_exit;

    /* Fill CPU-pushed portion of interrupt frame:
     * [17]=RIP  [18]=CS  [19]=RFLAGS  [20]=RSP  [21]=SS */
    frame[17] = (uint64_t)entry;    /* RIP = thread entry function */
    frame[18] = 0x38;               /* CS  = kernel code segment */
    frame[19] = 0x202;              /* RFLAGS = IF=1, reserved bit 1 */
    frame[20] = ret_addr;           /* RSP = just below frame (ABI: 8 mod 16) */
    frame[21] = 0x30;               /* SS  = kernel data segment */

    /* Set initial scheduler state */
    p->kernel_rsp = frame_addr;
    p->state = PROC_READY;
    p->quantum = qos_quantum[p->qos_class];

    /* Auto-activate scheduler on first spawn */
    if (!sched_enabled) {
        /* Set up kernel process for scheduling */
        proctab[sched_current_idx].quantum = qos_quantum[proctab[sched_current_idx].qos_class];
        sched_enabled = true;
        serial_puts("[SCHED] Preemptive scheduling activated\n");
    }

    serial_puts("[SCHED] Spawned '");
    serial_puts(name);
    serial_puts("' PID ");
    serial_putdec(p->pid);
    serial_puts("\n");

    return (int)p->pid;
}

/* ── sched_yield: voluntarily give up remaining time slice ────── */

void sched_yield(void)
{
    if (!sched_enabled || sched_current_idx < 0) return;
    proctab[sched_current_idx].quantum = 0;
    __asm__ volatile ("hlt");  /* wait for next timer tick → switch */
}

/* ── sched_stats: return context switch count ────────────────── */

uint64_t sched_get_switches(void) { return sched_switches; }
bool sched_is_enabled(void) { return sched_enabled; }

/* ── Test threads (used by shell 'sched' command) ────────────── */

extern uint64_t idt_get_ticks(void);

void sched_test_a(void)
{
    for (int i = 0; i < 20; i++) {
        serial_puts("A");
        fb_puts("A");
        /* Busy-wait ~500ms (50 ticks @ 100Hz) */
        uint64_t start = idt_get_ticks();
        while (idt_get_ticks() - start < 50)
            __asm__ volatile ("hlt");
    }
    serial_puts("\n[thread_a] done\n");
    fb_puts("\n[thread_a] done\n");
}

void sched_test_b(void)
{
    for (int i = 0; i < 20; i++) {
        serial_puts("B");
        fb_puts("B");
        uint64_t start = idt_get_ticks();
        while (idt_get_ticks() - start < 50)
            __asm__ volatile ("hlt");
    }
    serial_puts("\n[thread_b] done\n");
    fb_puts("\n[thread_b] done\n");
}

/* ════════════════════════════════════════════════════════════════
 * X-SYSCALL40: fork / wait4 / execve
 *
 * fork() creates a child with its own kernel stack containing a
 * fake interrupt frame. The scheduler's IRETQ delivers the child
 * to userspace with RAX=0 (fork return value for child).
 * Parent returns immediately with child PID.
 *
 * Both processes share the same address space (identity-mapped).
 * The child typically calls execve() immediately (gets new stack
 * and code) or _exit(). Like vfork, but using the scheduler.
 *
 * proc_wait4():
 *   Polls/blocks for ZOMBIE children, returns exit status.
 *
 * proc_execve():
 *   Replace current process image with new ELF binary.
 * ════════════════════════════════════════════════════════════════ */

#define ECHILD  10
#define WNOHANG  1

/* User RSP saved by syscall_entry.S before any pushes */
extern volatile uint64_t syscall_user_rsp;

/*
 * proc_fork — create child process via scheduler.
 *
 * Creates a child with a fake interrupt frame on its own kernel stack.
 * When the scheduler switches to the child, IRETQ delivers it to
 * userspace at the instruction after the SYSCALL, with RAX=0.
 * Parent returns child PID immediately.
 *
 * ISR frame layout (176 bytes, 22 × uint64_t):
 *   [0]=R15  [1]=R14  [2]=R13  [3]=R12  [4]=R11  [5]=R10
 *   [6]=R9   [7]=R8   [8]=RBP  [9]=RDI  [10]=RSI [11]=RDX
 *   [12]=RCX [13]=RBX [14]=RAX [15]=vector [16]=error_code
 *   [17]=RIP [18]=CS  [19]=RFLAGS [20]=RSP [21]=SS
 */
int32_t proc_fork(void)
{
    if (!current_proc) return -1;

    process_t *parent = current_proc;

    /* We need the user's register state. The SYSCALL entry saved
     * registers on the stack in this order (from RSP, 14 pushes):
     *   RSP+0:  R9     (push #14)
     *   RSP+8:  R8     (push #13)
     *   RSP+16: R10    (push #12)
     *   RSP+24: RDX    (push #11)
     *   RSP+32: RSI    (push #10)
     *   RSP+40: RDI    (push #9)
     *   RSP+48: R11    (push #8) = user RFLAGS
     *   RSP+56: RCX    (push #7) = user RIP
     *   RSP+64: R15    (push #6)
     *   RSP+72: R14    (push #5)
     *   RSP+80: R13    (push #4)
     *   RSP+88: R12    (push #3)
     *   RSP+96: RBX    (push #2)
     *   RSP+104: RBP   (push #1)
     * But we can't access RSP from C (clobbered by function calls).
     * Instead we use the known values we CAN get:
     *   - User RIP  = RCX saved in syscall frame
     *   - User RFLAGS = R11 saved in syscall frame
     *   - User RSP = saved by syscall_entry.S in syscall_user_rsp
     *   - Callee-saved regs (RBX,RBP,R12-R15) = read via inline asm
     * The child doesn't need exact arg regs (RDI,RSI,etc.) because
     * fork() returns only RAX, and callee-saved regs + RSP/RIP are
     * what matters for resuming the C caller.
     */

    /* Read ALL user registers from the SYSCALL save area on the stack.
     * syscall_entry.S saves user RSP in syscall_user_rsp before any pushes,
     * then pushes 14 registers in this order:
     *   push RBP, RBX, R12, R13, R14, R15, RCX(=RIP), R11(=RFLAGS),
     *        RDI, RSI, RDX, R10, R8, R9
     * So frame_base = user_rsp - 14*8 and the offsets are:
     *   [0]=R9  [1]=R8  [2]=R10  [3]=RDX  [4]=RSI  [5]=RDI
     *   [6]=R11(RFLAGS) [7]=RCX(RIP)
     *   [8]=R15  [9]=R14  [10]=R13  [11]=R12  [12]=RBX  [13]=RBP
     *
     * IMPORTANT: We MUST read from the stack frame, NOT from inline asm.
     * GCC may be using callee-saved registers (RBX, RBP, R12-R15) for
     * its own purposes inside proc_fork. The stack frame has the actual
     * user values saved at SYSCALL entry.
     */
    uint64_t user_rsp = syscall_user_rsp;
    uint64_t *frame_base = (uint64_t *)(user_rsp - 14 * 8);

    uint64_t user_r9     = frame_base[0];
    uint64_t user_r8     = frame_base[1];
    uint64_t user_r10    = frame_base[2];
    uint64_t user_rdx    = frame_base[3];
    uint64_t user_rsi    = frame_base[4];
    uint64_t user_rdi    = frame_base[5];
    uint64_t user_rflags = frame_base[6];   /* saved R11 = user RFLAGS */
    uint64_t user_rip    = frame_base[7];   /* saved RCX = user RIP */
    uint64_t r15         = frame_base[8];
    uint64_t r14         = frame_base[9];
    uint64_t r13         = frame_base[10];
    uint64_t r12         = frame_base[11];
    uint64_t rbx         = frame_base[12];
    uint64_t rbp         = frame_base[13];

    /* Allocate child process */
    process_t *child = proc_alloc(parent->name);
    if (!child) {
        serial_puts("[FORK] Process table full\n");
        return -1;
    }

    child->ppid = parent->pid;

    /* Clone parent's fd_table into the child. Each inherited pipe fd
     * bumps the corresponding read_refs/write_refs on the shared
     * pipe_buf_t, so sys_close in one process doesn't kill the pipe
     * end in the other. This is the fix for zsh's subshell pipe. */
    memcpy(child->fds, parent->fds, sizeof(child->fds));
    for (int i = 0; i < MAX_FDS; i++) {
        if (!child->fds[i].open) continue;
        if (child->fds[i].type == FD_TYPE_PIPE && child->fds[i].pipe) {
            pipe_buf_t *p = (pipe_buf_t *)child->fds[i].pipe;
            if ((child->fds[i].oflags & 0x3) == 0 /* O_RDONLY */)
                p->read_refs++;
            else
                p->write_refs++;
        }
    }

    child->region_count = 0;

    /* Allocate kernel stack for the child via the upper-half mirror. */
    void *stack_phys = mem_alloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!stack_phys) {
        child->state = PROC_FREE;
        serial_puts("[FORK] Stack alloc failed\n");
        return -1;
    }
    void *stack = PHYS_TO_VIRT(stack_phys);
    child->kernel_stack = stack;

    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;

    /* Build fake interrupt frame at top of child's kernel stack.
     * When the scheduler switches to this process, the ISR stub
     * pops GPRs from this frame and IRETQ returns to userspace. */
    uint64_t child_frame_addr = (stack_top - 176) & ~0xFULL;
    uint64_t *cf = (uint64_t *)child_frame_addr;
    memset(cf, 0, 176);

    /* GPRs — match parent's values */
    cf[0]  = r15;           /* R15 */
    cf[1]  = r14;           /* R14 */
    cf[2]  = r13;           /* R13 */
    cf[3]  = r12;           /* R12 */
    cf[4]  = user_rflags;   /* R11 (not used after IRETQ, but matches parent) */
    cf[5]  = user_r10;      /* R10 */
    cf[6]  = user_r9;       /* R9  */
    cf[7]  = user_r8;       /* R8  */
    cf[8]  = rbp;           /* RBP */
    cf[9]  = user_rdi;      /* RDI */
    cf[10] = user_rsi;      /* RSI */
    cf[11] = user_rdx;      /* RDX */
    cf[12] = user_rip;      /* RCX (unused after IRETQ) */
    cf[13] = rbx;           /* RBX */
    cf[14] = 0;             /* RAX = 0 → fork returns 0 to child */
    cf[15] = 0;             /* vector (unused) */
    cf[16] = 0;             /* error_code (unused) */

    /* Allocate a separate user stack for the child.
     * Without this, parent and child share the same user stack and
     * the scheduler's concurrent execution corrupts both frames.
     * Copy a portion of the parent's stack so the child has valid
     * return addresses and local variables for the short time before
     * it calls exec() or _exit(). */
#define CHILD_USTACK_SIZE  (64 * 1024)  /* Same size as ELF loader */
#define CHILD_USTACK_COPY  (32 * 1024)  /* Copy top 32KB of used stack */

    void *child_ustack_phys = mem_alloc_aligned(CHILD_USTACK_SIZE, 4096);
    if (!child_ustack_phys) {
        mem_free_pages(stack_phys, KERNEL_STACK_SIZE / 4096);
        child->state = PROC_FREE;
        serial_puts("[FORK] User stack alloc failed\n");
        return -1;
    }
    void *child_ustack = PHYS_TO_VIRT(child_ustack_phys);
    memset(child_ustack, 0, CHILD_USTACK_SIZE);

    /* The parent's stack grows downward. user_rsp is the current top of the
     * used portion. We copy CHILD_USTACK_COPY bytes above user_rsp (the used
     * frames: return addresses, local variables, etc.). */
    uint64_t child_ustack_top = (uint64_t)child_ustack + CHILD_USTACK_SIZE;
    uint64_t copy_size = CHILD_USTACK_COPY;
    /* Copy from parent's [user_rsp .. user_rsp + copy_size) to child */
    memcpy((void *)(child_ustack_top - copy_size),
           (void *)user_rsp, copy_size);

    /* Child's RSP = same offset from top as parent's */
    uint64_t child_user_rsp = child_ustack_top - copy_size;

    /* Register child user stack for cleanup on exit. region.base is
     * the phys address handed to mem_free_pages later. */
    if (child->region_count < MAX_REGIONS) {
        child->regions[child->region_count].base = child_ustack_phys;
        child->regions[child->region_count].pages = CHILD_USTACK_SIZE / 4096;
        child->region_count++;
    }

    /* IRETQ frame */
    cf[17] = user_rip;      /* RIP = return to userspace after SYSCALL */
    cf[18] = 0x38;          /* CS  = kernel code segment */
    cf[19] = user_rflags | 0x200;  /* RFLAGS with IF=1 */
    cf[20] = child_user_rsp; /* RSP = child's own stack (copied from parent) */
    cf[21] = 0x30;          /* SS  = kernel data segment */

    /* Adjust child's RBP to point into the new stack if it was in the
     * parent's stack range. This is needed for frame pointer unwinding. */
    if (rbp >= user_rsp && rbp < user_rsp + copy_size) {
        cf[8] = child_user_rsp + (rbp - user_rsp);  /* RBP adjusted */
    }

    /* Relocate saved frame pointers within the copied stack.
     *
     * The copied stack contains saved RBP values (pushed by function
     * prologues) that point into the PARENT's stack. When the child
     * returns through these functions, `pop %rbp` restores a parent
     * pointer, causing the child to read/write parent stack memory.
     *
     * Fix: scan the copied region for any 8-byte value that falls
     * within the parent's copied range [user_rsp .. user_rsp+copy_size),
     * and adjust it by the parent→child delta. This catches all saved
     * frame pointers without needing to walk the frame chain. */
    {
        int64_t delta = (int64_t)child_user_rsp - (int64_t)user_rsp;
        uint64_t *scan = (uint64_t *)child_user_rsp;
        uint64_t scan_count = copy_size / 8;
        for (uint64_t i = 0; i < scan_count; i++) {
            uint64_t val = scan[i];
            if (val >= user_rsp && val < user_rsp + copy_size) {
                scan[i] = val + delta;
            }
        }
    }

    /* Set up scheduler state — child inherits parent's QoS class */
    child->qos_class = parent->qos_class;
    child->kernel_rsp = child_frame_addr;
    child->state = PROC_READY;
    child->quantum = qos_quantum[child->qos_class];

    /* Ensure scheduler tracks the parent (currently running) process.
     * proc_exec started the parent outside the scheduler, so sched_current_idx
     * may still point to the kernel process. Fix that now. */
    int parent_idx = (int)(parent - &proctab[0]);
    sched_current_idx = parent_idx;
    parent->quantum = qos_quantum[parent->qos_class];

    /* The parent (ELF process from proc_exec) doesn't have a kernel_stack
     * because it was started via the setjmp/longjmp lifecycle. It's running
     * on the same stack as the kernel. The scheduler can still save/restore
     * its context via the timer ISR frame — kernel_rsp will be set by
     * sched_tick when the timer next fires. */

    /* Enable scheduler if not already */
    if (!sched_enabled) {
        sched_enabled = true;
        serial_puts("[SCHED] Preemptive scheduling activated (by fork)\n");
    }

    /* Save parent's brk state before child can execve+reset it */
    syscall_save_brk();

    /* Parent returns child PID immediately */
    return (int32_t)child->pid;
}

/*
 * proc_clone_thread — create a new thread (X-THREAD).
 *
 * Unlike fork, threads share the parent's address space (identity-mapped
 * OS means this is automatic). The child_stack parameter provides the
 * thread's own stack. The thread runs fn(arg) by starting at user_rip
 * (which should be the clone() return point in libc's __clone wrapper).
 *
 * clone flags: CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
 *              CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
 *              CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID
 *
 * Returns child TID to parent, 0 to child (via RAX in ISR frame).
 */
int32_t proc_clone_thread(uint64_t child_stack, uint64_t parent_tidptr,
                          uint64_t child_tidptr, uint64_t tls)
{
    if (!current_proc) return -1;

    process_t *parent = current_proc;

    /* Read parent's register state from SYSCALL save frame */
    uint64_t user_rsp = syscall_user_rsp;
    uint64_t *frame_base = (uint64_t *)(user_rsp - 14 * 8);

    uint64_t user_r9     = frame_base[0];
    uint64_t user_r8     = frame_base[1];
    uint64_t user_r10    = frame_base[2];
    uint64_t user_rdx    = frame_base[3];
    uint64_t user_rsi    = frame_base[4];
    uint64_t user_rdi    = frame_base[5];
    uint64_t user_rflags = frame_base[6];
    uint64_t user_rip    = frame_base[7];
    uint64_t r15         = frame_base[8];
    uint64_t r14         = frame_base[9];
    uint64_t r13         = frame_base[10];
    uint64_t r12         = frame_base[11];
    uint64_t rbx         = frame_base[12];
    uint64_t rbp         = frame_base[13];

    /* Allocate thread in process table */
    process_t *thread = proc_alloc(parent->name);
    if (!thread) {
        serial_puts("[THREAD] Process table full\n");
        return -1;
    }

    /* Thread shares parent's thread group */
    thread->tgid = parent->tgid;
    thread->ppid = parent->pid;
    thread->is_thread = true;

    /* Clone parent's fd_table into the thread. Same pattern as fork —
     * bump pipe refcounts for every inherited pipe end. Thread-local
     * fd divergence is accepted (POSIX would require sharing via an
     * fd_owner pointer; follow-up if a real multithreaded-fd test
     * needs it). */
    memcpy(thread->fds, parent->fds, sizeof(thread->fds));
    for (int i = 0; i < MAX_FDS; i++) {
        if (!thread->fds[i].open) continue;
        if (thread->fds[i].type == FD_TYPE_PIPE && thread->fds[i].pipe) {
            pipe_buf_t *p = (pipe_buf_t *)thread->fds[i].pipe;
            if ((thread->fds[i].oflags & 0x3) == 0 /* O_RDONLY */)
                p->read_refs++;
            else
                p->write_refs++;
        }
    }

    thread->region_count = 0;

    /* Set per-thread TLS */
    thread->fs_base = tls;

    /* CLONE_PARENT_SETTID: write child TID to parent's memory */
    if (parent_tidptr) {
        *(int *)parent_tidptr = (int)thread->pid;
    }

    /* CLONE_CHILD_CLEARTID: remember address for futex wake on exit */
    if (child_tidptr) {
        thread->clear_child_tid = (uint64_t *)child_tidptr;
        /* Also write TID there now (CLONE_CHILD_SETTID behavior) */
        *(int *)child_tidptr = (int)thread->pid;
    }

    /* Allocate kernel stack for the thread via the upper-half mirror. */
    void *kstack_phys = mem_alloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!kstack_phys) {
        thread->state = PROC_FREE;
        serial_puts("[THREAD] Kernel stack alloc failed\n");
        return -1;
    }
    void *kstack = PHYS_TO_VIRT(kstack_phys);
    thread->kernel_stack = kstack;

    uint64_t kstack_top = (uint64_t)kstack + KERNEL_STACK_SIZE;

    /* Build fake interrupt frame */
    uint64_t frame_addr = (kstack_top - 176) & ~0xFULL;
    uint64_t *cf = (uint64_t *)frame_addr;
    memset(cf, 0, 176);

    /* GPRs — copy parent's values */
    cf[0]  = r15;
    cf[1]  = r14;
    cf[2]  = r13;
    cf[3]  = r12;
    cf[4]  = user_rflags;
    cf[5]  = user_r10;
    cf[6]  = user_r9;
    cf[7]  = user_r8;
    cf[8]  = rbp;
    cf[9]  = user_rdi;
    cf[10] = user_rsi;
    cf[11] = user_rdx;
    cf[12] = user_rip;
    cf[13] = rbx;
    cf[14] = 0;             /* RAX = 0 → clone returns 0 to child */
    cf[15] = 0;
    cf[16] = 0;

    /* IRETQ frame — child uses provided stack, NOT parent's */
    cf[17] = user_rip;
    cf[18] = 0x38;
    cf[19] = user_rflags | 0x200;
    cf[20] = child_stack;   /* Thread's own stack (provided by caller) */
    cf[21] = 0x30;

    /* Scheduler state — thread inherits parent's QoS class */
    thread->qos_class = parent->qos_class;
    thread->kernel_rsp = frame_addr;
    thread->state = PROC_READY;
    thread->quantum = qos_quantum[thread->qos_class];

    /* Ensure scheduler is tracking parent */
    int parent_idx = (int)(parent - &proctab[0]);
    sched_current_idx = parent_idx;
    parent->quantum = qos_quantum[parent->qos_class];

    if (!sched_enabled) {
        sched_enabled = true;
        serial_puts("[SCHED] Preemptive scheduling activated (by clone)\n");
    }

    return (int32_t)thread->pid;
}

/* ── Futex wait queue (X-THREAD) ────────────────────────────── */

#define FUTEX_HASH_SIZE  32
#define MAX_FUTEX_WAITERS 32

typedef struct {
    uint64_t    addr;       /* futex user address */
    int         proc_idx;   /* index into proctab (process waiting) */
    bool        active;
} futex_waiter_t;

static futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];

/* futex_wait — block current process until woken.
 * Returns 0 on success, -EAGAIN if value mismatch. */
int futex_do_wait(uint64_t uaddr, int expected)
{
    volatile int *addr = (volatile int *)uaddr;

    /* Atomic check: if value changed, return immediately */
    if (*addr != expected)
        return -11; /* EAGAIN */

    /* Find a free waiter slot */
    int slot = -1;
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -12; /* ENOMEM — too many waiters */

    process_t *cur = current_proc;
    int cur_idx = (int)(cur - &proctab[0]);

    /* Register waiter */
    futex_waiters[slot].addr = uaddr;
    futex_waiters[slot].proc_idx = cur_idx;
    futex_waiters[slot].active = true;

    /* Block: mark process as BLOCKED, yield to scheduler.
     * sched_tick skips BLOCKED processes. We'll be woken by futex_wake. */
    cur->state = PROC_BLOCKED;

    /* Yield CPU — scheduler will switch away on next tick.
     * We spin on HLT until the scheduler preempts us out. */
    while (cur->state == PROC_BLOCKED) {
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }

    /* Woken — clear waiter slot (may already be cleared by wake) */
    futex_waiters[slot].active = false;

    return 0;
}

/* futex_wake — wake up to 'count' processes waiting on uaddr.
 * Returns number of processes woken. */
int futex_do_wake(uint64_t uaddr, int count)
{
    int woken = 0;
    for (int i = 0; i < MAX_FUTEX_WAITERS && woken < count; i++) {
        if (futex_waiters[i].active && futex_waiters[i].addr == uaddr) {
            int idx = futex_waiters[i].proc_idx;
            if (idx >= 0 && idx < MAX_PROCESSES &&
                proctab[idx].state == PROC_BLOCKED) {
                proctab[idx].state = PROC_READY;
                woken++;
            }
            futex_waiters[i].active = false;
        }
    }
    return woken;
}

/* Thread exit cleanup: clear_child_tid + futex wake (X-THREAD) */
static void thread_exit_cleanup(process_t *p)
{
    if (p->clear_child_tid) {
        /* Write 0 to the TID address (signals thread death to parent) */
        *(int *)p->clear_child_tid = 0;
        /* Wake any futex waiter on that address (pthread_join uses this) */
        futex_do_wake((uint64_t)p->clear_child_tid, 1);
        p->clear_child_tid = NULL;
    }
}

/*
 * proc_wait4 — wait for child process state change.
 * pid==-1: any child. pid>0: specific child.
 * Returns child PID on success, -ECHILD if no children.
 */
int32_t proc_wait4(int32_t pid, int *wstatus, int options)
{
    if (!current_proc) return -ECHILD;

    uint32_t my_pid = current_proc->pid;
    bool has_children = false;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) continue;
        if (proctab[i].ppid != my_pid) continue;

        has_children = true;

        if (pid > 0 && proctab[i].pid != (uint32_t)pid)
            continue;

        if (proctab[i].state == PROC_ZOMBIE) {
            int32_t child_pid = (int32_t)proctab[i].pid;
            if (wstatus)
                *wstatus = (proctab[i].exit_code & 0xFF) << 8;

            /* Free child resources */
            proctab[i].state = PROC_FREE;
            for (int r = 0; r < proctab[i].region_count; r++) {
                if (proctab[i].regions[r].base &&
                    proctab[i].regions[r].pages > 0)
                    mem_free_pages(proctab[i].regions[r].base,
                                   proctab[i].regions[r].pages);
            }
            proctab[i].region_count = 0;
            if (proctab[i].kernel_stack) {
                mem_free_pages((void *)VIRT_TO_PHYS(proctab[i].kernel_stack),
                               KERNEL_STACK_SIZE / 4096);
                proctab[i].kernel_stack = NULL;
            }

            /* Restore parent's RW data and brk heap state after child
             * overwrote them (fork+execve of same binary in identity-mapped OS). */
            elf_fork_restore();
            syscall_restore_brk();

            return child_pid;
        }
    }

    if (!has_children)
        return -ECHILD;

    /* WNOHANG: return 0 if no child has exited yet */
    if (options & WNOHANG)
        return 0;

    /* Blocking wait: poll until a child becomes ZOMBIE.
     * SYSCALL entry disables interrupts (FMASK clears IF).
     * We MUST enable them so the scheduler can run the child.
     * STI + HLT + CLI: allow one timer tick, then re-disable.
     * "memory" clobber forces the compiler to re-read proctab
     * from memory after each tick (state changes via scheduler). */
    for (int tries = 0; tries < 10000; tries++) {
        __asm__ volatile ("sti; hlt; cli" ::: "memory");

        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proctab[i].state != PROC_ZOMBIE) continue;
            if (proctab[i].ppid != my_pid) continue;
            if (pid > 0 && proctab[i].pid != (uint32_t)pid) continue;

            int32_t child_pid = (int32_t)proctab[i].pid;
            if (wstatus)
                *wstatus = (proctab[i].exit_code & 0xFF) << 8;

            proctab[i].state = PROC_FREE;
            /* Free memory regions (user stack, ELF segments) */
            for (int r = 0; r < proctab[i].region_count; r++) {
                if (proctab[i].regions[r].base &&
                    proctab[i].regions[r].pages > 0)
                    mem_free_pages(proctab[i].regions[r].base,
                                   proctab[i].regions[r].pages);
            }
            proctab[i].region_count = 0;
            if (proctab[i].kernel_stack) {
                mem_free_pages((void *)VIRT_TO_PHYS(proctab[i].kernel_stack),
                               KERNEL_STACK_SIZE / 4096);
                proctab[i].kernel_stack = NULL;
            }

            /* Restore parent's RW data and brk heap after reaping forked child */
            elf_fork_restore();
            syscall_restore_brk();

            return child_pid;
        }
    }

    return -ECHILD;
}

/*
 * proc_execve — replace current process image with new ELF.
 * Called from sys_execve. The current process gets a new ELF loaded.
 * For forked children: elf_exec gives them their own stack+segments.
 */
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, uint64_t);

int proc_execve(const char *path, char *const argv[])
{
    if (!current_proc || !path) return -1;

    process_t *p = current_proc;

    /* /proc/self/exe or /proc/<pid>/exe → re-exec current binary */
    if (strcmp(path, "/proc/self/exe") == 0) {
        path = p->name;
    } else if (strncmp(path, "/proc/", 6) == 0) {
        /* /proc/<pid>/exe — find last component */
        const char *end = path + strlen(path);
        if (end - path >= 4 && strcmp(end - 4, "/exe") == 0) {
            path = p->name;
        }
    }

    serial_puts("[EXECVE] pid ");
    serial_putdec(p->pid);
    serial_puts(" -> '");
    serial_puts(path);
    serial_puts("'\n");

    /* Update process name */
    int j = 0;
    const char *basename = path;
    for (const char *c = path; *c; c++)
        if (*c == '/') basename = c + 1;
    while (basename[j] && j < MAX_NAME_LEN - 1) {
        p->name[j] = basename[j];
        j++;
    }
    p->name[j] = '\0';

    /* Free old memory regions ONLY if this process owns them.
     * Forked children share parent's memory (identity-mapped OS),
     * so we must NOT free the parent's regions. Only free if this
     * process has its own ELF regions (from a previous execve). */
    if (!p->kernel_stack) {
        /* Non-forked process (shell exec) — safe to free */
        for (int i = 0; i < p->region_count; i++) {
            if (p->regions[i].base && p->regions[i].pages > 0)
                mem_free_pages(p->regions[i].base, p->regions[i].pages);
        }
    }
    p->region_count = 0;

    /* Count argc from argv */
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }

    /* Reset per-process syscall state (brk, file FDs) */
    syscall_reset_process();

    /* Non-stdio FDs are closed by syscall_reset_process() in syscall.c
     * (which operates on the global fd_table). */

    /* Execute the ELF — does not return on success.
     * elf_exec loads segments, sets up stack, jumps to entry.
     * When the process exits, proc_exit() handles cleanup. */
    int ret = elf_exec(path, argc, (const char **)argv);

    /* If we get here, exec failed */
    serial_puts("[EXECVE] Failed: ");
    serial_puts(path);
    serial_puts("\n");

    return ret;
}

/* ── Initialize process subsystem ────────────────────────────── */

/* ── QoS API ─────────────────────────────────────────────────── */

/* Set QoS class for a process by PID. 0 = current process. */
int sched_set_qos(uint32_t pid, uint8_t qos)
{
    if (qos >= QOS_NUM_CLASSES)
        return -1;

    process_t *target = NULL;
    if (pid == 0) {
        target = current_proc;
    } else {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proctab[i].state != PROC_FREE && proctab[i].pid == pid) {
                target = &proctab[i];
                break;
            }
        }
    }
    if (!target) return -1;

    target->qos_class = qos;
    /* Adjust quantum immediately if upgrading */
    uint32_t new_q = qos_quantum[qos];
    if (new_q < target->quantum)
        target->quantum = new_q;
    return 0;
}

/* Get QoS class for a process by PID. 0 = current process. */
uint8_t sched_get_qos(uint32_t pid)
{
    if (pid == 0 && current_proc)
        return current_proc->qos_class;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state != PROC_FREE && proctab[i].pid == pid)
            return proctab[i].qos_class;
    }
    return QOS_DEFAULT;
}

/* ── sched_spawn_qos: spawn with explicit QoS class ─────────── */

int sched_spawn_qos(const char *name, void (*entry)(void), uint8_t qos)
{
    int pid = sched_spawn(name, entry);
    if (pid > 0 && qos < QOS_NUM_CLASSES)
        sched_set_qos((uint32_t)pid, qos);
    return pid;
}

/* ── Process initialization ──────────────────────────────────── */

void proc_init(void)
{
    serial_puts("[PROC] Initializing process subsystem...\n");

    memset(proctab, 0, sizeof(proctab));
    current_proc = NULL;

    /* Create PID 1 (kernel) */
    process_t *kernel = proc_alloc("kernel");
    if (kernel) {
        kernel->state = PROC_RUNNING;
        current_proc = kernel;
        sched_current_idx = (int)(kernel - &proctab[0]);
        /* Seed kernel process with stdin/stdout/stderr = console.
         * All future processes inherit or reset these via execve. */
        extern void syscall_seed_stdio(fd_entry_t *fds);
        syscall_seed_stdio(kernel->fds);
        serial_puts("[PROC] Kernel process PID ");
        serial_putdec(kernel->pid);
        serial_puts("\n");
    }

    fb_puts(" Process subsystem ready\n");
}

uint32_t proc_count_active(void)
{
    uint32_t count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state != PROC_FREE)
            count++;
    }
    return count;
}
