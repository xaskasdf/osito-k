/*
 * sys.c -- program entry and system calls (Osito-K AArch64 libc)
 *
 * Programs are static PIEs that the kernel (kernel/elf.c) relocates and
 * calls as _start(argc, argv) on their own stack, at EL1. System calls are
 * Linux AArch64's: svc #0, number in x8, arguments in x0-x5, -errno back in
 * x0 (kernel/syscall.c).
 */

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SYS_openat          56
#define SYS_close           57
#define SYS_lseek           62
#define SYS_read            63
#define SYS_write           64
#define SYS_exit            93
#define SYS_nanosleep       101
#define SYS_clock_gettime   113
#define SYS_getpid          172
#define SYS_brk             214

int errno;

static long sc6(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

long syscall(long nr, ...)
{
    va_list ap;
    va_start(ap, nr);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    return sc6(nr, a, b, c, d, e, f);
}

/* -errno to errno and -1 */
static long ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

ssize_t read(int fd, void *buf, size_t n)  { return ret(sc6(SYS_read, fd, (long)buf, (long)n, 0, 0, 0)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(sc6(SYS_write, fd, (long)buf, (long)n, 0, 0, 0)); }
int close(int fd)                           { return (int)ret(sc6(SYS_close, fd, 0, 0, 0, 0, 0)); }
off_t lseek(int fd, off_t off, int whence)  { return ret(sc6(SYS_lseek, fd, off, whence, 0, 0, 0)); }
pid_t getpid(void)                          { return (pid_t)sc6(SYS_getpid, 0, 0, 0, 0, 0, 0); }

int open(const char *path, int flags, ...)
{
    return (int)ret(sc6(SYS_openat, AT_FDCWD, (long)path, flags, 0, 0, 0));
}

void _exit(int status)
{
    for (;;)
        sc6(SYS_exit, status, 0, 0, 0, 0, 0);
}

/* ── The heap: brk moves within a region the kernel set aside ── */

static char *cur_brk;

int brk(void *addr)
{
    char *got = (char *)sc6(SYS_brk, (long)addr, 0, 0, 0, 0, 0);
    cur_brk = got;
    if (got != addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void *sbrk(intptr_t increment)
{
    if (!cur_brk)
        cur_brk = (char *)sc6(SYS_brk, 0, 0, 0, 0, 0, 0);
    char *old = cur_brk;
    if (increment && brk(old + increment) < 0)
        return (void *)-1;
    return old;
}

/* ── Time ───────────────────────────────────────────────────── */

int clock_gettime(int clk, struct timespec *ts)
{
    return (int)ret(sc6(SYS_clock_gettime, clk, (long)ts, 0, 0, 0, 0));
}

time_t time(time_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

clock_t clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (clock_t)ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / 1000;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    return (int)ret(sc6(SYS_nanosleep, (long)req, 0, 0, 0, 0, 0));
}

unsigned sleep(unsigned seconds)
{
    struct timespec ts = { (time_t)seconds, 0 };
    nanosleep(&ts, 0);
    return 0;
}

int usleep(useconds_t usec)
{
    struct timespec ts = { (time_t)(usec / 1000000), (long)(usec % 1000000) * 1000 };
    return nanosleep(&ts, 0);
}

/* ── Entry ──────────────────────────────────────────────────── */

extern int main(int argc, char **argv, char **envp);
static char *no_env[1];
char **environ = no_env;

void _start(int argc, char **argv) __attribute__((noreturn, used));
void _start(int argc, char **argv)
{
    exit(main(argc, argv, environ));
}

void __assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
    abort();
}
