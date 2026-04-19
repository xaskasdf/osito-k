#include <stdarg.h>

/* ── Syscall primitives (from syscall.S) ── */

extern long __syscall0(long nr);
extern long __syscall1(long nr, long a1);
extern long __syscall2(long nr, long a1, long a2);
extern long __syscall3(long nr, long a1, long a2, long a3);
extern long __syscall4(long nr, long a1, long a2, long a3, long a4);
extern long __syscall5(long nr, long a1, long a2, long a3, long a4, long a5);
extern long __syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);

/* ── Syscall numbers (Linux x86-64) ── */

#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_stat      4
#define SYS_fstat     5
#define SYS_lstat     6
#define SYS_poll      7
#define SYS_lseek     8
#define SYS_mmap      9
#define SYS_mprotect  10
#define SYS_munmap    11
#define SYS_brk       12
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_ioctl     16
#define SYS_access    21
#define SYS_pipe      22
#define SYS_dup2      33
#define SYS_nanosleep 35
#define SYS_getpid    39
#define SYS_fork      57
#define SYS_execve    59
#define SYS_exit      60
#define SYS_wait4     61
#define SYS_kill      62
#define SYS_fcntl     72
#define SYS_getcwd    79
#define SYS_chdir     80
#define SYS_umask     95
#define SYS_setpgid   109
#define SYS_getppid   110
#define SYS_getpgid   121
#define SYS_getdents64 217

/* ── POSIX-like wrappers ── */

typedef unsigned long size_t;
typedef long ssize_t;

char **environ;

extern int *__errno_location(void);
#define errno (*__errno_location())

void _exit(int status)
{
    __syscall1(SYS_exit, status);
    __builtin_unreachable();
}

ssize_t write(int fd, const void *buf, size_t count)
{
    long ret = __syscall3(SYS_write, fd, (long)buf, (long)count);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

ssize_t read(int fd, void *buf, size_t count)
{
    long ret = __syscall3(SYS_read, fd, (long)buf, (long)count);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

int open(const char *path, int flags, ...)
{
    long ret = __syscall3(SYS_open, (long)path, flags, 0);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int close(int fd)
{
    long ret = __syscall1(SYS_close, fd);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

long lseek(int fd, long offset, int whence)
{
    long ret = __syscall3(SYS_lseek, fd, offset, whence);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return ret;
}

int pipe(int pipefd[2])
{
    long ret = __syscall1(SYS_pipe, (long)pipefd);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int dup2(int oldfd, int newfd)
{
    long ret = __syscall2(SYS_dup2, oldfd, newfd);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int nanosleep(const void *req, void *rem)
{
    long ret = __syscall2(SYS_nanosleep, (long)req, (long)rem);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int getpid(void)
{
    return (int)__syscall0(SYS_getpid);
}

int getppid(void)
{
    return (int)__syscall0(SYS_getppid);
}

int fork(void)
{
    long ret = __syscall0(SYS_fork);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    long ret = __syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int kill(int pid, int sig)
{
    long ret = __syscall2(SYS_kill, pid, sig);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int stat(const char *path, void *buf)
{
    long ret = __syscall2(SYS_stat, (long)path, (long)buf);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int fstat(int fd, void *buf)
{
    long ret = __syscall2(SYS_fstat, fd, (long)buf);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int lstat(const char *path, void *buf)
{
    long ret = __syscall2(SYS_lstat, (long)path, (long)buf);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int access(const char *path, int mode)
{
    long ret = __syscall2(SYS_access, (long)path, mode);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int chdir(const char *path)
{
    long ret = __syscall1(SYS_chdir, (long)path);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int umask(int mask)
{
    return (int)__syscall1(SYS_umask, mask);
}

int poll(void *fds, unsigned long nfds, int timeout)
{
    long ret = __syscall3(SYS_poll, (long)fds, nfds, timeout);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int ioctl(int fd, unsigned long request, void *arg)
{
    long ret = __syscall3(SYS_ioctl, fd, request, (long)arg);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    long ret = __syscall3(SYS_fcntl, fd, cmd, arg);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

char *getcwd(char *buf, size_t size)
{
    long ret = __syscall2(SYS_getcwd, (long)buf, (long)size);
    if (ret < 0) { errno = (int)-ret; return (void *)0; }
    return (char *)ret;
}

long getdents64(int fd, void *dirp, size_t count)
{
    long ret = __syscall3(SYS_getdents64, fd, (long)dirp, (long)count);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return ret;
}

int wait4(int pid, int *wstatus, int options, void *rusage)
{
    long ret = __syscall4(SYS_wait4, pid, (long)wstatus, options, (long)rusage);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int setpgid(int pid, int pgid)
{
    long ret = __syscall2(SYS_setpgid, pid, pgid);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int getpgid(int pid)
{
    long ret = __syscall1(SYS_getpgid, pid);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int sys_sigaction(int signum, const void *act, void *oldact)
{
    long ret = __syscall4(SYS_rt_sigaction, signum, (long)act, (long)oldact, 8);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int sys_sigprocmask(int how, const void *set, void *oldset)
{
    long ret = __syscall4(SYS_rt_sigprocmask, how, (long)set, (long)oldset, 8);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

/* ── mmap/munmap/mprotect ── */

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    long ret = __syscall6(SYS_mmap, (long)addr, (long)length,
                          (long)prot, (long)flags, (long)fd, offset);
    if (ret < 0) { errno = (int)-ret; return MAP_FAILED; }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    long ret = __syscall2(SYS_munmap, (long)addr, (long)length);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

int mprotect(void *addr, size_t length, int prot)
{
    long ret = __syscall3(SYS_mprotect, (long)addr, (long)length, (long)prot);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}

/* ── sbrk wrapper ── */

static long sys_brk(long addr)
{
    return __syscall1(SYS_brk, addr);
}

void *sbrk(long increment)
{
    long current_brk = sys_brk(0);
    if (increment == 0) {
        return (void *)current_brk;
    }
    long new_brk = sys_brk(current_brk + increment);
    if (new_brk == current_brk) {
        /* Failed to grow */
        return (void *)-1;
    }
    return (void *)current_brk;
}

/* ── String functions ── */

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char val = (unsigned char)c;

    /* Small fills: byte loop (avoids REP setup overhead) */
    if (n < 64) {
        /* Word-fill for aligned runs >= 8 bytes */
        if (n >= 8) {
            unsigned long w = val;
            w |= w << 8;  w |= w << 16;  w |= w << 32;
            while (((unsigned long)d & 7) && n) { *d++ = val; n--; }
            while (n >= 8) { *(unsigned long *)d = w; d += 8; n -= 8; }
        }
        while (n--) *d++ = val;
        return dst;
    }

    /* Large fills: REP STOSB (ERMS — 256-bit internal stores on modern CPUs) */
    __asm__ volatile (
        "rep stosb"
        : "+D"(d), "+c"(n)
        : "a"(val)
        : "memory"
    );
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Small copies: word-at-a-time then byte tail */
    if (n < 64) {
        while (n >= 8 && !((unsigned long)d & 7) && !((unsigned long)s & 7)) {
            *(unsigned long *)d = *(const unsigned long *)s;
            d += 8; s += 8; n -= 8;
        }
        while (n--) *d++ = *s++;
        return dst;
    }

    /* Large copies: REP MOVSB (ERMS — 256-bit internal stores on modern CPUs) */
    __asm__ volatile (
        "rep movsb"
        : "+D"(d), "+S"(s), "+c"(n)
        :
        : "memory"
    );
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ── Simple printf (integer formats only) ── */

static void putchar_fd(int fd, char c)
{
    write(fd, &c, 1);
}

static void puts_fd(int fd, const char *s)
{
    write(fd, s, strlen(s));
}

static void putdec_fd(int fd, long val)
{
    if (val < 0) { putchar_fd(fd, '-'); val = -val; }
    char buf[20];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    else while (val > 0) { buf[i++] = '0' + (int)(val % 10); val /= 10; }
    while (i > 0) putchar_fd(fd, buf[--i]);
}

static void puthex_fd(int fd, unsigned long val)
{
    const char *hex = "0123456789abcdef";
    char buf[16];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    else while (val > 0) { buf[i++] = hex[val & 0xf]; val >>= 4; }
    while (i > 0) putchar_fd(fd, buf[--i]);
}

int vprintf(const char *fmt, va_list ap)
{
    int count = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar_fd(1, *fmt);
            count++;
            continue;
        }
        fmt++;
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            puts_fd(1, s);
            count += (int)strlen(s);
            break;
        }
        case 'd': {
            long v = is_long ? va_arg(ap, long)
                             : (long)va_arg(ap, int);
            putdec_fd(1, v);
            count++;
            break;
        }
        case 'u': {
            unsigned long v = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
            putdec_fd(1, (long)v);
            count++;
            break;
        }
        case 'x': {
            unsigned long v = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
            puthex_fd(1, v);
            count++;
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            puts_fd(1, "0x");
            puthex_fd(1, v);
            count += 3;
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            putchar_fd(1, (char)c);
            count++;
            break;
        }
        case '%':
            putchar_fd(1, '%');
            count++;
            break;
        default:
            putchar_fd(1, '%');
            putchar_fd(1, *fmt);
            count += 2;
            break;
        }
    }
    return count;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int count = vprintf(fmt, ap);
    va_end(ap);
    return count;
}

int puts(const char *s)
{
    puts_fd(1, s);
    putchar_fd(1, '\n');
    return 0;
}
