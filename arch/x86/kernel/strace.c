/*
 * OsitoK x86-64 — Syscall Tracer (strace)
 *
 * Logs syscall invocations for a traced process.
 * Enable per-process via strace_enable(pid) or shell "strace" command.
 * Output goes to serial console.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint32_t proc_current_pid(void);

/* ── Trace State ─────────────────────────────────────────────── */

#define STRACE_MAX_PIDS 4

static uint32_t traced_pids[STRACE_MAX_PIDS];
static int traced_count;
static bool strace_global;  /* Trace ALL processes */

/* ── Syscall Name Table ──────────────────────────────────────── */

static const char *syscall_name(uint64_t nr)
{
    switch (nr) {
    case 0:   return "read";
    case 1:   return "write";
    case 2:   return "open";
    case 3:   return "close";
    case 5:   return "fstat";
    case 8:   return "lseek";
    case 9:   return "mmap";
    case 10:  return "mprotect";
    case 11:  return "munmap";
    case 12:  return "brk";
    case 13:  return "sigaction";
    case 14:  return "sigprocmask";
    case 17:  return "pread64";
    case 18:  return "pwrite64";
    case 20:  return "writev";
    case 21:  return "access";
    case 22:  return "pipe";
    case 24:  return "sched_yield";
    case 35:  return "nanosleep";
    case 37:  return "alarm";
    case 39:  return "getpid";
    case 41:  return "socket";
    case 42:  return "connect";
    case 43:  return "accept";
    case 44:  return "sendto";
    case 45:  return "recvfrom";
    case 49:  return "bind";
    case 50:  return "listen";
    case 56:  return "clone";
    case 57:  return "fork";
    case 59:  return "execve";
    case 60:  return "exit";
    case 62:  return "kill";
    case 72:  return "fcntl";
    case 79:  return "getcwd";
    case 80:  return "chdir";
    case 87:  return "unlink";
    case 89:  return "readlink";
    case 158: return "arch_prctl";
    case 217: return "getdents64";
    case 231: return "exit_group";
    case 232: return "epoll_wait";
    case 233: return "epoll_ctl";
    case 291: return "epoll_create1";
    case 293: return "pipe2";
    default:  return NULL;
    }
}

/* ── Public API ──────────────────────────────────────────────── */

void strace_enable(uint32_t pid)
{
    if (traced_count >= STRACE_MAX_PIDS) return;
    traced_pids[traced_count++] = pid;
    serial_puts("[STRACE] Tracing PID ");
    serial_putdec(pid);
    serial_puts("\n");
}

void strace_disable(uint32_t pid)
{
    for (int i = 0; i < traced_count; i++) {
        if (traced_pids[i] == pid) {
            traced_pids[i] = traced_pids[--traced_count];
            return;
        }
    }
}

void strace_enable_all(void) { strace_global = true; }
void strace_disable_all(void) { strace_global = false; }

/* Check if current process is being traced */
static bool strace_active(void)
{
    if (strace_global) return true;
    uint32_t pid = proc_current_pid();
    for (int i = 0; i < traced_count; i++) {
        if (traced_pids[i] == pid) return true;
    }
    return false;
}

/* ── Log Entry/Exit ──────────────────────────────────────────── */

void strace_log_entry(uint64_t nr, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5)
{
    if (!strace_active()) return;

    serial_puts("[");
    serial_putdec(proc_current_pid());
    serial_puts("] ");

    const char *name = syscall_name(nr);
    if (name) {
        serial_puts(name);
    } else {
        serial_puts("syscall_");
        serial_putdec(nr);
    }

    serial_puts("(");

    /* Print first 3 args for common syscalls */
    if (nr == 2 /* open */) {
        /* Print path string */
        const char *path = (const char *)a1;
        if (path) { serial_puts("\""); serial_puts(path); serial_puts("\""); }
        serial_puts(", 0x"); serial_puthex(a2, 4);
    } else if (nr == 0 /* read */ || nr == 1 /* write */) {
        serial_puts("fd="); serial_putdec(a1);
        serial_puts(", buf=0x"); serial_puthex(a2, 12);
        serial_puts(", len="); serial_putdec(a3);
    } else if (nr == 9 /* mmap */) {
        serial_puts("addr=0x"); serial_puthex(a1, 12);
        serial_puts(", len="); serial_putdec(a2);
        serial_puts(", prot="); serial_putdec(a3);
    } else {
        serial_puts("0x"); serial_puthex(a1, 8);
        if (a2) { serial_puts(", 0x"); serial_puthex(a2, 8); }
        if (a3) { serial_puts(", 0x"); serial_puthex(a3, 8); }
    }
    (void)a4; (void)a5;

    serial_puts(")");
}

void strace_log_exit(uint64_t nr, int64_t ret)
{
    if (!strace_active()) return;
    (void)nr;

    serial_puts(" = ");
    if (ret < 0) {
        serial_puts("-");
        serial_putdec((uint64_t)(-ret));
    } else {
        serial_puts("0x");
        serial_puthex((uint64_t)ret, 8);
    }
    serial_puts("\n");
}
