/*
 * syscall.c -- Linux AArch64 syscall dispatch
 *
 * SVC #0 entry: x8 = syscall number, x0-x5 = args, return in x0.
 * Frame pointer passed from _svc_entry in start.S.
 */

#include "../include/hal.h"
#include "../include/types.h"

/* Linux AArch64 syscall numbers (from asm-generic/unistd.h) */
#define SYS_close           57
#define SYS_read            63
#define SYS_write           64
#define SYS_writev          66
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_set_tid_address 96
#define SYS_clock_gettime   113
#define SYS_brk             214
#define SYS_mmap            222
#define SYS_munmap          215
#define SYS_ioctl           29
#define SYS_getpid          172
#define SYS_gettid          178

/* ── Process state (simplified for Phase 3) ──────────────── */

static int process_running;
static int process_exit_code;
static uint64_t brk_current;
static uint64_t brk_base;

void proc_set_brk(uint64_t base)
{
    brk_base = base;
    brk_current = base;
}

int proc_get_exit_code(void) { return process_exit_code; }
int proc_is_running(void) { return process_running; }
void proc_mark_running(void) { process_running = 1; }

/* ── Syscall implementations ─────────────────────────────── */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd == 1 || fd == 2) {
        /* stdout/stderr → serial */
        const char *p = (const char *)buf;
        for (uint64_t i = 0; i < count; i++)
            serial_putc(p[i]);
        return (int64_t)count;
    }
    return -9;  /* -EBADF */
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    (void)fd; (void)buf; (void)count;
    return -9;  /* -EBADF: no input for now */
}

/* Longjmp buffer set by proc_run before calling user entry */
static void *exit_jmpbuf[5];
void proc_set_exit_jmpbuf(void *buf[5])
{
    for (int i = 0; i < 5; i++) exit_jmpbuf[i] = buf[i];
}

static int64_t sys_exit_impl(uint64_t code, uint64_t *frame)
{
    (void)frame;
    process_running = 0;
    process_exit_code = (int)code;
    /* Jump back to proc_run */
    __builtin_longjmp(exit_jmpbuf, 1);
    return 0;  /* unreachable */
}

static int64_t sys_brk(uint64_t addr)
{
    if (addr == 0 || addr < brk_base)
        return (int64_t)brk_current;

    /* Grow brk (no actual page allocation in identity-mapped kernel) */
    if (addr > brk_current) {
        uint64_t new_pages = (addr - brk_current + 4095) / 4096;
        void *p = mem_alloc_pages(new_pages);
        if (!p) return (int64_t)brk_current;
    }
    brk_current = addr;
    return (int64_t)brk_current;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    /* Anonymous mmap only */
    uint64_t pages = (len + 4095) / 4096;
    void *p = mem_alloc_pages(pages);
    if (!p) return (int64_t)-12;  /* -ENOMEM */
    return (int64_t)(uint64_t)p;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    uint64_t pages = (len + 4095) / 4096;
    mem_free_pages((void *)addr, pages);
    return 0;
}

static int64_t sys_writev(uint64_t fd, uint64_t iov_ptr, uint64_t iovcnt)
{
    struct iovec { uint64_t base; uint64_t len; };
    const struct iovec *iov = (const struct iovec *)iov_ptr;
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        int64_t r = sys_write(fd, iov[i].base, iov[i].len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

static int64_t sys_clock_gettime(uint64_t clk, uint64_t tp)
{
    (void)clk;
    struct { uint64_t sec; uint64_t nsec; } *ts = (void *)tp;
    uint64_t ms = timer_ms();
    ts->sec = ms / 1000;
    ts->nsec = (ms % 1000) * 1000000;
    return 0;
}

/* ── Dispatch ────────────────────────────────────────────── */

void svc_handler(uint64_t *frame)
{
    uint64_t nr  = frame[8];   /* x8 = syscall number */
    uint64_t a0  = frame[0];   /* x0 */
    uint64_t a1  = frame[1];   /* x1 */
    uint64_t a2  = frame[2];   /* x2 */
    uint64_t a3  = frame[3];   /* x3 */
    uint64_t a4  = frame[4];   /* x4 */
    uint64_t a5  = frame[5];   /* x5 */
    (void)a3; (void)a4; (void)a5;

    int64_t ret;

    switch (nr) {
    case SYS_write:           ret = sys_write(a0, a1, a2); break;
    case SYS_writev:          ret = sys_writev(a0, a1, a2); break;
    case SYS_read:            ret = sys_read(a0, a1, a2); break;
    case SYS_close:           ret = 0; break;
    case SYS_exit:            ret = sys_exit_impl(a0, frame); break;
    case SYS_exit_group:      ret = sys_exit_impl(a0, frame); break;
    case SYS_brk:             ret = sys_brk(a0); break;
    case SYS_mmap:            ret = sys_mmap(a0, a1, a2, a3, a4, a5); break;
    case SYS_munmap:          ret = sys_munmap(a0, a1); break;
    case SYS_ioctl:           ret = 0; break;
    case SYS_set_tid_address: ret = 1; break;   /* return "PID" */
    case SYS_getpid:          ret = 1; break;
    case SYS_gettid:          ret = 1; break;
    case SYS_clock_gettime:   ret = sys_clock_gettime(a0, a1); break;
    default:
        serial_puts("[SYS ] Unknown syscall ");
        serial_putdec(nr);
        serial_puts("\n");
        ret = -38;  /* -ENOSYS */
        break;
    }

    frame[0] = (uint64_t)ret;  /* x0 = return value */
}
