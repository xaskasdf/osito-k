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

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void fb_puts(const char *s);
extern void fb_puts_color(const char *s, uint32_t color);
extern void fb_putdec(uint64_t val);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void *mem_alloc_aligned(uint64_t size, uint64_t alignment);
extern void  mem_free_pages(void *addr, uint64_t count);
extern uint64_t paging_get_kernel_cr3(void);

/* ELF loader */
extern int elf_exec(const char *filename, int argc, const char **argv);

/* Syscall state cleanup */
extern void syscall_reset_process(void);

/* ── Constants ───────────────────────────────────────────────── */

#define MAX_PROCESSES   16
#define MAX_FDS         16
#define MAX_NAME_LEN    64
#define MAX_REGIONS     16

/* Process states */
#define PROC_FREE       0
#define PROC_READY      1
#define PROC_RUNNING    2
#define PROC_BLOCKED    3
#define PROC_ZOMBIE     4

/* Scheduler constants (X-SCHED) */
#define SCHED_QUANTUM       5       /* ticks per time slice (50ms @ 100Hz) */
#define KERNEL_STACK_SIZE   16384   /* 16KB per kernel thread */

/* ── File descriptor ─────────────────────────────────────────── */

typedef ssize_t (*fd_read_fn)(void *buf, size_t count);
typedef ssize_t (*fd_write_fn)(const void *buf, size_t count);

typedef struct {
    bool        open;
    fd_read_fn  read;
    fd_write_fn write;
    uint32_t    flags;
} proc_fd_t;

/* ── Memory region tracking ──────────────────────────────────── */

typedef struct {
    void    *base;
    uint64_t pages;
} mem_region_t;

/* ── Process structure ───────────────────────────────────────── */

typedef struct {
    uint32_t    pid;
    uint32_t    state;
    char        name[MAX_NAME_LEN];
    int32_t     exit_code;

    /* File descriptors */
    proc_fd_t   fds[MAX_FDS];

    /* Memory regions (for cleanup) */
    mem_region_t regions[MAX_REGIONS];
    int          region_count;

    /* Address space (for future per-process paging) */
    uint64_t    cr3;

    /* Scheduler context (X-SCHED) */
    void    *kernel_stack;       /* allocated kernel stack (NULL for kernel proc) */
    uint64_t kernel_rsp;         /* saved RSP pointing to interrupt frame */
    uint32_t quantum;            /* ticks remaining in time slice */
} process_t;

/* ── Process table ───────────────────────────────────────────── */

static process_t proctab[MAX_PROCESSES];
static process_t *current_proc;
static uint32_t next_pid = 1;

/* Kernel return context — saved before exec, restored on exit */
extern int  kern_setjmp(uint64_t *buf);
extern void kern_longjmp(uint64_t *buf, int val);

static uint64_t exec_jmpbuf[8];   /* setjmp/longjmp buffer */
static int32_t  last_exit_code;

/* Console I/O (shared with syscall.c) */
extern void serial_putc(char c);
extern void fb_putc(char c, uint32_t color);

static ssize_t console_write(const void *buf, size_t count)
{
    const char *s = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        serial_putc(s[i]);
        fb_putc(s[i], 0x00CCCCCC);
    }
    return (ssize_t)count;
}

static ssize_t console_read(void *buf, size_t count)
{
    (void)buf; (void)count;
    return 0;  /* EOF until keyboard driver */
}

/* ── Allocate a process slot ─────────────────────────────────── */

static process_t *proc_alloc(const char *name)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proctab[i].state == PROC_FREE) {
            process_t *p = &proctab[i];
            memset(p, 0, sizeof(*p));
            p->pid = next_pid++;
            p->state = PROC_READY;
            p->cr3 = paging_get_kernel_cr3();

            /* Copy name */
            int j = 0;
            while (name[j] && j < MAX_NAME_LEN - 1) {
                p->name[j] = name[j];
                j++;
            }
            p->name[j] = '\0';

            /* Setup standard FDs */
            p->fds[0].open = true;
            p->fds[0].read = console_read;

            p->fds[1].open = true;
            p->fds[1].write = console_write;

            p->fds[2].open = true;
            p->fds[2].write = console_write;

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

    /* Reset per-process syscall state (file FDs, brk heap) */
    syscall_reset_process();

    /* Close FDs */
    for (int i = 0; i < MAX_FDS; i++)
        p->fds[i].open = false;

    p->state = PROC_FREE;
}

/* ── Register memory region with current process (for cleanup) ── */

void proc_add_region(void *base, uint64_t pages)
{
    if (!current_proc) return;
    process_t *p = current_proc;
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

/* Get current PID */
int32_t proc_current_pid(void)
{
    return current_proc ? (int32_t)current_proc->pid : 0;
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

/* Process exit — called from sys_exit() */
void proc_exit(int32_t code)
{
    last_exit_code = code;
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

    /* Set as current process */
    process_t *prev = current_proc;
    current_proc = p;
    p->state = PROC_RUNNING;

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
        int code = last_exit_code;
        current_proc = prev;
        proc_free(p);
        return code;
    }

    /* Execute ELF — this calls elf_exec which does not return
     * on success (jumps to ELF entry). On failure, returns here. */
    int ret = elf_exec(filename, argc, argv);

    /* If we get here, exec failed */
    serial_puts("[PROC] exec failed for '");
    serial_puts(filename);
    serial_puts("'\n");

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

    /* Decrement quantum — if still running, continue */
    if (cur->quantum > 1) {
        cur->quantum--;
        return;
    }

    /* Quantum expired — find next READY process (round-robin) */
    int next_idx = -1;
    for (int i = 1; i <= MAX_PROCESSES; i++) {
        int idx = (sched_current_idx + i) % MAX_PROCESSES;
        if (proctab[idx].state == PROC_READY) {
            next_idx = idx;
            break;
        }
    }

    if (next_idx < 0) {
        /* No other runnable process — reset quantum, continue */
        cur->quantum = SCHED_QUANTUM;
        return;
    }

    /* ── Context switch ────────────────────────────────────────── */

    /* Save current process: frame_ptr is RSP pointing to the saved
     * GPRs on this process's stack (set by ISR stub before calling
     * isr_handler). Store it so we can restore later. */
    cur->kernel_rsp = (uint64_t)frame_ptr;
    cur->state = PROC_READY;

    /* Load next process */
    process_t *next = &proctab[next_idx];
    next->state = PROC_RUNNING;
    next->quantum = SCHED_QUANTUM;
    current_proc = next;
    sched_current_idx = next_idx;

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

    /* Allocate kernel stack */
    void *stack = mem_alloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!stack) {
        p->state = PROC_FREE;
        serial_puts("[SCHED] Stack allocation failed\n");
        return -1;
    }
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
    p->quantum = SCHED_QUANTUM;

    /* Auto-activate scheduler on first spawn */
    if (!sched_enabled) {
        /* Set up kernel process for scheduling */
        proctab[sched_current_idx].quantum = SCHED_QUANTUM;
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

/* ── Initialize process subsystem ────────────────────────────── */

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
        serial_puts("[PROC] Kernel process PID ");
        serial_putdec(kernel->pid);
        serial_puts("\n");
    }

    fb_puts(" Process subsystem ready\n");
}
