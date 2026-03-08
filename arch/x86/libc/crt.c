/*
 * OsitoK x86-64 — Minimal C runtime for freestanding programs.
 *
 * Provides _start (ELF entry), syscall wrappers, and basic string
 * functions so TCC-compiled C programs can run on OsitoK.
 *
 * Link order: crt.o syscall.o <user>.o → static ELF
 */

/* ── Syscall primitives (from syscall.S) ── */

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
#define SYS_lseek     8
#define SYS_mmap      9
#define SYS_mprotect  10
#define SYS_munmap    11
#define SYS_brk       12
#define SYS_pipe      22
#define SYS_dup2      33
#define SYS_exit      60
#define SYS_kill      62

/* ── POSIX-like wrappers ── */

typedef unsigned long size_t;
typedef long ssize_t;

void _exit(int status)
{
    __syscall1(SYS_exit, status);
    __builtin_unreachable();
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return __syscall3(SYS_write, fd, (long)buf, (long)count);
}

ssize_t read(int fd, void *buf, size_t count)
{
    return __syscall3(SYS_read, fd, (long)buf, (long)count);
}

int open(const char *path, int flags, ...)
{
    return (int)__syscall3(SYS_open, (long)path, flags, 0);
}

int close(int fd)
{
    return (int)__syscall1(SYS_close, fd);
}

long lseek(int fd, long offset, int whence)
{
    return __syscall3(SYS_lseek, fd, offset, whence);
}

int pipe(int pipefd[2])
{
    return (int)__syscall1(SYS_pipe, (long)pipefd);
}

int dup2(int oldfd, int newfd)
{
    return (int)__syscall2(SYS_dup2, oldfd, newfd);
}

int kill(int pid, int sig)
{
    return (int)__syscall2(SYS_kill, pid, sig);
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
    if (ret < 0) return MAP_FAILED;
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    return (int)__syscall2(SYS_munmap, (long)addr, (long)length);
}

int mprotect(void *addr, size_t length, int prot)
{
    return (int)__syscall3(SYS_mprotect, (long)addr, (long)length, (long)prot);
}

/* ── brk-based malloc (bump allocator) ── */

static char *heap_cur;
static char *heap_end;

static long sys_brk(long addr)
{
    return __syscall1(SYS_brk, addr);
}

void *malloc(size_t size)
{
    if (!heap_cur) {
        heap_cur = (char *)sys_brk(0);
        heap_end = heap_cur;
    }

    /* Align to 16 bytes */
    size = (size + 15) & ~15UL;

    char *new_end = heap_cur + size;
    if (new_end > heap_end) {
        /* Grow heap in 4KB increments */
        long grow = (long)(new_end - heap_end + 4095) & ~4095L;
        long result = sys_brk((long)(heap_end + grow));
        if (result < (long)(heap_end + grow))
            return (void *)0;
        heap_end = (char *)result;
    }

    char *ptr = heap_cur;
    heap_cur = new_end;
    return ptr;
}

void free(void *ptr)
{
    (void)ptr;  /* bump allocator — no free */
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
    char *d = (char *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
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

int printf(const char *fmt, ...)
{
    /* Minimal: supports %s, %d, %ld, %x, %lx, %c, %%, %p, %u, %lu */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

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
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            puts_fd(1, s);
            count += (int)strlen(s);
            break;
        }
        case 'd': {
            long v = is_long ? __builtin_va_arg(ap, long)
                             : (long)__builtin_va_arg(ap, int);
            putdec_fd(1, v);
            count++;
            break;
        }
        case 'u': {
            unsigned long v = is_long ? __builtin_va_arg(ap, unsigned long)
                                      : (unsigned long)__builtin_va_arg(ap, unsigned int);
            putdec_fd(1, (long)v);
            count++;
            break;
        }
        case 'x': {
            unsigned long v = is_long ? __builtin_va_arg(ap, unsigned long)
                                      : (unsigned long)__builtin_va_arg(ap, unsigned int);
            puthex_fd(1, v);
            count++;
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)__builtin_va_arg(ap, void *);
            puts_fd(1, "0x");
            puthex_fd(1, v);
            count += 3;
            break;
        }
        case 'c': {
            int c = __builtin_va_arg(ap, int);
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

    __builtin_va_end(ap);
    return count;
}

int puts(const char *s)
{
    puts_fd(1, s);
    putchar_fd(1, '\n');
    return 0;
}

/* _start is in syscall.S — reads argc/argv before C prologue corrupts RSP */
