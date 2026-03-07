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
#define PROC_ZOMBIE     3

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

    /* Saved context (for future scheduling) */
    uint64_t    rsp;
    uint64_t    rip;
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

/* ── Initialize process subsystem ────────────────────────────── */

void proc_init(void)
{
    serial_puts("[PROC] Initializing process subsystem...\n");

    memset(proctab, 0, sizeof(proctab));
    current_proc = NULL;

    /* Create PID 0 (kernel) */
    process_t *kernel = proc_alloc("kernel");
    if (kernel) {
        kernel->state = PROC_RUNNING;
        current_proc = kernel;
        serial_puts("[PROC] Kernel process PID ");
        serial_putdec(kernel->pid);
        serial_puts("\n");
    }

    fb_puts(" Process subsystem ready\n");
}
