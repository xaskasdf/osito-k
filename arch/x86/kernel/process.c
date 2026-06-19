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
#include "smp.h"

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

/* Forward decl: sched_current_idx is defined (= -1) further down but used by
 * the FS-base/TLS diagnostics above its definition (parallel fork/TLS work). */
static int sched_current_idx;
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
#define KERNEL_STACK_SIZE   262144  /* 256KB per kernel thread */

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

/* TSC-deadline mode: quanta in microseconds (100x finer than periodic).
 *   IDLE        = 100ms
 *   BACKGROUND  =  20ms
 *   DEFAULT     =   5ms
 *   INTERACTIVE =   2ms
 *   REALTIME    = 100us  ← 100x more precise than the 10ms periodic tick */
static const uint32_t qos_quantum_us[QOS_NUM_CLASSES] = {
    100000, 20000, 5000, 2000, 100
};

/* ── Memory region tracking ──────────────────────────────────── */

typedef struct {
    void    *base;
    uint64_t pages;
    uint64_t virt_base;   /* VA mapped via paging_map_page for ET_EXEC,
                            * or 0 if identity/upper-half only (no unmap
                            * needed at execve). */
} mem_region_t;

/* ── Process structure ───────────────────────────────────────── */

typedef struct {
    uint32_t    pid;
    uint32_t    ppid;           /* parent PID (for wait4) */
    uint32_t    state;
    char        name[MAX_NAME_LEN];
    int32_t     exit_code;
    uint64_t    zombie_tick;        /* idt_get_ticks() when state→ZOMBIE; 0 if alive */

    /* Memory regions (for cleanup) */
    mem_region_t regions[MAX_REGIONS];
    int          region_count;

    /* Address space (for future per-process paging) */
    uint64_t    cr3;
    bool        owns_cr3;         /* true = this process allocated its own
                                   * isolated CR3 (proc_exec / execve) and
                                   * must free it on exit. false = it SHARES
                                   * another process's CR3 (fork child until
                                   * execve, or a thread) → must NOT free it. */
    uint32_t    vforked_parent;   /* PID of a vfork parent blocked until this
                                   * process execve's or exits (0 = none). */
    uint64_t    user_stack_top;   /* highest VA of this process's user stack
                                   * (stack_base + USER_STACK_SIZE), set by the
                                   * ELF loader. fork() copies the WHOLE used
                                   * stack [user_rsp, user_stack_top). 0=unset */

    /* Scheduler context (X-SCHED) */
    void    *kernel_stack;       /* allocated kernel stack (NULL for kernel proc) */
    uint64_t kernel_rsp;         /* saved RSP pointing to interrupt frame */
    uint32_t quantum;            /* ticks remaining in time slice */
    uint8_t  qos_class;          /* QOS_IDLE..QOS_REALTIME */
    int16_t  runq_next;          /* next in same-QoS run queue, -1 = tail */

    /* Thread support (X-THREAD) */
    uint32_t tgid;               /* thread group ID (= leader's PID) */
    bool     is_thread;          /* true if created via CLONE_THREAD */
    uint64_t fs_base;            /* per-thread FS_BASE (TLS) */
    uint64_t *clear_child_tid;   /* set_tid_address / CLONE_CHILD_CLEARTID */

    /* Memory compression (macOS-style) */
    uint64_t last_active_tick;   /* tick when process last ran */
    bool     pages_compressed;   /* true if RW pages are compressed */

    /* User-space symbol table (captured at elf_load time from the
     * binary's .symtab/.strtab). Used by the crash-dump symbolizer
     * in idt.c and the user backtrace walker. NULL if symbols are
     * unavailable (stripped binary, demand-paged, or capture failed). */
    void     *user_symtab;       /* kmalloc'd copy of .symtab */
    uint64_t  user_symtab_size;  /* bytes */
    char     *user_strtab;       /* kmalloc'd copy of .strtab */
    uint64_t  user_strtab_size;  /* bytes */
    uint64_t  user_load_bias;    /* PIE: rip = st_value + bias */

    /* x87 + SSE state buffer used by isr_common's fxsave64/fxrstor64.
     * 512 bytes, 16-aligned. Points-to is stored in fpu_state_ptr while
     * this process is current, so interrupts save/restore into the
     * owning process's slot and context switches atomically change
     * which slot the ISR touches next. */
    __attribute__((aligned(16))) uint8_t fpu_state[512];

    /* Speculation analysis — populated at ELF load time (spec_analyze.c) */
    void        *spec_info;          /* spec_analysis_t* or NULL */

    /* Debug: saved CS from frame at preemption time.
     * If this differs from frame[18] at restore time,
     * the frame was overwritten. */
    uint64_t saved_frame_cs;
    uint64_t saved_frame_rip;

    /* Refcounted file descriptor table (shared between threads).
     * Allocated separately via kmalloc to avoid bloating process_t. */
    fd_table_t *fd_table;

    /* io_predict per-process "last file opened" for speculative prefetch.
     * Kept in-struct so TCC/compositor/shell don't pollute each other's
     * observed sequences. See arch/x86/kernel/io_predict.c. */
    char         last_opened[32];
} process_t;

/* ── Process table ───────────────────────────────────────────── */

static process_t proctab[MAX_PROCESSES];
static process_t *current_proc;
static uint32_t next_pid = 1;

/* ── O(1) run queue: bitmap + per-QoS linked lists ──────────── */

/* One bit per proctab slot. With MAX_PROCESSES=64, fits in one uint64_t.
 * Bit i is set iff proctab[i].state == PROC_READY. */
static uint64_t ready_bitmap;

/* Per-QoS run queue heads. Each is an index into proctab, or -1 if empty.
 * process_t gains a runq_next field (int16_t) for chaining. */
static int16_t runq_head[QOS_NUM_CLASSES] = { -1, -1, -1, -1, -1 };
static int16_t runq_tail[QOS_NUM_CLASSES] = { -1, -1, -1, -1, -1 };

/* PID -> proctab index for O(1) lookup. -1 = no process with that PID. */
#define MAX_PID 4096
static int16_t pid_to_idx[MAX_PID];

/* Forward decl — defined after process_t's runq_next field is visible */
static void runq_enqueue(int idx);
static void runq_dequeue(int idx);
static inline void proc_transition(process_t *p, uint32_t new_state);
static void runq_init(void);

/* ── Exec cache: reuse read-only ELF segments across exec() ─── */

#define EXEC_CACHE_SIZE 8
#define EXEC_CACHE_MAX_SEGS 4

typedef struct {
    char     name[64];
    uint32_t name_hash;
    uint64_t file_size;
    struct {
        void    *phys_base;
        uint64_t vaddr;
        uint64_t pages;
    } segs[EXEC_CACHE_MAX_SEGS];
    int      seg_count;
    uint32_t refcount;
    uint64_t last_used_tick;
} exec_cache_entry_t;

static exec_cache_entry_t exec_cache[EXEC_CACHE_SIZE];

static uint32_t exec_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h;
}

exec_cache_entry_t *exec_cache_lookup(const char *name, uint64_t fsize)
{
    uint32_t h = exec_hash(name);
    for (int i = 0; i < EXEC_CACHE_SIZE; i++) {
        if (exec_cache[i].name_hash == h &&
            exec_cache[i].file_size == fsize &&
            strcmp(exec_cache[i].name, name) == 0)
            return &exec_cache[i];
    }
    return NULL;
}

void exec_cache_store(const char *name, uint64_t fsize,
                      void *phys, uint64_t vaddr, uint64_t pages)
{
    int best = 0;
    uint64_t oldest = ~0ULL;
    for (int i = 0; i < EXEC_CACHE_SIZE; i++) {
        if (exec_cache[i].name_hash == 0) { best = i; break; }
        if (exec_cache[i].refcount == 0 &&
            exec_cache[i].last_used_tick < oldest) {
            oldest = exec_cache[i].last_used_tick;
            best = i;
        }
    }
    exec_cache_entry_t *e = &exec_cache[best];
    if (e->refcount > 0) return;
    memset(e, 0, sizeof(*e));
    int j = 0;
    while (name[j] && j < 63) { e->name[j] = name[j]; j++; }
    e->name_hash = exec_hash(name);
    e->file_size = fsize;
    e->segs[0].phys_base = phys;
    e->segs[0].vaddr = vaddr;
    e->segs[0].pages = pages;
    e->seg_count = 1;
    e->refcount = 1;
    extern uint64_t idt_get_ticks(void);
    e->last_used_tick = idt_get_ticks();
}

void exec_cache_release(const char *name)
{
    uint32_t h = exec_hash(name);
    for (int i = 0; i < EXEC_CACHE_SIZE; i++) {
        if (exec_cache[i].name_hash == h &&
            strcmp(exec_cache[i].name, name) == 0) {
            if (exec_cache[i].refcount > 0)
                exec_cache[i].refcount--;
            return;
        }
    }
}

/* ── Per-process FPU/SSE state ────────────────────────────────
 * `isr_common` does `fxsave64 (%rax)` / `fxrstor64 (%rax)` where
 * %rax is loaded from `fpu_state_ptr`. That pointer tracks the
 * currently scheduled process's `fpu_state` field, so an ISR that
 * fires while process A is running saves A's FPU state on entry;
 * if the scheduler chooses to switch to process B inside the C
 * handler, we also rewrite `fpu_state_ptr` to B's slot, and the
 * exit path's `fxrstor64` restores B's state on the way out via
 * IRETQ. For the boot window (before any process exists) and for
 * kernel threads that never got a process_t, the pointer points
 * at `fpu_state_kernel` below. */
__attribute__((aligned(16))) uint8_t fpu_state_kernel[512];
uint8_t *fpu_state_ptr = fpu_state_kernel;  /* BSP default (legacy, index 0) */
uint64_t fpu_corrupt_val;  /* set by isr_common when fpu_state_ptr is corrupt */

/* Per-CPU FPU state pointers — indexed by LAPIC ID (0..15).
 * BSP (LAPIC 0) uses fpu_state_ptrs[0] = process's fpu_state.
 * APs use fpu_state_ptrs[lapic_id] = their own static buffer.
 * The ISR stub reads LAPIC ID and indexes into this array. */
#define FPU_MAX_CPUS 16
uint8_t *fpu_state_ptrs[FPU_MAX_CPUS];
__attribute__((aligned(16))) uint8_t fpu_state_ap_bufs[FPU_MAX_CPUS][512];

static void fpu_state_init(uint8_t *state)
{
    memset(state, 0, 512);
    *(uint16_t *)(state + 0) = 0x037F;      /* x87 control word */
    *(uint32_t *)(state + 24) = 0x00001F80; /* MXCSR: mask SIMD FP traps */
    *(uint32_t *)(state + 28) = 0x0000FFFF; /* valid MXCSR feature mask */
}

void fpu_percpu_init(void)
{
    fpu_state_init(fpu_state_kernel);
    for (int i = 0; i < FPU_MAX_CPUS; i++)
        fpu_state_init(fpu_state_ap_bufs[i]);

    /* BSP (index 0) starts with kernel default */
    fpu_state_ptrs[0] = fpu_state_kernel;
    /* APs get their own static buffers */
    for (int i = 1; i < FPU_MAX_CPUS; i++)
        fpu_state_ptrs[i] = fpu_state_ap_bufs[i];
}

/* Update both current_proc and fpu_state_ptr together so the ISR
 * save/restore path and the scheduler agree about which FPU slot
 * is live. NULL means "back to the kernel-default slot". */
static inline void set_current_proc(process_t *p)
{
    current_proc = p;
    uint8_t *fpu = p ? p->fpu_state : fpu_state_kernel;
    fpu_state_ptr = fpu;           /* legacy global (BSP only) */
    fpu_state_ptrs[0] = fpu;      /* per-CPU array slot for BSP */
}

/* Kernel return context — saved before exec, restored on exit */
extern int  kern_setjmp(uint64_t *buf) __attribute__((returns_twice));
extern void kern_longjmp(uint64_t *buf, int val);

uint64_t exec_jmpbuf[9];   /* setjmp/longjmp buffer — slot[8] holds saved CR3 */
int32_t  last_exit_code;

/* Target process for region registration during exec.
 * Preemptive scheduler can change current_proc mid-exec; anchor to
 * the newly-spawned process so proc_add_region() registers ELF
 * segments to the right slot, preventing a page-free miss on exit. */
static process_t *exec_target_proc;

/* The parent of a *synchronous* exec (proc_exec — kernel shell `exec`). It is
 * suspended inside proc_exec for the whole child run (it only resumes via the
 * proc_exit longjmp), so it must be BLOCKED while the child runs — otherwise a
 * timer tick re-enqueues/re-selects its slot and reverts current_proc +
 * sched_current_idx away from the child, making the child's arch_prctl write
 * the TLS base into the parent's process_t. NULL on the fork+exec path. */
static process_t *exec_parent_proc;


/* ── Run queue implementation ────────────────────────────────── */

static void runq_enqueue(int idx)
{
    uint8_t q = proctab[idx].qos_class;
    proctab[idx].runq_next = -1;
    if (runq_tail[q] >= 0)
        proctab[runq_tail[q]].runq_next = (int16_t)idx;
    else
        runq_head[q] = (int16_t)idx;
    runq_tail[q] = (int16_t)idx;
    ready_bitmap |= (1ULL << idx);
}

static void runq_dequeue(int idx)
{
    uint8_t q = proctab[idx].qos_class;
    ready_bitmap &= ~(1ULL << idx);

    if (runq_head[q] == idx) {
        runq_head[q] = proctab[idx].runq_next;
        if (runq_head[q] < 0)
            runq_tail[q] = -1;
    } else {
        int16_t prev = runq_head[q];
        while (prev >= 0 && proctab[prev].runq_next != idx)
            prev = proctab[prev].runq_next;
        if (prev >= 0) {
            proctab[prev].runq_next = proctab[idx].runq_next;
            if (runq_tail[q] == idx)
                runq_tail[q] = prev;
        }
    }
    proctab[idx].runq_next = -1;
}

static inline void proc_transition(process_t *p, uint32_t new_state)
{
    int idx = (int)(p - proctab);
    uint32_t old = p->state;
    p->state = new_state;

    if (old == PROC_READY && new_state != PROC_READY)
        runq_dequeue(idx);
    else if (old != PROC_READY && new_state == PROC_READY)
        runq_enqueue(idx);

    /* Stamp the death time when a process first enters ZOMBIE state.
     * The auto-reaper uses this to apply a grace period before
     * reclaiming the slot, so a parent that calls wait4() within the
     * grace window can still observe its child's exit_code. Reset on
     * a transition back to PROC_FREE (slot reuse). */
    if (new_state == PROC_ZOMBIE && old != PROC_ZOMBIE)
        p->zombie_tick = idt_get_ticks();
    else if (new_state == PROC_FREE)
        p->zombie_tick = 0;
}

static void runq_init(void)
{
    ready_bitmap = 0;
    for (int q = 0; q < QOS_NUM_CLASSES; q++) {
        runq_head[q] = -1;
        runq_tail[q] = -1;
    }
    for (int i = 0; i < MAX_PID; i++)
        pid_to_idx[i] = -1;
}

/* Console I/O (shared with syscall.c) */
/* ── Allocate a process slot ─────────────────────────────────── */

static process_t *proc_alloc(const char *name)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) {
            process_t *p = &proctab[i];
            memset(p, 0, sizeof(*p));
            fpu_state_init(p->fpu_state);
            p->pid = next_pid++;
            p->ppid = current_proc ? current_proc->pid : 0;
            /* State stays PROC_FREE (from memset). Caller must call
             * proc_transition() to PROC_READY/RUNNING after setup. */
            p->runq_next = -1;
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

            /* Allocate per-process fd_table (refcounted) */
            p->fd_table = kmalloc(sizeof(fd_table_t));
            if (p->fd_table) {
                memset(p->fd_table, 0, sizeof(fd_table_t));
                p->fd_table->refcount = 1;
            }

            /* O(1) PID lookup registration */
            if (p->pid < MAX_PID)
                pid_to_idx[p->pid] = (int16_t)i;

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
    extern void vg3d_cleanup_process(uint32_t pid);
    compositor_cleanup_process(p->pid);
    shm_cleanup_process(p->pid);
    /* Release any GPU 3D contexts + resources owned by this process so
     * backing pages aren't leaked. Must run while p->pid is still valid. */
    vg3d_cleanup_process(p->pid);

    extern void kbd_flush(void);
    kbd_flush();

    /* Reset per-process syscall state (file FDs, brk heap) */
    syscall_reset_process();

    /* FDs live in the global fd_table[] in syscall.c — closed via
     * syscall_reset_process() which is called from compositor_cleanup_process. */

    /* X-PGTBL: release the per-process page tables if this process owns
     * one. Must run *after* syscall_reset_process because VMA cleanup
     * needs to walk the per-process PML4 to free faulted pages. */
    if (p->owns_cr3 && p->cr3 && p->cr3 != paging_get_kernel_cr3()) {
        extern void paging_free_process_cr3(uint64_t cr3);
        paging_free_process_cr3(p->cr3);
        p->cr3 = 0;
    }
    p->owns_cr3 = false;

    /* Release the user-symbol-table copies captured at elf_load time. */
    if (p->user_symtab) { kfree(p->user_symtab); p->user_symtab = NULL; }
    if (p->user_strtab) { kfree(p->user_strtab); p->user_strtab = NULL; }
    p->user_symtab_size = 0;
    p->user_strtab_size = 0;
    p->user_load_bias = 0;

    /* Release fd_table (refcounted — shared between threads) */
    if (p->fd_table) {
        p->fd_table->refcount--;
        if (p->fd_table->refcount <= 0)
            kfree(p->fd_table);
        p->fd_table = NULL;
    }

    /* Clear PID lookup before freeing slot */
    if (p->pid < MAX_PID)
        pid_to_idx[p->pid] = -1;
    proc_transition(p, PROC_FREE);
}

/* External kill-and-reap — mark a process as ZOMBIE, free all its
 * resources (page tables, fd table, symbol tables, GPU contexts,
 * SHM regions, ...), then put the proctab slot back to PROC_FREE so
 * it can be reused. Used by winexec to clean up orphan win32 threads
 * before re-enabling the APIC LVT: leaving them as ZOMBIE merely
 * stops the scheduler from dispatching them, but keeps their slot
 * occupied and their context structures allocated. proc_free() does
 * the real work; we just need to make sure we aren't reaping the
 * currently-running process (would self-corrupt).
 *
 * Returns 0 on successful reap, -1 if the pid is invalid, the slot
 * is already free, or the process is the caller itself. */
int proc_kill_pid(int pid)
{
    if (pid <= 0 || pid >= MAX_PID) return -1;
    int idx = pid_to_idx[pid];
    if (idx < 0 || idx >= MAX_PROCESSES) return -1;
    process_t *p = &proctab[idx];
    if (p->state == PROC_FREE) return -1;
    /* Refuse to reap ourselves — proc_free dismantles state that the
     * current execution context still depends on. proc_exit() is the
     * right API for self-termination. */
    if (p == current_proc) return -1;

    /* If the process is ready but never dispatched (compositor's
     * orphan win32 threads after PE exit fall into this bucket), we
     * can free it immediately. proc_free handles dequeueing from the
     * run queue, releasing per-process resources, and transitioning
     * to PROC_FREE. */
    p->exit_code = -1;
    proc_transition(p, PROC_ZOMBIE);
    proc_free(p);
    return 0;
}

/* Back-compat alias — kept so existing callers (winexec, debug
 * tooling) continue to compile. Prefer proc_kill_pid going forward. */
int proc_zombify_pid(int pid) { return proc_kill_pid(pid); }

/* ── Register memory region with current process (for cleanup) ── */

void proc_add_region(void *base, uint64_t pages)
{
    process_t *p = exec_target_proc ? exec_target_proc : current_proc;
    if (!p) return;
    if (p->region_count >= MAX_REGIONS) return;
    p->regions[p->region_count].base = base;
    p->regions[p->region_count].pages = pages;
    p->regions[p->region_count].virt_base = 0;
    p->region_count++;
}

/* Virt-aware variant: records a separate VA base for ET_EXEC segments
 * that were mapped via paging_map_page. execve uses virt_base to unmap
 * stale PTEs before loading a new ELF, otherwise VAs that the old ELF
 * touched but the new ELF doesn't map would still resolve to recycled
 * physical pages with stale data. */
void proc_add_region_virt(void *base, uint64_t pages, uint64_t virt_base)
{
    process_t *p = exec_target_proc ? exec_target_proc : current_proc;
    if (!p) return;
    if (p->region_count >= MAX_REGIONS) return;
    p->regions[p->region_count].base = base;
    p->regions[p->region_count].pages = pages;
    p->regions[p->region_count].virt_base = virt_base;
    p->region_count++;
}

void proc_set_spec_info(void *info)
{
    process_t *p = exec_target_proc ? exec_target_proc : current_proc;
    if (p) p->spec_info = info;
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
    return current_proc->fd_table->entries;
}

/* X-PGTBL: CR3 of the currently-running process (0 if none). */
uint64_t proc_current_cr3(void)
{
    return current_proc ? current_proc->cr3 : 0;
}

/* Called by elf_jump right before entering a freshly-exec'd binary.
 * proc_exec sets current_proc = the new process, but the ELF load that
 * follows runs with interrupts ON, so a timer tick can context-switch
 * current_proc to a kernel thread (cr3 == kernel_cr3) and never restore
 * it before elf_jump. Then proc_current_cr3() returns the stale kernel
 * CR3, elf_jump skips the address-space switch, and the binary runs
 * un-isolated under kernel_cr3 — where its load VA (0x20000000) COLLIDES
 * with the kernel's low identity map, corrupting its own code pages
 * (timing-dependent). Re-anchor current_proc to exec_target_proc (the
 * process actually being launched, already used to anchor region
 * registration) and return ITS cr3, so the launch + every subsequent
 * demand fault use the correct isolated address space. */
uint64_t proc_launch_prepare(void)
{
    if (exec_target_proc) {
        extern void sched_current_set_proc(void *pp);
        set_current_proc(exec_target_proc);
        /* Make the scheduler track this process so a later preemption
         * saves/restores ITS context + CR3 (not the kernel/shell slot). */
        sched_current_set_proc(exec_target_proc);
        /* The synchronous-exec parent's kernel context is abandoned at the
         * elf_jump that immediately follows (control enters the child and
         * only returns via the proc_exit longjmp). BLOCK the parent so the
         * scheduler can't re-enqueue/re-select its slot and revert
         * current_proc + sched_current_idx back to it mid-run — that desync
         * is what made the child's arch_prctl(ARCH_SET_FS) land in the
         * parent's process_t (child fs_base=0 → fs:[0] #PF). proc_transition
         * to BLOCKED also dequeues it from the runq if a tick during the ELF
         * load already enqueued it READY. Unblocked at proc_exec's setjmp
         * return. exec_parent_proc is NULL on the fork+exec path. */
        bool blocked_parent = false;
        if (exec_parent_proc && exec_parent_proc != exec_target_proc &&
            exec_parent_proc->state != PROC_ZOMBIE &&
            exec_parent_proc->state != PROC_FREE) {
            proc_transition(exec_parent_proc, PROC_BLOCKED);
            blocked_parent = true;
        }
        serial_puts("[LP] anchor child pid=");
        serial_putdec(exec_target_proc->pid);
        serial_puts(":");
        serial_putdec((uint64_t)(int)(exec_target_proc - proctab));
        serial_puts(" parent=");
        serial_putdec(exec_parent_proc ? exec_parent_proc->pid : 0);
        serial_puts(blocked_parent ? " BLOCKED" : " (not blocked)");
        serial_puts("\n");
        return exec_target_proc->cr3;
    }
    return current_proc ? current_proc->cr3 : 0;
}

/* The process an in-progress exec is launching (NULL when not in exec).
 * VMA/region registration must anchor ownership HERE, not to
 * proc_current(): a timer context-switch during the (interrupt-enabled)
 * ELF load can transiently move current_proc to a kernel thread, so a
 * VMA registered then would be owned by the wrong process and fail the
 * owner filter in demand_page_fault once the binary is correctly running
 * as exec_target. See proc_launch_prepare. */
void *proc_exec_target(void)
{
    return exec_target_proc;
}

/* CR3 of the process an in-progress exec is launching. The eager-ELF-load
 * mapping (elf_load_segments) must install the binary's pages into THIS
 * PML4, not proc_current_cr3(): the load runs with interrupts enabled, so a
 * timer context-switch can transiently move current_proc to the launchpad
 * parent or a kernel thread — mapping there leaves the child (the process
 * that actually elf_jumps to the entry) with an unmapped entry page → #PF on
 * the first instruction fetch. Returns 0 when not in an exec (caller falls
 * back to proc_current_cr3). See proc_launch_prepare / proc_exec_target. */
uint64_t proc_exec_target_cr3(void)
{
    return exec_target_proc ? exec_target_proc->cr3 : 0;
}

/* User-symbol-table accessors — used by usym.c so it doesn't have to
 * know the layout of process_t. Take/return void* so usym.c stays
 * decoupled from this struct's anonymous tag. */
void     *user_symtab_get(void *pp)      { process_t *p = pp; return p ? p->user_symtab      : NULL; }
uint64_t  user_symtab_size_get(void *pp) { process_t *p = pp; return p ? p->user_symtab_size : 0; }
char     *user_strtab_get(void *pp)      { process_t *p = pp; return p ? p->user_strtab      : NULL; }
uint64_t  user_strtab_size_get(void *pp) { process_t *p = pp; return p ? p->user_strtab_size : 0; }
uint64_t  user_load_bias_get(void *pp)   { process_t *p = pp; return p ? p->user_load_bias   : 0; }

/* Setter used by elf.c after capturing .symtab/.strtab from the loaded
 * binary. Takes ownership of the kmalloc'd buffers — proc_free will
 * kfree them later. */
void user_symtab_set(void *pp,
                     void *symtab, uint64_t symtab_size,
                     char *strtab, uint64_t strtab_size,
                     uint64_t load_bias)
{
    process_t *p = pp;
    if (!p) return;
    if (p->user_symtab) kfree(p->user_symtab);
    if (p->user_strtab) kfree(p->user_strtab);
    p->user_symtab      = symtab;
    p->user_symtab_size = symtab_size;
    p->user_strtab      = strtab;
    p->user_strtab_size = strtab_size;
    p->user_load_bias   = load_bias;
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

/* Thread group ID of an arbitrary process_t* (0 if NULL). Used by
 * vma_owned_by_current so a CLONE_THREAD thread can fault-in VMAs its
 * thread-group siblings (incl. the main thread) registered — they share
 * the address space. */
int32_t proc_tgid_of(void *p)
{
    return p ? (int32_t)((process_t *)p)->tgid : 0;
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
    if (current_proc) {
        current_proc->fs_base = addr;
        /* DIAG: which process_t actually receives the TLS base, and whether
         * current_proc agrees with sched_current_idx. If current_proc's slot
         * != sched_idx (or slot_pid != current_proc pid), the two "current"
         * notions have diverged and FS will be written/read on different
         * process_t entries. */
        int cur_slot = (int)(current_proc - proctab);
        int si = sched_current_idx;
        serial_puts("[FSSET] current_proc pid=");
        serial_putdec(current_proc->pid);
        serial_puts(" slot=");
        serial_putdec((uint64_t)cur_slot);
        serial_puts(" | sched_idx=");
        serial_putdec((uint64_t)(uint32_t)si);
        serial_puts(" slot_pid=");
        serial_putdec((si >= 0 && si < MAX_PROCESSES) ? proctab[si].pid : 0);
        serial_puts(" fs_base=0x");
        serial_puthex(addr, 16);
        serial_puts("\n");
    }
}

/* Record the highest VA of the process's user stack so fork() can copy the
 * whole used extent. Called by the ELF loader after the stack is allocated.
 * Targets the exec target (mid-exec) or the current process. */
void proc_set_user_stack_top(uint64_t top)
{
    extern void *proc_exec_target(void);
    process_t *p = (process_t *)proc_exec_target();
    if (!p) p = current_proc;
    if (p) p->user_stack_top = top;
    serial_puts("[USTKTOP] set pid=");
    serial_putdec(p ? p->pid : 0);
    serial_puts(" top=0x"); serial_puthex(top, 16);
    serial_puts("\n");
}

/* Current process's stored TLS base (process_t.fs_base). */
uint64_t proc_get_fs_base(void)
{
    return current_proc ? current_proc->fs_base : 0;
}

/* TLS contract hardening (x86-64 Linux ABI):
 * Re-assert the live MSR_FS_BASE from the calling process's stored
 * fs_base on every syscall return. A long-running syscall (e.g. the
 * inference path, which sched_yield()s to other tasks mid-call) can
 * return to userland on a path where the live FS base no longer matches
 * the process's TLS pointer; musl then dereferences fs:[0] (the TCB
 * self-pointer used by errno / __pthread_self) and faults with CR2=0.
 * The process_t copy is the source of truth — it is set by arch_prctl
 * and preserved across context switches — so reloading it here makes the
 * thread pointer survive unconditionally. Skipped for tasks that never
 * set up TLS (fs_base==0, e.g. kernel threads) so we never clobber a
 * legitimately-zero base. Called from syscall_entry.S after dispatch. */
void proc_reassert_fs_base(void)
{
    if (!current_proc) return;
    uint64_t want = current_proc->fs_base;
    if (!want) return;              /* never force a 0 base (kernel threads) */
    uint64_t live = rdmsr(MSR_FS_BASE);
    if (live != want) {
        /* A syscall left the live FS base diverged from the process's
         * stored TLS base. Log it (rare → no spam) and correct it so
         * userland resumes with the right thread pointer. This both
         * diagnoses the clobber and cures the fs:[0] fault. */
        serial_puts("[FSRA] pid=");
        serial_putdec(current_proc->pid);
        serial_puts(" live=0x");
        serial_puthex(live, 16);
        serial_puts(" -> stored=0x");
        serial_puthex(want, 16);
        serial_puts("\n");
        wrmsr(MSR_FS_BASE, want);
    }
}

/* Diagnostic: dump the live MSR_FS_BASE vs the process's stored fs_base.
 * Lets the #PF handler tell apart "live MSR was clobbered" (stored base
 * still correct) from "stored base was zeroed" (deeper bug). */
void proc_dump_fs_state(void)
{
    uint64_t live = rdmsr(MSR_FS_BASE);
    serial_puts("  [FS] live MSR_FS_BASE=0x");
    serial_puthex(live, 16);
    serial_puts(" stored fs_base=0x");
    serial_puthex(current_proc ? current_proc->fs_base : 0, 16);
    if (current_proc) {
        serial_puts(" pid=");
        serial_putdec(current_proc->pid);
        serial_puts(" tgid=");
        serial_putdec(current_proc->tgid);
        serial_puts(current_proc->is_thread ? " thread" : " main");
    }
    serial_puts("\n");
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
    /* O(1) PID lookup via index table */
    if (pid < MAX_PID) {
        int16_t idx = pid_to_idx[pid];
        if (idx >= 0 && idx < MAX_PROCESSES &&
            proctab[idx].pid == pid && proctab[idx].state != PROC_FREE)
            return &proctab[idx];
    }
    /* Fallback: linear scan (for PIDs beyond MAX_PID) */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].pid == pid && proctab[i].state != PROC_FREE)
            return &proctab[i];
    }
    return NULL;
}

/* Opaque accessors for pred_sched — avoids leaking process_t layout. */
void *proc_find_ptr(uint16_t pid) { return proc_find((uint32_t)pid); }
uint64_t proc_kernel_rsp(void *p) { return p ? ((process_t *)p)->kernel_rsp : 0; }
void *proc_fpu_state_ptr(void *p) { return p ? ((process_t *)p)->fpu_state : 0; }

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
    { extern void vfork_release(void *); vfork_release(p); }  /* wake vfork parent if any */

    /* Forked/spawned process — mark ZOMBIE, scheduler will switch away */
    if (p->kernel_stack) {
        thread_exit_cleanup(p);
        p->exit_code = code;
        proc_transition(p, PROC_ZOMBIE);
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
    { extern void vfork_release(void *); vfork_release(p); }  /* wake vfork parent if any */

    /* If this process has a kernel_stack, it was created by fork/sched_spawn.
     * Mark as ZOMBIE and let the scheduler switch away. Parent reaps via wait4. */
    if (p && p->kernel_stack) {
        thread_exit_cleanup(p);  /* X-THREAD: clear_child_tid + futex wake */
        p->exit_code = code;
        proc_transition(p, PROC_ZOMBIE);
        /* DON'T free memory regions here — we're still running on the
         * user stack (SYSCALL doesn't switch stacks in ring-0 OS).
         * proc_wait4 handles all cleanup after the process is reaped. */

        /* Wake parent if it's blocked in wait4 */
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proctab[i].pid == p->ppid && proctab[i].state == PROC_BLOCKED) {
                proc_transition(&proctab[i], PROC_READY);
                break;
            }
        }

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
            *tss_ist1_ptr = (uint64_t)(ist1_stack + 262144);  /* IST1_STACK_SIZE */
    }

    kern_longjmp(exec_jmpbuf, 1);
}

/* proc_exit_group — terminate the entire thread group, then exit the caller.
 *
 * POSIX exit() / return-from-main emits SYS_exit_group: every pthread sharing
 * this tgid must die, not just the calling thread. We mark each sibling ZOMBIE
 * so the scheduler can never resume it on the (about-to-be-freed) shared
 * address space, and run its TID/futex cleanup so any pthread_join waiter is
 * released. We deliberately do NOT proc_free() siblings here: proc_free() runs
 * current-process-coupled teardown (syscall_reset_process closes the live FD
 * table, frees brk, resets the terminal), which would corrupt the still-running
 * caller. The zombie reaper kthread reclaims the sibling slots afterward.
 *
 * The thread-group leader (which owns the CR3) is left ZOMBIE for the parent /
 * shell to reap via wait4 → proc_free → paging_free_process_cr3. By then every
 * sibling is already ZOMBIE, so no thread runs on the freed page tables. */
void proc_exit_group(int32_t code)
{
    process_t *self = current_proc;
    if (self) {
        uint32_t tg = self->tgid;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *p = &proctab[i];
            if (p == self) continue;
            if (p->tgid != tg) continue;
            if (p->state == PROC_FREE || p->state == PROC_ZOMBIE) continue;
            thread_exit_cleanup(p);   /* clear_child_tid + futex wake */
            p->exit_code = code;
            proc_transition(p, PROC_ZOMBIE);
        }
    }
    proc_exit(code);   /* never returns */
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
    (void)paging_switch;  /* still referenced below for setjmp restore */
    uint64_t new_cr3 = paging_create_process_cr3();
    if (new_cr3) {
        p->cr3 = new_cr3;
        p->owns_cr3 = true;       /* proc_exec allocated it → free on exit */
        /* Don't activate the new CR3 yet — the kernel is still running
         * on the boot kernel stack at low phys, which the user PML4
         * deliberately does NOT map. elf_jump() switches CR3 right
         * before jumping into the user binary. */

        /* Map VDSO data + code pages into this process's address space */
        extern int vdso_map_process(uint64_t cr3);
        extern int vdso_thunks_map_process(uint64_t cr3);
        vdso_map_process(new_cr3);
        vdso_thunks_map_process(new_cr3);
    }

    /* Set as current process and pin region registration target */
    process_t *prev = current_proc;
    set_current_proc(p);
    exec_target_proc = p;
    /* Pin the parent so proc_launch_prepare can BLOCK it for the child's
     * lifetime (see exec_parent_proc). Cleared on the child's exit below. */
    exec_parent_proc = prev;
    serial_puts("[PE] proc_exec parent(prev)=");
    serial_putdec(prev ? prev->pid : 0);
    serial_puts(":");
    serial_putdec(prev ? (uint64_t)(int)(prev - proctab) : 0);
    serial_puts(" child=");
    serial_putdec(p->pid);
    serial_puts(":");
    serial_putdec((uint64_t)(int)(p - proctab));
    serial_puts(" sched_idx=");
    serial_putdec((uint64_t)(uint32_t)sched_current_idx);
    serial_puts("\n");
    proc_transition(p, PROC_RUNNING);

    /* Seed stdio — proc_alloc zeros the fd table, so the new process
     * has no fds until we give it 0/1/2 = console. */
    extern void syscall_seed_stdio(fd_entry_t *fds);
    syscall_seed_stdio(p->fd_table->entries);

    fb_puts_color(" [PID ", 0x0000AAFF);
    fb_putdec(p->pid);
    fb_puts("] ");
    fb_puts(filename);
    fb_puts("\n");

    /* Remember the scheduler's current slot so we can restore it when the
     * exec'd process exits (proc_launch_prepare points it at the new
     * process; on exit we must hand it back, since p's slot gets freed).
     * volatile so it survives the proc_exit longjmp below. */
    extern int  sched_current_get(void);
    extern void sched_current_set_idx(int idx);
    volatile int prev_sched_idx = sched_current_get();

    /* Save kernel context so proc_exit() can longjmp back here */
    if (kern_setjmp(exec_jmpbuf) != 0) {
        /* Returned from proc_exit via longjmp. Re-anchor the parent as the
         * running process and UNBLOCK it (blocked at launch) BEFORE
         * re-enabling interrupts, so a timer tick can't catch the scheduler
         * still pointing at the now-dead child. SYSCALL disabled interrupts
         * (FMASK clears IF) and the longjmp bypassed SYSRET, so IF is still 0
         * here — keep it 0 until current_proc/sched_idx/state are consistent. */
        sched_current_set_idx(prev_sched_idx);
        set_current_proc(prev);
        if (exec_parent_proc) {
            if (exec_parent_proc->state == PROC_BLOCKED)
                proc_transition(exec_parent_proc, PROC_RUNNING);
            exec_parent_proc = NULL;
        }
        exec_target_proc = NULL;
        int code = last_exit_code;
        __asm__ volatile ("sti");
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

    sched_current_set_idx(prev_sched_idx);
    exec_target_proc = NULL;
    set_current_proc(prev);
    /* exec failed before elf_jump, so the parent was never blocked; clear the
     * pin (and unblock defensively in case a path did block it). */
    if (exec_parent_proc) {
        if (exec_parent_proc->state == PROC_BLOCKED)
            proc_transition(exec_parent_proc, PROC_RUNNING);
        exec_parent_proc = NULL;
    }
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
extern volatile uint64_t sched_switch_cr3;

static int      sched_current_idx = -1;

/* Scheduler current-slot accessors used by proc_launch_prepare / proc_exec
 * to make a freshly-exec'd binary the scheduler's tracked process. Without
 * this, proc_exec launches the binary outside the scheduler (sched_current_idx
 * still on the kernel/shell slot), so the FIRST timer preemption saves the
 * binary's context into the wrong slot and RESUMES it under the wrong (kernel)
 * CR3 — colliding the binary's load VA (0x20000000) with the kernel low
 * identity map and corrupting its own code. The fork path already does this
 * fixup (sched_current_idx = parent_idx); this generalizes it to plain exec. */
int  sched_current_get(void) { return sched_current_idx; }
void sched_current_set_idx(int idx) { sched_current_idx = idx; }
void sched_current_set_proc(void *pp)
{
    if (!pp) return;
    process_t *p = (process_t *)pp;
    sched_current_idx = (int)(p - &proctab[0]);
    p->quantum = qos_quantum[p->qos_class];
}
static bool     sched_enabled = false;
static uint64_t sched_switches = 0;

/* Thread exit trampoline — if a kernel thread's entry function returns,
 * execution lands here (the return address was placed below the fake frame). */
static void __attribute__((noreturn)) sched_thread_exit(void)
{
    if (sched_current_idx >= 0)
        proctab[sched_current_idx].state = PROC_ZOMBIE;
    /* Yield repeatedly via software int $0x20 instead of `hlt`. With
     * the APIC timer masked (compat32 sessions do this), a hlt would
     * never wake; the scheduler would never run again and any
     * waiter (e.g. UT99 main on WaitForSingleObject) would deadlock.
     * Software-INT into the timer ISR runs sched_tick synchronously
     * which switches us off this ZOMBIE process to whoever is READY. */
    for (;;) __asm__ volatile ("int $0x20" ::: "memory");
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

void __hot sched_tick(void *frame_ptr)
{
    if (!sched_enabled || sched_current_idx < 0)
        return;

    /* Only BSP (LAPIC ID 0) runs the scheduler — APs have their own
     * timer interrupts but must not touch single-CPU scheduler state */
    if (sched_get_lapic_id() != 0)
        return;

    /* Expire any timed futex waiters (pthread_cond_timedwait, sem_timedwait).
     * No-op unless at least one timed waiter is parked. */
    extern void futex_timeout_sweep(void);
    futex_timeout_sweep();

    process_t *cur = &proctab[sched_current_idx];

    /* Drain syscall-free command ring for current process (if registered).
     * fd_table macro resolves via current_proc which is correct here. */
    {
        extern void cmdring_drain(int proc_idx);
        cmdring_drain((int)cur->pid);
    }

    /* Check if a higher-priority process is READY (preemption).
     * ZOMBIE/BLOCKED processes always force-switch immediately.
     * Otherwise, only switch if quantum expired or preempted. */
    bool force_switch = (cur->state == PROC_ZOMBIE || cur->state == PROC_BLOCKED);
    bool quantum_expired = false;

    if (!force_switch) {
        extern bool idt_tsc_deadline_active(void);
        if (idt_tsc_deadline_active()) {
            /* In TSC-deadline mode, every timer interrupt IS a quantum expiry —
             * the deadline was programmed for exactly one quantum duration. */
            quantum_expired = true;
        } else if (cur->quantum > 1) {
            cur->quantum--;
        } else {
            quantum_expired = true;
        }
    }

    /* O(1) run queue: pick head of highest non-empty QoS queue */
    int next_idx = -1;
    if (ready_bitmap) {
        for (int q = QOS_REALTIME; q >= QOS_IDLE; q--) {
            if (runq_head[q] >= 0) {
                next_idx = runq_head[q];
                break;
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
     *  - quantum_expired: switch to next candidate
     *  - preemption: higher-priority READY process preempts current */
    uint8_t best_qos = proctab[next_idx].qos_class;
    if (!force_switch && !quantum_expired) {
        /* Still have quantum — only preempt if candidate is strictly higher priority */
        if (best_qos <= cur->qos_class) {
            /* No switch — speculatively prefetch code ahead on an idle AP */
            extern void spec_prefetch_ahead(uint64_t rip, uint64_t cr3);
            uint64_t *f = (uint64_t *)frame_ptr;
            spec_prefetch_ahead(f[17] /* RIP */, cur->cr3);
            return;
        }
        /* Preemption: higher priority process is waiting */
    }

    /* ── Context switch ────────────────────────────────────────── */

    /* Save current process: frame_ptr is RSP pointing to the saved
     * GPRs on this process's stack (set by ISR stub before calling
     * isr_handler). Store it so we can restore later. */
    cur->kernel_rsp = (uint64_t)frame_ptr;

    /* Save-time canary: verify frame is valid NOW and plant a canary
     * below it so we can detect post-save corruption at restore time. */
    {
        uint64_t *f = (uint64_t *)frame_ptr;
        uint64_t cs = f[18];
        if (cs != 0x38 && cs != 0x28 && cs != 0x43 && cs != 0x40) {
            serial_puts("[SCHED] BAD SAVE PID ");
            serial_putdec(cur->pid);
            serial_puts(": CS=0x"); serial_puthex(cs, 8);
            serial_puts(" RIP=0x"); serial_puthex(f[17], 16);
            serial_puts(" vec="); serial_putdec(f[15]);
            serial_puts("\n");
        }
        /* Save CS+RIP in process_t so we can detect frame corruption
         * at restore time (process_t is in BSS, not on the stack). */
        cur->saved_frame_cs  = f[18];
        cur->saved_frame_rip = f[17];
    }

    {
        uint64_t live_fs = rdmsr(MSR_FS_BASE);  /* save per-thread TLS */
        /* DIAG: smoking gun for "save clobbers a good TLS base to 0".
         * If this process had a valid TLS base but the live MSR reads 0
         * at save time, the base was lost while it was current (another
         * task's restore wrote 0 and we are about to persist that). */
        if (cur->fs_base && !live_fs) {
            serial_puts("[FSSAVE-ZERO] pid=");
            serial_putdec(cur->pid);
            serial_puts(" had fs_base=0x");
            serial_puthex(cur->fs_base, 16);
            serial_puts(" but live MSR=0 -> persisting 0\n");
        }
        cur->fs_base = live_fs;
    }
    /* Only mark as READY if currently RUNNING.
     * ZOMBIE processes must stay ZOMBIE — proc_wait4 relies on this. */
    if (cur->state == PROC_RUNNING)
        proc_transition(cur, PROC_READY);

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
            extern int smp_submit_any(void (*)(void*, void*), void*, void*);
            extern int ap_worker_count;

            /* Pool of arg blocks — fixes single-static-arg race where
             * two eligible processes in the same tick clobber each other's
             * args before the AP reads them. */
            #define MC_SLOTS 4
            static struct mc_arg_s {
                uint32_t pid; void *base; uint64_t pages;
                volatile int in_use;
            } mc_args[MC_SLOTS];

            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t *p = &proctab[i];
                if (p->state == PROC_BLOCKED && !p->pages_compressed &&
                    p->last_active_tick > 0 &&
                    (now - p->last_active_tick) > memcompress_idle_threshold()) {
                    if (ap_worker_count > 0) {
                        /* Find a free arg slot */
                        int slot = -1;
                        for (int s = 0; s < MC_SLOTS; s++) {
                            if (__sync_lock_test_and_set(&mc_args[s].in_use, 1) == 0) {
                                slot = s;
                                break;
                            }
                        }
                        if (slot < 0) break;  /* all slots busy, retry next cycle */

                        mc_args[slot].pid = p->pid;
                        mc_args[slot].base = (p->region_count > 0) ? p->regions[0].base : 0;
                        mc_args[slot].pages = (p->region_count > 0) ? p->regions[0].pages : 0;
                        extern void memcompress_worker(void *arg, void *result);
                        extern int smp_submit_ff(void (*)(void*, void*), void*, void*);
                        if (smp_submit_ff(memcompress_worker, &mc_args[slot], 0) < 0)
                            __sync_lock_release(&mc_args[slot].in_use);  /* no AP free */
                    }
                    /* No inline fallback — never compress inside the ISR.
                     * Single-CPU systems skip compression entirely (acceptable:
                     * compression is an optimization, not a correctness requirement). */
                    p->pages_compressed = true;
                }
            }
        }
    }

    /* Drive network stack en cada timer tick:
     *   - Si hay process bloqueado en net I/O (DHCP retry, TCP connect, etc.).
     *   - O si la NIC señaló IRQ pending (paquete entrante por drenar).
     * El segundo caso es crítico: después de boot, sin waiters, los
     * frames entrantes (ICMP, ARP requests del peer) llenarían el ring
     * RX hasta que HW empezara a dropear.  net_poll tiene un guard de
     * reentrancia (in_net_poll), así que es seguro llamar siempre.    */
    {
        extern void     net_poll(void);
        extern void     paging_switch(uint64_t cr3);
        extern uint64_t paging_get_kernel_cr3(void);
        /* MSI on I211 has a "pending acknowledge" gate: after the first
         * MSI is delivered, the chip won't fire a new one until SW
         * explicitly reads ICR (or writes 1 to clear).  Our ISR DOES
         * read ICR — but somehow under bursty Mac→OsitoK traffic the
         * chain stalls (observed: irq_pending=1 stuck, RDH advancing,
         * RDT lagging, and ISR count not incrementing for 35 pings).
         *
         * Bypass: drive net_poll unconditionally each tick.  The
         * function has a reentrancy guard and is cheap when the ring
         * is empty (one MMIO descriptor read returning DD=0).
         *
         * CR3: run net_poll under the KERNEL CR3. net_poll's call graph
         * (virtio_net_recv, NIC RX buffers, etc.) dereferences RAW PHYSICAL
         * / low-identity addresses that only exist in the kernel CR3's low
         * map. A user per-process CR3 has an empty PML4[0] (no low identity,
         * see paging_create_process_cr3), so when this fires from the timer
         * while a user process (gcc/cc1) is current, net_poll #PFs (observed:
         * virtio_net_recv+0xa0 touching a low RX buffer under cc1's CR3).
         * net_poll only touches kernel memory, never the interrupted user
         * pages, so kernel_cr3 is safe + complete. Restore before the
         * scheduler loads next->cr3 below. Guarded so kernel-thread ticks
         * (cr3 == kernel_cr3) pay no TLB cost. */
        uint64_t saved_cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(saved_cr3));
        uint64_t kcr3 = paging_get_kernel_cr3();
        /* BISECT: the CR3 switch corrupts the stack out from under us when
         * sched_tick is running on a LOW (user-VA) stack — true for pthread
         * worker/render threads (musl puts their stacks in the low mmap
         * region, e.g. 0x5_1408_xxxx), since that VA maps to a DIFFERENT
         * physical page in kernel_cr3's identity map than in the process CR3.
         * The main thread is immune (upper-half stack, shared in all CR3s).
         * Only do the kernel-CR3 net_poll dance when our own RSP is upper-half
         * (always-mapped); otherwise skip net_poll this tick (it's opportunistic
         * and will run next tick from an upper-half-stack process). */
        uint64_t cur_rsp;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(cur_rsp));
        bool rsp_upper = (cur_rsp >= 0xFFFF800000000000ULL);
        if (saved_cr3 == kcr3) {
            /* Already on kernel CR3 (kernel thread / idle): no switch, the
             * low-identity stack is valid as-is. Always safe. */
            net_poll();
        } else if (rsp_upper) {
            /* User process with an upper-half stack (main thread): the stack is
             * shared in PML4[256] across all CR3s, so the kernel-CR3 switch
             * leaves it valid. */
            paging_switch(kcr3);
            net_poll();
            paging_switch(saved_cr3);
        }
        /* else: user process on a LOW (non-identity) stack — a pthread worker/
         * render thread. Switching to kernel CR3 would remap RSP to a different
         * physical page (identity) and corrupt the live stack → return-to-garbage
         * / RIP=0. Skip net_poll this tick; it runs next tick from the main
         * thread or a kernel thread. */
    }

    /* Load next process */
    process_t *next = &proctab[next_idx];

    /* Decompress pages if needed before running */
    if (next->pages_compressed) {
        extern int memcompress_restore_process(uint32_t);
        memcompress_restore_process(next->pid);
        next->pages_compressed = false;
    }

    proc_transition(next, PROC_RUNNING);
    next->quantum = qos_quantum[next->qos_class];
    next->last_active_tick = idt_get_ticks();
    set_current_proc(next);
    sched_current_idx = next_idx;
    wrmsr(MSR_FS_BASE, next->fs_base);  /* restore per-thread TLS */

    uint64_t active_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(active_cr3));

    /* Sanity check: verify the interrupt frame at kernel_rsp has
     * a valid CS selector and the canary we planted at save time. */
    if (next->kernel_rsp >= 0xFFFF800000000000ULL || next->cr3 == active_cr3) {
        uint64_t *frame = (uint64_t *)next->kernel_rsp;
        uint64_t cs = frame[18];

        uint64_t rip = frame[17];
        uint64_t ss  = frame[21];
        uint64_t rsp_saved = frame[20];
        bool bad_cs  = (cs != 0x38 && cs != 0x28 && cs != 0x43 && cs != 0x40);
        bool bad_ss  = (ss != 0x30 && ss != 0x3B && ss != 0x00);
        /* A low (user-space) RIP is corruption only for a KERNEL thread,
         * whose entry is a kernel function at a high RIP. User-ELF processes
         * legitimately run their own code at low addresses (e.g. 0x20000000)
         * in ring 0 with CS=0x38, and a timer tick can interrupt them there.
         * Gate on next->cr3==0 (kernel threads have no per-process CR3) so
         * this never false-positive-kills a long-running user program (gcc)
         * mid-execution. */
        /* A near-NULL RIP is never a legitimate resume target for ANY
         * process (lowest user-ELF base is 0x20000000). Catch it ungated:
         * a CLONE_THREAD child (cr3 != 0) whose saved frame got RIP=0 must
         * not be iret'd into address 0 (wild #PF at instruction-fetch). */
        bool bad_rip = (rip < 0x1000ULL) ||
                       (rip < 0xFFFF800000000000ULL && cs == 0x38 &&
                        next->cr3 == 0);

        if (bad_cs || bad_ss || bad_rip) {
            serial_puts("[SCHED] CORRUPT PID ");
            serial_putdec(next->pid);
            serial_puts(": CS=0x"); serial_puthex(cs, 4);
            serial_puts(" SS=0x"); serial_puthex(ss, 4);
            serial_puts(" RIP=0x"); serial_puthex(rip, 16);
            serial_puts(" fRSP=0x"); serial_puthex(rsp_saved, 16);
            serial_puts(" | was CS=0x"); serial_puthex(next->saved_frame_cs, 4);
            serial_puts(" RIP=0x"); serial_puthex(next->saved_frame_rip, 16);
            serial_puts("\n");
            proc_transition(next, PROC_ZOMBIE);
            return;
        }
    }

    /* Markov scheduler hooks (Phase 6 of plan):
     *   1. Record the transition so the model learns this pattern.
     *   2. Predict the PID we'll switch to next quantum and prefetch
     *      its kernel_rsp + FPU state so the upcoming switch sees hot
     *      cache lines instead of 20-cycle L2/L3 loads per field. */
    {
        extern void pred_record(uint16_t, uint16_t, uint8_t);
        extern uint16_t pred_next(uint16_t, uint8_t);
        extern void pred_prewarm(uint16_t);
        uint8_t trig = 0 /* PRED_TRIGGER_QUANTUM */;
        pred_record((uint16_t)cur->pid, (uint16_t)next->pid, trig);
        uint16_t pred = pred_next((uint16_t)next->pid, trig);
        if (pred) pred_prewarm(pred);
    }

    /* Tell ISR stub to complete the address-space switch after C returns.
     * The stub uses an upper-half trampoline stack before loading CR3, then
     * moves to next->kernel_rsp and pops the saved interrupt frame. Doing
     * the CR3 load here is unsafe when the timer tick entered on a low
     * pthread stack: the outgoing stack can disappear before paging_switch()
     * returns. */
    sched_switch_cr3 = next->cr3;
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
        proc_transition(p, PROC_FREE);
        serial_puts("[SCHED] Stack allocation failed\n");
        return -1;
    }
    void *stack = PHYS_TO_VIRT(stack_phys);
    p->kernel_stack = stack;

    /* Guard page: unmap the bottom 4KB so stack overflow triggers #PF
     * instead of silently corrupting adjacent memory. */
    extern int paging_unmap_page(uint64_t virt);
    paging_unmap_page((uint64_t)stack);

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
    proc_transition(p, PROC_READY);
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
    /* Invoke the timer ISR via software INT instead of waiting for
     * the next hardware tick. The APIC LVT_TIMER is masked while UT99
     * (and any other compat32 process) is running so a `hlt` here
     * would never wake. `int $0x20` runs isr_stub_32 → isr_handler →
     * sched_tick synchronously, which performs the context switch
     * exactly as a real timer tick would. After iretq we resume on
     * whichever process the scheduler picks next (or back to us if
     * we're still the highest-priority READY process). */
    __asm__ volatile ("int $0x20" ::: "memory");
}

/* ── sched_stats: return context switch count ────────────────── */

uint64_t sched_get_switches(void) { return sched_switches; }
bool sched_is_enabled(void) { return sched_enabled; }

/* TSC-deadline: return microseconds until next preemption.
 * Returns 0 for tickless idle (no READY processes). */
uint64_t sched_get_next_deadline_us(void)
{
    if (!sched_enabled || sched_current_idx < 0)
        return 10000;  /* 10ms default if scheduler not active yet */

    /* Check if any process is READY */
    if (ready_bitmap == 0)
        return 0;  /* tickless idle — no deadline */

    process_t *cur = &proctab[sched_current_idx];
    return qos_quantum_us[cur->qos_class];
}

/* ── Net-blocking helpers ───────────────────────────────────────
 * Used by net.c to block the current process while waiting for
 * network events (ARP replies, TCP handshakes, data arrival).
 * The packet handler (handle_tcp/handle_arp) calls sched_unblock()
 * to wake the process when the expected event occurs.
 */
int sched_block_current(void)
{
    if (!sched_enabled || sched_current_idx < 0) return -1;
    proc_transition(&proctab[sched_current_idx], PROC_BLOCKED);
    __asm__ volatile ("mfence" ::: "memory");
    return sched_current_idx;
}

void sched_unblock(int proc_idx)
{
    if (proc_idx >= 0 && proc_idx < MAX_PROCESSES &&
        proctab[proc_idx].state == PROC_BLOCKED)
        proc_transition(&proctab[proc_idx], PROC_READY);
}

/* Accessor for io_predict — returns pointer to the current process's
 * 32-byte last_opened scratch buffer, or NULL if no current proc. */
char *proc_current_last_opened(void)
{
    if (sched_current_idx < 0 || sched_current_idx >= MAX_PROCESSES) return 0;
    process_t *p = &proctab[sched_current_idx];
    if (p->state == PROC_FREE) return 0;
    return p->last_opened;
}

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
int32_t proc_fork(uint64_t child_stack)
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

    /* vfork-style address-space sharing: the child runs in the PARENT's
     * CR3 until it execve's (which allocates a fresh isolated CR3) or
     * _exit's. Under X-PGTBL each process has its own CR3, so without this
     * the child would inherit proc_alloc's kernel_cr3 default and could not
     * see the parent's code at 0x20000000. The child must NOT free this
     * shared CR3 on exit (owns_cr3=false). The parent is suspended below
     * until the child execs/exits, so the shared space is never used by
     * both at once. */
    child->cr3 = parent->cr3;
    child->owns_cr3 = false;

    /* Inherit the parent's TLS base (thread pointer). POSIX fork duplicates
     * the calling thread, so the child's FS base must equal the parent's —
     * its TLS block lives at the same virtual address in the shared/copied
     * address space. proc_alloc zeroed child->fs_base; without this the
     * scheduler restores fs_base=0 when it first runs the child, and the
     * child's first TLS access (errno / __pthread_self → fs:[0]) faults at
     * CR2=0 while still running the parent's image (before execve installs a
     * new one whose musl will arch_prctl its own base). The child enters
     * userland via the fake IRETQ frame, NOT the syscall return path, so the
     * syscall_dispatch FS-restore wrapper does not cover it — this does. */
    child->fs_base = parent->fs_base;

    /* Fork: allocate a NEW fd_table (separate copy for child).
     * Each inherited pipe fd bumps the corresponding refcounts. */
    child->fd_table = kmalloc(sizeof(fd_table_t));
    if (!child->fd_table) {
        proc_transition(child, PROC_FREE);
        serial_puts("[FORK] fd_table alloc failed\n");
        return -1;
    }
    child->fd_table->refcount = 1;
    memcpy(child->fd_table->entries, parent->fd_table->entries,
           sizeof(child->fd_table->entries));
    for (int i = 0; i < MAX_FDS; i++) {
        if (!child->fd_table->entries[i].open) continue;
        if (child->fd_table->entries[i].type == FD_TYPE_PIPE &&
            child->fd_table->entries[i].pipe) {
            pipe_buf_t *p = (pipe_buf_t *)child->fd_table->entries[i].pipe;
            if ((child->fd_table->entries[i].oflags & 0x3) == 0)
                p->read_refs++;
            else
                p->write_refs++;
        }
    }

    child->region_count = 0;

    /* Allocate kernel stack for the child via the upper-half mirror. */
    void *stack_phys = mem_alloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!stack_phys) {
        proc_transition(child, PROC_FREE);
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

    /* Copy EXACTLY the used portion of the parent's stack [user_rsp, stack_top)
     * — no more (reading past stack_top hits memory above the stack) and no
     * fixed floor (the old 32KB window over-read past the top when the used
     * stack was smaller). stack_top is the parent's recorded user_stack_top. */
#define CHILD_USTACK_FALLBACK_COPY  (32 * 1024)
    uint64_t stk_top = parent->user_stack_top;
    uint64_t copy_size;
    if (stk_top && stk_top > user_rsp)
        copy_size = stk_top - user_rsp;                 /* exact used extent */
    else
        copy_size = (uint64_t)CHILD_USTACK_FALLBACK_COPY; /* top unknown */
    if (copy_size > 8ULL * 1024 * 1024) copy_size = 8ULL * 1024 * 1024;
    /* Round up + one guard page of headroom below the copied frames. */
    uint64_t child_ustack_size = ((copy_size + 0xFFFULL) & ~0xFFFULL) + 0x1000ULL;

    serial_puts("[FORK-COPY] stk_top=0x"); serial_puthex(stk_top, 16);
    serial_puts(" copy="); serial_putdec(copy_size);
    serial_puts(" ustk="); serial_putdec(child_ustack_size); serial_puts("\n");

    void *child_ustack_phys = mem_alloc_aligned(child_ustack_size, 4096);
    if (!child_ustack_phys) {
        mem_free_pages(stack_phys, KERNEL_STACK_SIZE / 4096);
        proc_transition(child, PROC_FREE);
        serial_puts("[FORK] User stack alloc failed\n");
        return -1;
    }
    void *child_ustack = PHYS_TO_VIRT(child_ustack_phys);
    memset(child_ustack, 0, child_ustack_size);

    uint64_t child_ustack_top = (uint64_t)child_ustack + child_ustack_size;
    /* Copy from parent's [user_rsp .. user_rsp + copy_size) to child */
    memcpy((void *)(child_ustack_top - copy_size),
           (void *)user_rsp, copy_size);

    /* Child's RSP = same offset from top as parent's */
    uint64_t child_user_rsp = child_ustack_top - copy_size;

    /* Register child user stack for cleanup on exit. region.base is
     * the phys address handed to mem_free_pages later. */
    if (child->region_count < MAX_REGIONS) {
        child->regions[child->region_count].base = child_ustack_phys;
        child->regions[child->region_count].pages = child_ustack_size / 4096;
        child->region_count++;
    }

    /* IRETQ frame (common fields; RSP depends on the clone variant below). */
    cf[17] = user_rip;      /* RIP = return to userspace after SYSCALL */
    cf[18] = 0x38;          /* CS  = kernel code segment */
    cf[19] = user_rflags | 0x200;  /* RFLAGS with IF=1 */
    cf[21] = 0x30;          /* SS  = kernel data segment */

    if (child_stack) {
        /* clone()/vfork() with an explicit child stack (musl posix_spawn and
         * the __clone wrapper). musl has ALREADY set up the child's function +
         * args on that stack, so the child must run on it verbatim — NOT on a
         * copy of the parent's stack — and the callee-saved registers must keep
         * the parent's raw values (the child re-establishes them from
         * child_stack itself). Ignoring child_stack and substituting a copied
         * stack made the child read garbage (→ #GP). */
        cf[20] = child_stack;
    } else {
        /* raw fork(): the child runs on its private copy of the parent's stack.
         * Relocate RBP, the other callee-saved regs (R12-R15, RBX), and any
         * saved frame pointers from the parent's stack range into the copy so
         * the child doesn't dereference the parent's stack. */
        cf[20] = child_user_rsp;
        if (rbp >= user_rsp && rbp < user_rsp + copy_size)
            cf[8] = child_user_rsp + (rbp - user_rsp);
        {
            int64_t delta = (int64_t)child_user_rsp - (int64_t)user_rsp;
            static const int reloc_idx[] = { 0, 1, 2, 3, 13 };  /* R15 R14 R13 R12 RBX */
            for (unsigned k = 0; k < sizeof(reloc_idx) / sizeof(reloc_idx[0]); k++) {
                uint64_t v = cf[reloc_idx[k]];
                if (v >= user_rsp && v < user_rsp + copy_size)
                    cf[reloc_idx[k]] = (uint64_t)((int64_t)v + delta);
            }
            uint64_t *scan = (uint64_t *)child_user_rsp;
            uint64_t scan_count = copy_size / 8;
            for (uint64_t i = 0; i < scan_count; i++) {
                uint64_t val = scan[i];
                if (val >= user_rsp && val < user_rsp + copy_size)
                    scan[i] = val + delta;
            }
        }
    }

    /* Set up scheduler state — child inherits parent's QoS class */
    child->qos_class = parent->qos_class;
    child->kernel_rsp = child_frame_addr;
    child->quantum = qos_quantum[child->qos_class];
    proc_transition(child, PROC_READY);

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

    /* DIAG [FORKED]: confirm child origin + the callee-saved regs copied into
     * its fake frame (a non-canonical r12 here would explain its #GP). */
    serial_puts("[FORKED] child pid=");
    serial_putdec(child->pid);
    serial_puts(" ppid=");
    serial_putdec(parent->pid);
    serial_puts(" rip=0x"); serial_puthex(user_rip, 16);
    serial_puts(" ursp=0x"); serial_puthex(user_rsp, 16);
    serial_puts(" r12=0x"); serial_puthex(r12, 16);
    serial_puts(" rbp=0x"); serial_puthex(rbp, 16);
    serial_puts(" cstk=0x"); serial_puthex(child_user_rsp, 16);
    serial_puts("\n");

    /* vfork semantics: SUSPEND the parent until the child execve()s or _exit()s.
     * The child has its OWN copied stack, but parent and child still SHARE the
     * address space (heap/globals via the same CR3). Letting both run
     * concurrently races those shared writes — the child read a struct pointer
     * the parent was mutating → garbage r12 → #GP. Blocking the parent here
     * serializes them. The parent's kernel context (this proc_fork frame) lives
     * on the parent's OWN stack, which the child never touches, so the suspend
     * frame is safe. Released by vfork_release() from proc_execve / proc_exit. */
    child->vforked_parent = parent->pid;
    proc_transition(parent, PROC_BLOCKED);
    {
        extern void sched_yield(void);
        sched_yield();   /* switch to the child; resumes here once released */
    }

    /* Parent returns child PID immediately */
    return (int32_t)child->pid;
}

/* Release a vfork-suspended parent when its child execs or exits. */
void vfork_release(void *child_vp)
{
    process_t *child = (process_t *)child_vp;
    if (!child || !child->vforked_parent) return;
    uint32_t ppid = child->vforked_parent;
    child->vforked_parent = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].pid == ppid && proctab[i].state == PROC_BLOCKED) {
            proc_transition(&proctab[i], PROC_READY);
            serial_puts("[VFORK-REL] woke parent pid="); serial_putdec(ppid);
            serial_puts("\n");
            return;
        }
    }
    serial_puts("[VFORK-REL] parent pid="); serial_putdec(ppid);
    serial_puts(" not blocked/found\n");
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

    /* A CLONE_THREAD thread genuinely shares the parent's address space →
     * share its CR3 (the old "identity-mapped OS is automatic" assumption
     * is false under X-PGTBL). Thread must NOT free the shared CR3. */
    thread->cr3 = parent->cr3;
    thread->owns_cr3 = false;

    /* CLONE_FILES: threads SHARE the parent's fd_table (POSIX-correct).
     * No copy — bump refcount. open()/close() in either thread affects both. */
    thread->fd_table = parent->fd_table;
    thread->fd_table->refcount++;

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
        proc_transition(thread, PROC_FREE);
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
    thread->quantum = qos_quantum[thread->qos_class];
    proc_transition(thread, PROC_READY);

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

#define FUTEX_HASH_BITS   5
#define FUTEX_HASH_SIZE   (1 << FUTEX_HASH_BITS)   /* 32 buckets */
#define MAX_FUTEX_WAITERS 256

typedef struct {
    uint64_t    addr;       /* futex user address */
    uint64_t    space;      /* address-space id (CR3) for PRIVATE futexes,
                             * 0 for cross-process SHARED futexes. Two distinct
                             * processes can have a libc global at the *same*
                             * virtual address; keying on the space prevents a
                             * private wake in one from waking a waiter in the
                             * other. Threads share a CR3 → share the space. */
    uint64_t    deadline;   /* idt_get_ticks() value at which a timed wait
                             * expires, or 0 for an infinite wait */
    int         proc_idx;   /* index into proctab (process waiting) */
    bool        active;
    bool        timed_out;  /* set by futex_timeout_sweep when deadline passes */
    int16_t     next;       /* next in same hash bucket, -1 = end */
} futex_waiter_t;

static futex_waiter_t futex_waiters[MAX_FUTEX_WAITERS];
static int16_t futex_buckets[FUTEX_HASH_SIZE];  /* heads, -1 = empty */
static int16_t futex_free_head = -1;            /* free slot list */
static int      futex_timed_count = 0;          /* # of active timed waiters;
                                                 * lets sched_tick skip the
                                                 * deadline sweep when zero */
static spinlock_t futex_lock = SPINLOCK_INIT;

static inline uint64_t futex_lock_irqsave(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(&futex_lock);
    return flags;
}

static inline void futex_unlock_irqrestore(uint64_t flags)
{
    spin_unlock(&futex_lock);
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static inline uint32_t futex_hash(uint64_t addr, uint64_t space)
{
    /* 32-bit Fibonacci hash → top FUTEX_HASH_BITS bits, MASKED to a valid
     * bucket index. BUG (pre-existing): the multiplier was `0x9e370001UL`
     * (unsigned LONG = 64-bit), so the product was 64-bit and `>> 27`
     * yielded bits 27..58 — a value far outside [0, FUTEX_HASH_SIZE) — and
     * `futex_buckets[bucket]` then read wildly out of bounds (#PF, observed
     * CR2 = &futex_buckets + 0xA8763EA2*2 when cc1's malloc-lock contention
     * exercised the futex path). Force a 32-bit multiply (`0x9e370001u`) and
     * mask to the bucket count so the index is always valid. */
    uint32_t h = (uint32_t)((addr >> 2) ^ (space >> 12)) * 0x9e370001u;
    return (h >> (32 - FUTEX_HASH_BITS)) & (FUTEX_HASH_SIZE - 1);
}

static void futex_init(void)
{
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        futex_buckets[i] = -1;
    /* Build free list */
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        futex_waiters[i].active = false;
        futex_waiters[i].next = (int16_t)(i + 1);
    }
    futex_waiters[MAX_FUTEX_WAITERS - 1].next = -1;
    futex_free_head = 0;
}

/* futex_wait — block current process until woken or (optionally) timed out.
 *   space        = address-space id (CR3) for a PRIVATE futex, 0 for SHARED.
 *   timeout_ticks = relative deadline in 100Hz ticks; 0 means wait forever.
 * Returns 0 on wake, -EAGAIN (-11) on value mismatch, -ETIMEDOUT (-110) on
 * timeout, -ENOMEM (-12) if the wait table is full. */
int futex_do_wait(uint64_t uaddr, int expected, uint64_t space,
                  uint64_t timeout_ticks)
{
    volatile int *addr = (volatile int *)uaddr;
    uint64_t flags = futex_lock_irqsave();

    if (*addr != expected) {
        futex_unlock_irqrestore(flags);
        return -11; /* EAGAIN */
    }

    /* Allocate from free list */
    if (futex_free_head < 0) {
        futex_unlock_irqrestore(flags);
        return -12; /* ENOMEM */
    }
    int slot = futex_free_head;
    futex_free_head = futex_waiters[slot].next;

    process_t *cur = current_proc;
    int cur_idx = (int)(cur - &proctab[0]);

    /* Insert into hash bucket */
    uint32_t bucket = futex_hash(uaddr, space);
    futex_waiters[slot].addr = uaddr;
    futex_waiters[slot].space = space;
    futex_waiters[slot].proc_idx = cur_idx;
    futex_waiters[slot].active = true;
    futex_waiters[slot].timed_out = false;
    futex_waiters[slot].deadline =
        timeout_ticks ? (idt_get_ticks() + timeout_ticks) : 0;
    futex_waiters[slot].next = futex_buckets[bucket];
    futex_buckets[bucket] = (int16_t)slot;
    if (futex_waiters[slot].deadline)
        futex_timed_count++;

    /* The value check, bucket insert, and BLOCKED transition must be atomic
     * with futex_wake. Otherwise a wake can unlink the waiter while it is
     * still RUNNING, after which this thread marks itself BLOCKED forever. */
    proc_transition(cur, PROC_BLOCKED);
    futex_unlock_irqrestore(flags);

    /* A BLOCKED task cannot poll its own deadline (the scheduler never
     * resumes it), so timeout enforcement is done by futex_timeout_sweep()
     * from sched_tick, which flips us back to READY + sets timed_out. */
    while (cur->state == PROC_BLOCKED) {
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }

    flags = futex_lock_irqsave();
    int rc = futex_waiters[slot].timed_out ? -110 /* ETIMEDOUT */ : 0;

    /* Woken — remove from the current bucket and return to free list. A
     * FUTEX_REQUEUE may have moved this waiter to another key while it slept,
     * so use the slot's live address/space instead of the original wait key. */
    uint32_t cleanup_bucket =
        futex_hash(futex_waiters[slot].addr, futex_waiters[slot].space);
    futex_waiters[slot].active = false;
    if (futex_waiters[slot].deadline) {
        futex_waiters[slot].deadline = 0;
        if (futex_timed_count > 0) futex_timed_count--;
    }
    /* Unlink from bucket (may already be unlinked by wake) */
    int16_t *pp = &futex_buckets[cleanup_bucket];
    while (*pp >= 0) {
        if (*pp == slot) { *pp = futex_waiters[slot].next; break; }
        pp = &futex_waiters[*pp].next;
    }
    futex_waiters[slot].next = futex_free_head;
    futex_free_head = (int16_t)slot;
    futex_unlock_irqrestore(flags);

    return rc;
}

/* futex_wake — wake up to 'count' processes waiting on (space, uaddr).
 * Returns number of processes woken. Only scans one hash bucket. */
int futex_do_wake(uint64_t uaddr, uint64_t space, int count)
{
    uint64_t flags = futex_lock_irqsave();
    uint32_t bucket = futex_hash(uaddr, space);
    int woken = 0;
    int16_t *pp = &futex_buckets[bucket];

    while (*pp >= 0 && woken < count) {
        int16_t idx = *pp;
        futex_waiter_t *w = &futex_waiters[idx];
        if (w->active && w->addr == uaddr && w->space == space) {
            int pidx = w->proc_idx;
            if (pidx >= 0 && pidx < MAX_PROCESSES &&
                proctab[pidx].state == PROC_BLOCKED) {
                proc_transition(&proctab[pidx], PROC_READY);
                woken++;
            }
            /*
             * Unlink from the bucket, but do not return the waiter slot to
             * the free list here. The sleeping thread still owns `slot` and
             * will read timed_out plus release the slot after it resumes from
             * futex_do_wait(). Returning it here races a new waiter into the
             * same slot and then double-frees it when the old waiter wakes.
             */
            w->active = false;
            if (w->deadline) {
                w->deadline = 0;
                if (futex_timed_count > 0) futex_timed_count--;
            }
            *pp = w->next;
            w->next = -1;
        } else {
            pp = &w->next;
        }
    }
    futex_unlock_irqrestore(flags);
    return woken;
}

/* futex_requeue — wake up to wake_count waiters on (space, uaddr), then move
 * up to requeue_count remaining waiters to (space2, uaddr2). This is the core
 * primitive used by pthread condition variables to hand waiters from the cond
 * variable futex to the associated mutex futex without losing wakeups. */
int futex_do_requeue(uint64_t uaddr, uint64_t space, int wake_count,
                     int requeue_count, uint64_t uaddr2, uint64_t space2)
{
    if (wake_count < 0) wake_count = 0;
    if (requeue_count < 0) requeue_count = 0;

    uint64_t flags = futex_lock_irqsave();
    uint32_t src_bucket = futex_hash(uaddr, space);
    bool same_key = (uaddr == uaddr2 && space == space2);
    bool can_requeue = (uaddr2 != 0 && !same_key);
    int woken = 0;
    int requeued = 0;
    int16_t moved_head = -1;
    int16_t *pp = &futex_buckets[src_bucket];

    while (*pp >= 0 &&
           (woken < wake_count || (can_requeue && requeued < requeue_count))) {
        int16_t idx = *pp;
        futex_waiter_t *w = &futex_waiters[idx];

        if (w->active && w->addr == uaddr && w->space == space) {
            if (woken < wake_count) {
                int pidx = w->proc_idx;
                if (pidx >= 0 && pidx < MAX_PROCESSES &&
                    proctab[pidx].state == PROC_BLOCKED) {
                    proc_transition(&proctab[pidx], PROC_READY);
                    woken++;
                }
                w->active = false;
                if (w->deadline) {
                    w->deadline = 0;
                    if (futex_timed_count > 0) futex_timed_count--;
                }
                *pp = w->next;
                w->next = -1;
            } else if (can_requeue && requeued < requeue_count) {
                *pp = w->next;
                w->next = moved_head;
                moved_head = idx;
                requeued++;
            } else {
                pp = &w->next;
            }
        } else {
            pp = &w->next;
        }
    }

    if (moved_head >= 0) {
        uint32_t dst_bucket = futex_hash(uaddr2, space2);
        while (moved_head >= 0) {
            int16_t idx = moved_head;
            futex_waiter_t *w = &futex_waiters[idx];
            moved_head = w->next;

            w->addr = uaddr2;
            w->space = space2;
            w->next = futex_buckets[dst_bucket];
            futex_buckets[dst_bucket] = idx;
        }
    }

    futex_unlock_irqrestore(flags);
    return woken + requeued;
}

/* futex_timeout_sweep — called from sched_tick on the BSP. Wakes any timed
 * waiter whose deadline has elapsed, marking it timed_out so futex_do_wait
 * returns -ETIMEDOUT. Cheap no-op when no timed waiters exist. The waiter
 * stays linked in its bucket; futex_do_wait unlinks it after resuming. */
void futex_timeout_sweep(void)
{
    if (futex_timed_count <= 0) return;
    uint64_t flags = futex_lock_irqsave();
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        futex_waiter_t *w = &futex_waiters[i];
        if (!w->active || !w->deadline) continue;
        if (now >= w->deadline) {
            int pidx = w->proc_idx;
            if (pidx >= 0 && pidx < MAX_PROCESSES &&
                proctab[pidx].state == PROC_BLOCKED) {
                w->timed_out = true;
                proc_transition(&proctab[pidx], PROC_READY);
            }
        }
    }
    futex_unlock_irqrestore(flags);
}

/* Thread exit cleanup: clear_child_tid + futex wake (X-THREAD) */
static void thread_exit_cleanup(process_t *p)
{
    if (p->clear_child_tid) {
        /* Write 0 to the TID address (signals thread death to parent) */
        *(int *)p->clear_child_tid = 0;
        /* Wake any futex waiter on that address (pthread_join uses this).
         * The joiner waits with a PRIVATE futex in this thread's address
         * space, so key the wake on the same CR3. */
        futex_do_wake((uint64_t)p->clear_child_tid, p->cr3, 1);
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
            proc_transition(&proctab[i], PROC_FREE);
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

    /* Event-driven wait: block this process and let proc_exit() wake us
     * when a child becomes ZOMBIE, instead of polling every tick. */
    proc_transition(current_proc, PROC_BLOCKED);
    __asm__ volatile ("mfence" ::: "memory");

    /* Re-scan immediately: a child may have exited between the initial
     * scan above and our PROC_BLOCKED assignment (close the race window). */
    {
        bool found_early = false;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proctab[i].state != PROC_ZOMBIE) continue;
            if (proctab[i].ppid != my_pid) continue;
            if (pid > 0 && proctab[i].pid != (uint32_t)pid) continue;
            found_early = true;
            break;
        }
        if (!found_early) {
            /* Sleep until proc_exit() sets us back to PROC_READY.
             * Keep the 10000-tick safety timeout (~100s) to avoid permanent hang
             * if a child is killed without going through proc_exit(). */
            for (int tries = 0; tries < 10000; tries++) {
                __asm__ volatile ("sti; hlt; cli" ::: "memory");
                if (current_proc->state == PROC_READY) break;
            }
        }
    }
    proc_transition(current_proc, PROC_READY);

    /* Scan for the zombie child */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state != PROC_ZOMBIE) continue;
        if (proctab[i].ppid != my_pid) continue;
        if (pid > 0 && proctab[i].pid != (uint32_t)pid) continue;

        int32_t child_pid = (int32_t)proctab[i].pid;
        if (wstatus)
            *wstatus = (proctab[i].exit_code & 0xFF) << 8;

        proc_transition(&proctab[i], PROC_FREE);
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

        elf_fork_restore();
        syscall_restore_brk();

        return child_pid;
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

    /* Validate the target exists BEFORE tearing down the caller's image.
     * Below we free the old regions and hand the process a fresh CR3; if the
     * file is then not found (e.g. /bin/sh absent from OsitoFS) the process is
     * left running a half-destroyed image → #UD. Failing here with -ENOENT
     * keeps the image intact so a vfork/posix_spawn child reports the error
     * (errno pipe) and _exit()s cleanly, and the parent's spawn returns it. */
    {
        extern int elf_path_exists(const char *path);
        if (!elf_path_exists(path)) {
            serial_puts("[EXECVE] -ENOENT (image intact): ");
            serial_puts(path); serial_puts("\n");
            return -2;  /* -ENOENT */
        }
    }

    /* Target exists — the exec is committing. Release a vfork parent now: the
     * child is about to replace its image on a fresh CR3 and won't touch the
     * shared address space again. A missing target above returned -ENOENT
     * WITHOUT releasing, so the parent stays suspended until the child _exit()s
     * (proc_exit releases it) — correct vfork semantics. */
    { extern void vfork_release(void *); vfork_release(current_proc); }

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

    /* execve must SNAPSHOT path + argv into KERNEL memory NOW, while the
     * caller's address space is still active and intact. Below we free the
     * old regions, reset brk, and (in the CR3 block) hand the process a
     * fresh PML4 — all of which unmap the user memory where path/argv live.
     * elf_setup_stack later reads argv to build the new stack; without this
     * snapshot it dereferences freed/unmapped pages → #PF (observed: cc1's
     * argv at 0x34B0xxxx/0xA591xxxx). Kernel copies are always mapped. */
    {
        uint64_t plen = strlen(path);
        char *kpath = (char *)kmalloc(plen + 1);
        if (kpath) { memcpy(kpath, path, plen + 1); path = kpath; }
        if (argv) {
            int ac = 0;
            while (argv[ac]) ac++;
            char **ka = (char **)kmalloc((uint64_t)(ac + 1) * sizeof(char *));
            if (ka) {
                for (int i = 0; i < ac; i++) {
                    uint64_t l = strlen(argv[i]);
                    char *s = (char *)kmalloc(l + 1);
                    if (s) memcpy(s, argv[i], l + 1);
                    ka[i] = s;
                }
                ka[ac] = NULL;
                argv = (char *const *)ka;
            }
        }
    }

    /* Free old memory regions ONLY if this process owns them.
     * Forked children share parent's memory (identity-mapped OS),
     * so we must NOT free the parent's regions. Only free if this
     * process has its own ELF regions (from a previous execve). */
    if (!p->kernel_stack) {
        /* Non-forked process (shell exec) — safe to free.
         *
         * FIX: unmap the virtual addresses FIRST before freeing the
         * physical pages. Without this, stale PTEs persist in the PML4
         * when ET_EXEC segments of the previous binary don't overlap
         * the new binary's VA layout. The recycled physical pages get
         * handed to the new process for unrelated allocations, but any
         * access by the new ELF to the old VAs (e.g. via a global the
         * linker placed at the same address but with a different initial
         * value) reads stale data from the recycled page or #PFs on a
         * freed frame. Observed: two ELFs executed in series leaked
         * data via VA 0x200020F0. */
        extern int paging_unmap_page(uint64_t virt);
        for (int i = 0; i < p->region_count; i++) {
            mem_region_t *r = &p->regions[i];
            if (r->virt_base) {
                for (uint64_t pg = 0; pg < r->pages; pg++)
                    paging_unmap_page(r->virt_base + pg * 4096);
            }
            if (r->base && r->pages > 0)
                mem_free_pages(r->base, r->pages);
        }
        /* Global TLB flush — cheaper than per-page invlpg when unmapping
         * hundreds of pages and the VA space is about to be rewritten. */
        __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3"
                          ::: "rax", "memory");
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

    /* execve replaces the address space: give the new image a FRESH
     * isolated CR3 (POSIX). For a fork child this DETACHES it from the
     * parent's shared CR3, so cc1/as/ld run isolated at 0x20000000 without
     * clobbering the parent — and the parent keeps its own CR3 intact.
     * exec_target_proc=p makes elf_jump's proc_launch_prepare re-anchor
     * current_proc + sched_current_idx + switch to this CR3 (the same
     * launch path proc_exec uses → no wrong-CR3-on-resume race). */
    {
        extern uint64_t paging_create_process_cr3(void);
        extern int  vdso_map_process(uint64_t cr3);
        extern int  vdso_thunks_map_process(uint64_t cr3);
        extern void paging_free_process_cr3(uint64_t cr3);
        uint64_t fresh = paging_create_process_cr3();
        if (fresh) {
            vdso_map_process(fresh);
            vdso_thunks_map_process(fresh);
            p->cr3 = fresh;
            p->owns_cr3 = true;
            /* NOTE: do NOT free the OLD cr3 here. The switch to `fresh`
             * happens later in elf_jump; until then the old address space
             * is still ACTIVE and elf_setup_stack reads the argv/envp
             * strings from it (they live in the caller's memory). Freeing
             * its page tables now unmaps those strings → #PF in
             * elf_setup_stack. The old cr3 (if we owned it) leaks a few KB
             * of page-table pages per direct-execve; TODO: free it after
             * the CR3 switch (e.g. via a deferred free in proc_exit/sched).
             * A fork child shares the parent's cr3 (owns_cr3 was false) and
             * must never free it regardless. */
            (void)paging_free_process_cr3;
        }
        exec_target_proc = p;
    }

    /* elf_setup_stack (inside elf_exec, BEFORE elf_jump's CR3 switch) reads
     * the kernel-snapshotted argv and writes the (mirror-backed) user stack.
     * It runs under the CURRENTLY-active CR3 — which here is the calling
     * process's ISOLATED CR3 (empty PML4[0], no low identity map), so the
     * kmalloc'd argv (low/identity kernel heap) is unmapped → #PF. The
     * proc_exec launch path doesn't hit this because it runs elf_setup_stack
     * under the shell's kernel_cr3. Switch to kernel_cr3 now so the load +
     * stack setup see the kernel heap + mirror; elf_jump then switches to
     * the fresh process CR3 before jumping into the binary. The kernel stack
     * we're running on is in the shared upper-half mirror, so this switch is
     * safe. */
    {
        extern void paging_switch(uint64_t cr3);
        paging_switch(paging_get_kernel_cr3());
    }

    /* Execute the ELF — does not return on success.
     * elf_exec loads segments, sets up stack, jumps to entry.
     * When the process exits, proc_exit() handles cleanup. */
    int ret = elf_exec(path, argc, (const char **)argv);

    /* If we get here, exec failed */
    exec_target_proc = NULL;
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

/* ── Auto-reaper: zombies are a smell, not a state ────────────── */
/*
 * Philosophy: a process that has finished running has no business
 * occupying a proctab slot, fd table, page-table allocations, FPU
 * area, symbol tables, etc. POSIX zombies exist purely so the parent
 * can later call wait4() and observe exit_code. If no one is going
 * to wait (kthreads, orphaned win32 threads, processes whose parent
 * has died), the zombie is pure leak.
 *
 * The auto-reaper kthread sweeps proctab every second and reclaims
 * zombies that:
 *   (a) have no parent process alive (orphan), OR
 *   (b) have been waiting beyond REAP_GRACE_TICKS with no parent
 *       blocked in wait4 on them.
 *
 * Before reclaiming, each victim is dumped to klog (mirrored to
 * dmesg via serial) so postmortem analysis is still possible.
 */
#define REAP_GRACE_TICKS  1000   /* 10 s at 100 Hz APIC */
#define REAP_SWEEP_TICKS  100    /* 1 s between sweeps */

extern uint64_t paging_get_kernel_cr3(void);

static int proc_has_blocked_waiter(uint32_t pid, uint32_t ppid)
{
    /* True if any process is PROC_BLOCKED on wait4 expecting this
     * zombie. We don't track the wait target explicitly — wait4 just
     * parks the parent in BLOCKED — so we approximate: parent (ppid)
     * is alive and currently BLOCKED. That's the same predicate
     * proc_exit() uses to wake a waiter (line 781). */
    if (ppid == 0) return 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].pid == ppid &&
            proctab[i].state == PROC_BLOCKED) {
            (void)pid;
            return 1;
        }
    }
    return 0;
}

static int proc_parent_alive(uint32_t ppid)
{
    if (ppid == 0) return 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].pid == ppid &&
            proctab[i].state != PROC_FREE &&
            proctab[i].state != PROC_ZOMBIE)
            return 1;
    }
    return 0;
}

static void proc_reap_dump(const process_t *p, uint64_t age_ticks,
                            const char *reason)
{
    serial_puts("[reaper] ");
    serial_puts(reason);
    serial_puts(" pid=");
    serial_putdec((uint64_t)p->pid);
    serial_puts(" name='");
    serial_puts(p->name[0] ? p->name : "(anon)");
    serial_puts("' ppid=");
    serial_putdec((uint64_t)p->ppid);
    serial_puts(" exit=");
    serial_putdec((uint64_t)(uint32_t)p->exit_code);
    serial_puts(" age=");
    serial_putdec(age_ticks * 10);
    serial_puts("ms");
    if (p->cr3 && p->cr3 != paging_get_kernel_cr3()) {
        serial_puts(" cr3=0x");
        serial_puthex(p->cr3, 16);
    }
    if (p->fd_table) {
        serial_puts(" fd_refs=");
        serial_putdec((uint64_t)p->fd_table->refcount);
    }
    if (p->region_count > 0) {
        serial_puts(" regions=");
        serial_putdec((uint64_t)p->region_count);
    }
    if (p->kernel_stack) {
        serial_puts(" kstack=0x");
        serial_puthex((uint64_t)p->kernel_stack, 16);
    }
    if (p->saved_frame_rip) {
        serial_puts(" last_rip=0x");
        serial_puthex(p->saved_frame_rip, 16);
    }
    serial_puts("\n");
}

/* Scan the proctab once and reap eligible zombies. Safe to call from
 * any context that can be preempted; the loop is O(MAX_PROCESSES). */
void proc_reap_zombies(void)
{
    uint64_t now = idt_get_ticks();
    int reaped = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proctab[i];
        if (p->state != PROC_ZOMBIE) continue;
        if (p == current_proc) continue;   /* paranoia */

        /* If a parent is currently blocked in wait4 for this child,
         * leave the zombie for it. wait4 wants to observe exit_code. */
        if (proc_has_blocked_waiter(p->pid, p->ppid))
            continue;

        uint64_t age = now - p->zombie_tick;
        const char *reason;
        if (!proc_parent_alive(p->ppid)) {
            /* Orphan: parent already gone (or kernel PID 0 / 1 isn't
             * going to wait on its child). Reap immediately. */
            reason = "reaped orphan";
        } else if (age >= REAP_GRACE_TICKS) {
            /* Parent alive but not waiting within the grace window.
             * It had its chance — reclaim the slot. */
            reason = "reaped (parent-no-wait)";
        } else {
            continue;
        }

        proc_reap_dump(p, age, reason);
        proc_free(p);
        reaped++;
    }
    (void)reaped;
}

/* Periodic kthread: sweep zombies once per second.
 * Runs at QOS_BACKGROUND so it can never starve real work. */
static void zombie_reaper_thread(void)
{
    extern void proc_set_qos(uint8_t qos);
    proc_set_qos(QOS_BACKGROUND);
    serial_puts("[reaper] zombie auto-reaper online (QOS_BACKGROUND, "
                "sweep=1s, grace=10s)\n");
    while (1) {
        proc_reap_zombies();
        /* Sleep ~1 s on APIC ticks. hlt + IRQ; if APIC is masked the
         * sweep cadence stretches, but that's fine — we're not on a
         * latency budget here. */
        uint64_t target = idt_get_ticks() + REAP_SWEEP_TICKS;
        while (idt_get_ticks() < target)
            __asm__ volatile ("hlt");
    }
}

void proc_init(void)
{
    serial_puts("[PROC] Initializing process subsystem...\n");

    memset(proctab, 0, sizeof(proctab));
    runq_init();
    futex_init();
    set_current_proc(NULL);

    /* Create PID 1 (kernel) */
    process_t *kernel = proc_alloc("kernel");
    if (kernel) {
        proc_transition(kernel, PROC_RUNNING);
        set_current_proc(kernel);
        sched_current_idx = (int)(kernel - &proctab[0]);
        /* Seed kernel process with stdin/stdout/stderr = console.
         * All future processes inherit or reset these via execve. */
        extern void syscall_seed_stdio(fd_entry_t *fds);
        syscall_seed_stdio(kernel->fd_table->entries);
        serial_puts("[PROC] Kernel process PID ");
        serial_putdec(kernel->pid);
        serial_puts("\n");
    }

    fb_puts(" Process subsystem ready\n");
}

/* Spawn the auto-reaper. Called from main.c after all boot
 * initialization is complete — not from proc_init, because spawning
 * a thread at proc_init time auto-enables preemption, and any APIC
 * tick during the still-running PCI / driver init steps would try
 * to dispatch the new thread from a low-half boot-stack context
 * which the scheduler isn't ready to context-switch out of. */
void proc_start_reaper(void)
{
    sched_spawn("reaper", zombie_reaper_thread);
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
