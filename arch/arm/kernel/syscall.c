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
#define SYS_fcntl           25
#define SYS_openat          56
#define SYS_lseek           62
#define SYS_fstat           80
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_getpid          172
#define SYS_gettid          178
#define SYS_getuid          174
#define SYS_getgid          176
#define SYS_geteuid         175
#define SYS_getegid         177
#define SYS_mprotect        226
#define SYS_getrandom       278

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

/* ── File Descriptor Table ───────────────────────────────── */

#define MAX_FDS  32
typedef struct {
    osfs2_file_t *file;     /* NULL = unused */
    uint64_t      pos;      /* current file position */
    int           flags;    /* O_RDONLY etc */
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];

static void fd_init(void)
{
    for (int i = 0; i < MAX_FDS; i++) fd_table[i].file = (void *)0;
    /* fd 0/1/2 are special (stdin/stdout/stderr) — no file pointer */
}

static int fd_alloc(osfs2_file_t *f)
{
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i].file) {
            fd_table[i].file = f;
            fd_table[i].pos = 0;
            fd_table[i].flags = 0;
            return i;
        }
    }
    return -24;  /* -EMFILE */
}

/* ── Syscall capture buffer (for claude.c tool execution) ── */

static char *capture_buf;
static int   capture_size;
static int   capture_pos;
static int   capturing;

void syscall_capture_start(void)
{
    capturing = 1;
    capture_pos = 0;
}

void syscall_capture_stop(char *buf, int bufsize)
{
    capturing = 0;
    if (capture_buf && buf && capture_pos > 0) {
        int len = capture_pos < bufsize - 1 ? capture_pos : bufsize - 1;
        memcpy(buf, capture_buf, len);
        buf[len] = 0;
    } else if (buf && bufsize > 0) {
        buf[0] = 0;
    }
}

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
    if (fd == 0) {
        /* stdin: read one char from UART */
        char *dst = (char *)buf;
        for (uint64_t i = 0; i < count; i++) {
            dst[i] = uart_getc();
            if (dst[i] == '\r') dst[i] = '\n';
            if (dst[i] == '\n') return (int64_t)(i + 1);
        }
        return (int64_t)count;
    }
    if (fd >= 3 && fd < MAX_FDS && fd_table[fd].file) {
        osfs2_file_t *f = fd_table[fd].file;
        uint64_t pos = fd_table[fd].pos;
        if (pos >= f->size) return 0;  /* EOF */
        uint64_t avail = f->size - pos;
        if (count > avail) count = avail;
        if (osfs2_read(f, pos, (void *)buf, count) < 0) return -5;  /* -EIO */
        fd_table[fd].pos += count;
        return (int64_t)count;
    }
    return -9;  /* -EBADF */
}

static int64_t sys_openat(uint64_t dirfd, uint64_t pathname, uint64_t flags)
{
    (void)dirfd; (void)flags;
    const char *name = (const char *)pathname;
    osfs2_file_t *f = osfs2_find(name);
    if (!f) return -2;  /* -ENOENT */
    return fd_alloc(f);
}

static int64_t sys_close(uint64_t fd)
{
    if (fd >= 3 && fd < MAX_FDS && fd_table[fd].file) {
        fd_table[fd].file = (void *)0;
        fd_table[fd].pos = 0;
        return 0;
    }
    return 0;  /* silently succeed for stdin/stdout/stderr */
}

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence)
{
    if (fd < 3 || fd >= MAX_FDS || !fd_table[fd].file) return -9;
    osfs2_file_t *f = fd_table[fd].file;
    int64_t new_pos;
    if (whence == 0) new_pos = offset;                          /* SEEK_SET */
    else if (whence == 1) new_pos = (int64_t)fd_table[fd].pos + offset;  /* SEEK_CUR */
    else if (whence == 2) new_pos = (int64_t)f->size + offset;  /* SEEK_END */
    else return -22;  /* -EINVAL */
    if (new_pos < 0) return -22;
    fd_table[fd].pos = (uint64_t)new_pos;
    return new_pos;
}

static int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags)
{
    (void)flags;
    uint8_t *dst = (uint8_t *)buf;
    uint64_t state;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(state));
    for (uint64_t i = 0; i < len; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        dst[i] = (uint8_t)state;
    }
    return (int64_t)len;
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
    case SYS_close:           ret = sys_close(a0); break;
    case SYS_openat:          ret = sys_openat(a0, a1, a2); break;
    case SYS_lseek:           ret = sys_lseek(a0, (int64_t)a1, a2); break;
    case SYS_fstat:           ret = 0; break;    /* stub */
    case SYS_exit:            ret = sys_exit_impl(a0, frame); break;
    case SYS_exit_group:      ret = sys_exit_impl(a0, frame); break;
    case SYS_brk:             ret = sys_brk(a0); break;
    case SYS_mmap:            ret = sys_mmap(a0, a1, a2, a3, a4, a5); break;
    case SYS_munmap:          ret = sys_munmap(a0, a1); break;
    case SYS_mprotect:        ret = 0; break;    /* stub */
    case SYS_ioctl:           ret = 0; break;
    case SYS_fcntl:           ret = 0; break;    /* stub */
    case SYS_set_tid_address: ret = 1; break;
    case SYS_getpid:          ret = 1; break;
    case SYS_gettid:          ret = 1; break;
    case SYS_getuid:          ret = 0; break;
    case SYS_getgid:          ret = 0; break;
    case SYS_geteuid:         ret = 0; break;
    case SYS_getegid:         ret = 0; break;
    case SYS_rt_sigaction:    ret = 0; break;    /* stub */
    case SYS_rt_sigprocmask:  ret = 0; break;    /* stub */
    case SYS_getrandom:       ret = sys_getrandom(a0, a1, a2); break;
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
