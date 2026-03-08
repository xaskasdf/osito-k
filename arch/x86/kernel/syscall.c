/*
 * OsitoK x86-64 — Syscall Interface
 *
 * X-OS4/X-OS10: Fast syscall via SYSCALL/SYSRET (MSR-based).
 * ABI: RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args.
 * Return value in RAX. Error: negative errno.
 *
 * Syscall entry point is in assembly (syscall_entry.S), which
 * saves registers and calls syscall_dispatch() here.
 *
 * Syscalls (Linux-compatible numbers):
 *   0-3,5,8-12  = read/write/open/close/fstat/lseek/mmap/mprotect/munmap/brk
 *  13-15        = rt_sigaction/rt_sigprocmask/rt_sigreturn
 *  16-21        = ioctl/pread64/pwrite64/writev/access
 *  22,24,32,33  = pipe/sched_yield/dup/dup2
 *  35,39        = nanosleep/getpid
 *  56-60,62     = clone/fork/execve/exit/kill   [clone/fork/execve stubs]
 *  72,74,79     = fcntl/fsync/getcwd
 *  87,89        = unlink/readlink
 * 131,158       = sigaltstack/arch_prctl(ARCH_SET_FS)
 * 186,202       = gettid/futex
 * 217-218       = getdents64/set_tid_address
 * 228,231,234   = clock_gettime/exit_group/tgkill
 * 262,273       = newfstatat/set_robust_list
 * 302,318       = prlimit64/getrandom
 */

#include "../include/types.h"

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void serial_putc(char c);

extern void fb_puts(const char *s);
extern void fb_putc(char c, uint32_t color);
extern void fb_putdec(uint64_t val);

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Timer */
extern uint64_t idt_get_ticks(void);

/* Physical memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Paging */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
extern int paging_set_flags(uint64_t virt, uint64_t flags);

/* ── MSR definitions ─────────────────────────────────────────── */

#define MSR_FS_BASE 0xC0000100  /* FS segment base (for TLS) */

#define MSR_STAR    0xC0000081  /* Segment selectors for SYSCALL/SYSRET */
#define MSR_LSTAR   0xC0000082  /* RIP for SYSCALL (64-bit) */
#define MSR_CSTAR   0xC0000083  /* RIP for SYSCALL (compat, unused) */
#define MSR_FMASK   0xC0000084  /* RFLAGS mask on SYSCALL */
#define MSR_EFER    0xC0000080  /* Extended Feature Enable Register */

#define EFER_SCE    (1ULL << 0)  /* SYSCALL Enable */

/* ── MSR helpers ─────────────────────────────────────────────── */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" : : "c"(msr),
                      "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── Syscall numbers (Linux-compatible subset) ───────────────── */

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_FSTAT       5
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MPROTECT    10
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_IOCTL       16
#define SYS_WRITEV      20
#define SYS_ACCESS      21
#define SYS_PIPE        22
#define SYS_DUP2        33
#define SYS_KILL        62
#define SYS_EXIT        60
#define SYS_UNLINK      87
#define SYS_GETCWD      79
#define SYS_READLINK    89
#define SYS_GETDENTS64  217
#define SYS_ARCH_PRCTL  158
#define SYS_SIGACTION   13
#define SYS_SIGPROCMASK 14
#define SYS_SIGRETURN   15
#define SYS_PREAD64     17
#define SYS_PWRITE64    18
#define SYS_SCHED_YIELD 24
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_CLONE       56
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_FCNTL       72
#define SYS_FSYNC       74
#define SYS_SIGALTSTACK 131
#define SYS_GETTID      186
#define SYS_FUTEX       202
#define SYS_SET_TID_ADDR 218
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP  231
#define SYS_TGKILL      234
#define SYS_NEWFSTATAT  262
#define SYS_SET_ROBUST_LIST 273
#define SYS_SENDFILE    40
#define SYS_WAIT4       61
#define SYS_UNAME       63
#define SYS_GETUID      102
#define SYS_GETGID      104
#define SYS_SETUID      105
#define SYS_SETGID      106
#define SYS_GETEUID     107
#define SYS_GETEGID     108
#define SYS_GETPPID     110
#define SYS_GETPGRP     111
#define SYS_SETSID      112
#define SYS_GETGROUPS   115
#define SYS_PRCTL       157
#define SYS_OPENAT      257
#define SYS_READLINKAT  267
#define SYS_DUP3        292
#define SYS_PIPE2       293
#define SYS_PRLIMIT64   302
#define SYS_GETRANDOM   318
#define SYS_RSEQ        334
#define SYS_CLOSE_RANGE 436
#define SYS_MADVISE     28
#define SYS_STAT        4
#define SYS_LSTAT       6

/* errno values */
#define EPERM    1
#define ENOSYS  38
#define EBADF    9
#define EFAULT  14
#define EINVAL  22
#define ENOENT   2
#define EMFILE  24
#define ENOMEM  12
#define EISDIR  21
#define ESPIPE  29
#define ENOTTY  25
#define EPIPE   32
#define ESRCH    3
#define EAGAIN  11
#define ENOTDIR 20
#define ENOSPC  28
#define ERANGE  34
#define ENOTSUP 95
#define EAFNOSUPPORT 97

/* open flags (Linux values) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_ACCMODE   0x0003

/* lseek whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* ── File descriptor table ───────────────────────────────────── */

#define MAX_FDS 16

#define FD_TYPE_CONSOLE 1
#define FD_TYPE_FILE    2
#define FD_TYPE_PIPE    3
#define FD_TYPE_DEV     4   /* virtual device (/dev/null, /dev/zero, etc.) */
#define FD_TYPE_PROC    5   /* virtual procfs (/proc/self/maps, etc.) */

typedef ssize_t (*fd_write_fn)(const void *buf, size_t count);
typedef ssize_t (*fd_read_fn)(void *buf, size_t count);

/* osfs2_file_t is opaque here — we get size via osfs2_file_size() */
extern uint64_t osfs2_file_size(void *file);

typedef struct {
    bool        open;
    uint8_t     type;       /* FD_TYPE_* */
    uint16_t    oflags;     /* O_RDONLY, O_WRONLY, O_RDWR */
    fd_read_fn  read;       /* console read callback */
    fd_write_fn write;      /* console write callback */
    void       *file;       /* osfs2_file_t * for FD_TYPE_FILE */
    uint64_t    offset;     /* current file position */
} fd_entry_t;

static fd_entry_t fd_table[MAX_FDS];

/* ── Pipe buffers ───────────────────────────────────────────── */

#define PIPE_BUF_SIZE   4096
#define MAX_PIPES       8

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;          /* write position */
    uint32_t tail;          /* read position */
    uint32_t count;         /* bytes in buffer */
    bool     write_open;    /* write end still open */
    bool     read_open;     /* read end still open */
    bool     in_use;
} pipe_buf_t;

static pipe_buf_t pipes[MAX_PIPES];

/* ── Signal state ───────────────────────────────────────────── */

#define NSIG        32
#define SIGINT       2
#define SIGPIPE     13
#define SIGTERM     15
#define SIGKILL      9

#define SIG_DFL     ((uint64_t)0)
#define SIG_IGN     ((uint64_t)1)

static uint64_t sig_handlers[NSIG];     /* handler addresses (SIG_DFL/SIG_IGN/fn) */
static uint32_t sig_pending;            /* bitmask of pending signals */

/* ── Output capture (X-CL4: tool exec) ──────────────────────── */

static char    *capture_buf;
static uint32_t capture_pos;
static uint32_t capture_max;

void syscall_capture_start(char *buf, uint32_t max_len)
{
    capture_buf = buf;
    capture_pos = 0;
    capture_max = max_len;
}

uint32_t syscall_capture_stop(void)
{
    uint32_t len = capture_pos;
    if (capture_buf && capture_pos < capture_max)
        capture_buf[capture_pos] = '\0';
    capture_buf = NULL;
    capture_pos = 0;
    capture_max = 0;
    return len;
}

/* stdout/stderr → serial + framebuffer (+ optional capture) */
static ssize_t console_write(const void *buf, size_t count)
{
    const char *s = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        serial_putc(s[i]);
        fb_putc(s[i], 0x00CCCCCC);
        if (capture_buf && capture_pos < capture_max - 1)
            capture_buf[capture_pos++] = s[i];
    }
    return (ssize_t)count;
}

/* stdin → stub (returns 0 = EOF for now, keyboard driver will fix) */
static ssize_t console_read(void *buf, size_t count)
{
    (void)buf;
    (void)count;
    return 0;  /* EOF — no keyboard yet */
}

/* ── brk state (process heap) ────────────────────────────────── */

#define BRK_HEAP_SIZE  (4ULL * 1024 * 1024)  /* 4MB process heap */

static uint8_t *brk_base;      /* start of brk region */
static uint8_t *brk_current;   /* current break */
static uint8_t *brk_max;       /* end of brk region */

/* ── mmap/VFS shared definitions ───────────────────────────────── */

/* mmap flags (Linux values) */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS

/* Page table flags */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

#define MAP_FAILED      ((uint64_t)-1)

/* VMA tracking — per-process mmap regions */
#define MAX_VMAS        64

typedef struct {
    uint64_t base;      /* virtual (== physical, identity-mapped) */
    uint64_t pages;     /* number of 4KB pages */
    uint32_t prot;      /* PROT_READ|PROT_WRITE|PROT_EXEC */
    bool     in_use;
} vma_t;

static vma_t vma_table[MAX_VMAS];

/* ── VFS device/proc forward declarations (X-VFS) ─────────────── */

/* Device IDs for virtual /dev entries */
#define DEV_NULL        0
#define DEV_ZERO        1
#define DEV_URANDOM     2
#define DEV_CONSOLE     3

/* RDTSC-based PRNG for /dev/urandom */
static uint64_t urandom_state;

static uint64_t urandom_next(void)
{
    if (!urandom_state) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        urandom_state = ((uint64_t)hi << 32) | lo;
    }
    urandom_state ^= urandom_state << 13;
    urandom_state ^= urandom_state >> 7;
    urandom_state ^= urandom_state << 17;
    return urandom_state;
}

/* Procfs content buffer — generated on open, read via offset */
#define PROC_BUF_SIZE  4096
static char  proc_buf[PROC_BUF_SIZE];
static int   proc_buf_len;

/* ── Syscall handlers ────────────────────────────────────────── */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!buf && count > 0) return -EFAULT;

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_DEV) {
        int dev_id = (int)f->offset;
        switch (dev_id) {
        case DEV_NULL:
            return (int64_t)count;  /* discard */
        case DEV_CONSOLE:
            return console_write((const void *)buf, (size_t)count);
        default:
            return -EBADF;  /* zero/urandom are read-only */
        }
    }

    if (f->type == FD_TYPE_PROC)
        return -EBADF;  /* procfs is read-only */

    if (f->type == FD_TYPE_FILE) {
        if ((f->oflags & O_ACCMODE) == O_RDONLY) return -EBADF;
        int ret = osfs2_write(f->file, f->offset, (const void *)buf, count);
        if (ret < 0) return -EFAULT;
        f->offset += count;
        return (int64_t)count;
    }

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *p = (pipe_buf_t *)f->file;
        if (!p || !p->read_open) return -EPIPE;
        const uint8_t *src = (const uint8_t *)buf;
        uint64_t written = 0;
        while (written < count) {
            if (p->count >= PIPE_BUF_SIZE) {
                /* Buffer full — return what we have (non-blocking) */
                if (written > 0) return (int64_t)written;
                return -EAGAIN;
            }
            p->buf[p->head] = src[written++];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        return (int64_t)written;
    }

    /* Console */
    if (!f->write) return -EBADF;
    return f->write((const void *)buf, (size_t)count);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!buf && count > 0) return -EFAULT;

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_DEV) {
        int dev_id = (int)f->offset;
        switch (dev_id) {
        case DEV_NULL:
            return 0;  /* always EOF */
        case DEV_ZERO: {
            memset((void *)buf, 0, (size_t)count);
            return (int64_t)count;
        }
        case DEV_URANDOM: {
            uint8_t *dst = (uint8_t *)buf;
            for (uint64_t i = 0; i < count; i += 8) {
                uint64_t r = urandom_next();
                uint64_t n = count - i;
                if (n > 8) n = 8;
                memcpy(dst + i, &r, (size_t)n);
            }
            return (int64_t)count;
        }
        case DEV_CONSOLE:
            return console_read((void *)buf, (size_t)count);
        default:
            return -EBADF;
        }
    }

    if (f->type == FD_TYPE_PROC) {
        /* Read from generated proc_buf */
        uint64_t off = f->offset;
        if (off >= (uint64_t)proc_buf_len) return 0;  /* EOF */
        uint64_t avail = (uint64_t)proc_buf_len - off;
        if (count > avail) count = avail;
        memcpy((void *)buf, proc_buf + off, (size_t)count);
        f->offset += count;
        return (int64_t)count;
    }

    if (f->type == FD_TYPE_FILE) {
        if ((f->oflags & O_ACCMODE) == O_WRONLY) return -EBADF;
        uint64_t file_size = osfs2_file_size(f->file);
        if (f->offset >= file_size) return 0;  /* EOF */
        uint64_t avail = file_size - f->offset;
        if (count > avail) count = avail;
        int ret = osfs2_read(f->file, f->offset, (void *)buf, count);
        if (ret < 0) return -EFAULT;
        f->offset += count;
        return (int64_t)count;
    }

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *p = (pipe_buf_t *)f->file;
        if (!p) return -EBADF;
        if (p->count == 0) {
            /* Empty — if write end is closed, return EOF */
            if (!p->write_open) return 0;
            return -EAGAIN;
        }
        uint8_t *dst = (uint8_t *)buf;
        uint64_t nread = 0;
        while (nread < count && p->count > 0) {
            dst[nread++] = p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            p->count--;
        }
        return (int64_t)nread;
    }

    /* Console */
    if (!f->read) return -EBADF;
    return f->read((void *)buf, (size_t)count);
}

/* ── VFS: Virtual Filesystem Layer (X-VFS) ────────────────────── */

/* Proc IDs for procfs entries */
#define PROC_MAPS       0
#define PROC_STATUS     1

/* Generate /proc/self/maps content into buffer */
static int proc_gen_maps(char *buf, int max)
{
    int pos = 0;
    /* List mmap VMAs */
    for (int i = 0; i < MAX_VMAS && pos < max - 80; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t start = vma_table[i].base;
        uint64_t end = start + vma_table[i].pages * 4096;
        uint32_t p = vma_table[i].prot;
        /* Format: start-end rwxp offset dev inode pathname */
        /* Simple hex formatter inline */
        char line[80];
        int lp = 0;
        /* start address */
        for (int d = 60; d >= 0; d -= 4) {
            int nib = (start >> d) & 0xF;
            if (nib || lp > 0 || d == 0)
                line[lp++] = "0123456789abcdef"[nib];
        }
        line[lp++] = '-';
        /* end address */
        for (int d = 60; d >= 0; d -= 4) {
            int nib = (end >> d) & 0xF;
            if (nib || lp > (int)(line + lp - line) || d == 0) /* always print at least one digit */
                line[lp++] = "0123456789abcdef"[nib];
        }
        line[lp++] = ' ';
        line[lp++] = (p & PROT_READ)  ? 'r' : '-';
        line[lp++] = (p & PROT_WRITE) ? 'w' : '-';
        line[lp++] = (p & PROT_EXEC)  ? 'x' : '-';
        line[lp++] = 'p';
        line[lp++] = ' ';
        /* offset + dev + inode: all zeros */
        for (int z = 0; z < 8; z++) line[lp++] = '0';
        line[lp++] = ' ';
        line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ':';
        line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ' ';
        line[lp++] = '0';
        line[lp++] = '\n';
        line[lp] = 0;
        /* Copy to output */
        for (int c = 0; c < lp && pos < max - 1; c++)
            buf[pos++] = line[c];
    }
    buf[pos] = 0;
    return pos;
}

/* Generate /proc/self/status content into buffer */
extern int32_t proc_current_pid(void);
extern const char *proc_current_name(void);

static int proc_gen_status(char *buf, int max)
{
    int pos = 0;
    const char *name = proc_current_name();
    int32_t pid = proc_current_pid();

    /* Name: */
    const char *s = "Name:\t";
    while (*s && pos < max - 1) buf[pos++] = *s++;
    if (name) while (*name && pos < max - 1) buf[pos++] = *name++;
    if (pos < max - 1) buf[pos++] = '\n';

    /* Pid: */
    s = "Pid:\t";
    while (*s && pos < max - 1) buf[pos++] = *s++;
    /* Simple decimal */
    char digits[12];
    int nd = 0;
    int32_t val = pid;
    if (val == 0) { digits[nd++] = '0'; }
    else { while (val > 0) { digits[nd++] = '0' + (val % 10); val /= 10; } }
    for (int d = nd - 1; d >= 0; d--)
        if (pos < max - 1) buf[pos++] = digits[d];
    if (pos < max - 1) buf[pos++] = '\n';

    /* State: */
    s = "State:\tR (running)\n";
    while (*s && pos < max - 1) buf[pos++] = *s++;

    buf[pos] = 0;
    return pos;
}

/* Helper: simple prefix match */
static bool str_startswith(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static int vfs_alloc_fd(void)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (!fd_table[i].open) return i;
    return -1;
}

static int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode)
{
    (void)mode;
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;

    int newfd = vfs_alloc_fd();
    if (newfd < 0) return -EMFILE;

    /* ── VFS: virtual /dev devices ─────────────────────── */

    if (str_startswith(path, "/dev/")) {
        const char *devname = path + 5;
        int dev_id = -1;

        if (strcmp(devname, "null") == 0)        dev_id = DEV_NULL;
        else if (strcmp(devname, "zero") == 0)    dev_id = DEV_ZERO;
        else if (strcmp(devname, "urandom") == 0) dev_id = DEV_URANDOM;
        else if (strcmp(devname, "random") == 0)  dev_id = DEV_URANDOM;
        else if (strcmp(devname, "console") == 0) dev_id = DEV_CONSOLE;
        else if (strcmp(devname, "tty") == 0)     dev_id = DEV_CONSOLE;
        else return -ENOENT;

        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_DEV;
        f->oflags = (uint16_t)(flags & 0xFFFF);
        f->offset = (uint64_t)dev_id;  /* store device ID in offset field */
        return newfd;
    }

    /* ── VFS: virtual /proc entries ──────────────────────── */

    if (str_startswith(path, "/proc/self/") || str_startswith(path, "/proc/")) {
        const char *entry = path;
        /* Skip /proc/self/ or /proc/<pid>/ */
        if (str_startswith(path, "/proc/self/"))
            entry = path + 11;
        else {
            /* /proc/<digits>/ — skip digits */
            entry = path + 6;
            while (*entry >= '0' && *entry <= '9') entry++;
            if (*entry == '/') entry++;
        }

        int proc_id = -1;
        if (strcmp(entry, "maps") == 0) proc_id = PROC_MAPS;
        else if (strcmp(entry, "status") == 0) proc_id = PROC_STATUS;
        else return -ENOENT;

        /* Generate content on open */
        if (proc_id == PROC_MAPS)
            proc_buf_len = proc_gen_maps(proc_buf, PROC_BUF_SIZE);
        else
            proc_buf_len = proc_gen_status(proc_buf, PROC_BUF_SIZE);

        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_PROC;
        f->oflags = O_RDONLY;
        f->offset = 0;  /* read position */
        return newfd;
    }

    /* ── OsitoFS: regular files ─────────────────────────── */

    void *file = osfs2_find(path);

    /* Strip leading "/" for OsitoFS lookup if not found */
    if (!file && path[0] == '/')
        file = osfs2_find(path + 1);

    if (!file && (flags & O_CREAT)) {
        file = osfs2_create(path, 0);
    }

    if (!file) return -ENOENT;

    fd_entry_t *f = &fd_table[newfd];
    memset(f, 0, sizeof(*f));
    f->open   = true;
    f->type   = FD_TYPE_FILE;
    f->oflags = (uint16_t)(flags & 0xFFFF);
    f->file   = file;
    f->offset = 0;

    if (flags & O_APPEND)
        f->offset = osfs2_file_size(file);

    if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        f->offset = 0;
    }

    return newfd;
}

static int64_t sys_close(uint64_t fd)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_PIPE && f->file) {
        pipe_buf_t *p = (pipe_buf_t *)f->file;
        /* Determine if this is read or write end via oflags */
        if ((f->oflags & O_ACCMODE) == O_RDONLY)
            p->read_open = false;
        else
            p->write_open = false;
        /* Free pipe when both ends closed */
        if (!p->read_open && !p->write_open)
            p->in_use = false;
    }

    f->open = false;
    f->file = NULL;
    return 0;
}

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -ESPIPE;

    int64_t new_off;
    uint64_t file_size = osfs2_file_size(f->file);

    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (int64_t)f->offset + offset; break;
    case SEEK_END: new_off = (int64_t)file_size + offset; break;
    default: return -EINVAL;
    }

    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

/* Minimal stat structure (Linux x86-64 layout, fields we populate) */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t _pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  _unused[3];
} linux_stat_t;

static int64_t sys_fstat(uint64_t fd, uint64_t statbuf_addr)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!statbuf_addr) return -EFAULT;

    linux_stat_t *st = (linux_stat_t *)statbuf_addr;
    memset(st, 0, sizeof(*st));

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_FILE) {
        st->st_mode = 0100644;  /* S_IFREG | 0644 */
        st->st_size = (int64_t)osfs2_file_size(f->file);
        st->st_blksize = 4096;
        st->st_blocks = (st->st_size + 511) / 512;
        st->st_nlink = 1;
    } else if (f->type == FD_TYPE_DEV) {
        st->st_mode = 0020666;  /* S_IFCHR | 0666 */
        int dev_id = (int)f->offset;
        if (dev_id == DEV_NULL)    st->st_rdev = 0x0103;  /* 1,3 */
        else if (dev_id == DEV_ZERO) st->st_rdev = 0x0105; /* 1,5 */
        else if (dev_id == DEV_URANDOM) st->st_rdev = 0x0109; /* 1,9 */
        else st->st_rdev = 0x0501;  /* /dev/console = 5,1 */
        st->st_nlink = 1;
        st->st_blksize = 4096;
    } else if (f->type == FD_TYPE_PROC) {
        st->st_mode = 0100444;  /* S_IFREG | 0444 (read-only) */
        st->st_size = (int64_t)proc_buf_len;
        st->st_blksize = 4096;
        st->st_nlink = 1;
    } else {
        /* Console device */
        st->st_mode = 0020666;  /* S_IFCHR | 0666 */
        st->st_rdev = 0x0501;   /* /dev/tty */
        st->st_nlink = 1;
        st->st_blksize = 1024;
    }

    return 0;
}

extern void proc_exit(int32_t code);

static int64_t sys_exit(uint64_t status)
{
    /* Return to kernel via proc_exit (longjmp to proc_exec) */
    proc_exit((int32_t)status);

    /* unreachable — proc_exit never returns */
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* brk — manage per-process heap region.
 * brk(0) returns current break.
 * brk(addr) sets break to addr if within region. */
static int64_t sys_brk(uint64_t addr)
{
    /* Lazy init: allocate brk region on first call */
    if (!brk_base) {
        brk_base = (uint8_t *)kmalloc(BRK_HEAP_SIZE);
        if (!brk_base) return 0;
        brk_current = brk_base;
        brk_max = brk_base + BRK_HEAP_SIZE;
    }

    if (addr == 0)
        return (int64_t)(uint64_t)brk_current;

    uint8_t *new_brk = (uint8_t *)addr;

    if (new_brk >= brk_base && new_brk <= brk_max) {
        /* Zero newly exposed memory */
        if (new_brk > brk_current)
            memset(brk_current, 0, (uint64_t)(new_brk - brk_current));
        brk_current = new_brk;
    }

    return (int64_t)(uint64_t)brk_current;
}

/* ── mmap/munmap/mprotect (X-MMAP) ────────────────────────────── */

static uint64_t prot_to_pte_flags(uint32_t prot)
{
    uint64_t flags = PTE_PRESENT | PTE_GLOBAL;
    if (prot & PROT_WRITE)
        flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC))
        flags |= PTE_NX;
    return flags;
}

/*
 * sys_mmap — MAP_ANONYMOUS only, identity-mapped.
 * Allocates contiguous physical pages and returns phys addr (== virt addr).
 * Linux ABI: mmap(addr, length, prot, flags, fd, offset)
 *   args: a1=addr, a2=length, a3=prot, a4=flags, a5(R8)=fd, a6(R9)=offset
 *   Note: R10 carries flags (a4 in our dispatch), fd is a5, offset is unused.
 */
static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)addr;    /* MAP_FIXED not supported yet */
    (void)offset;

    /* Only support anonymous private mappings */
    if (!(flags & MAP_ANONYMOUS))
        return -ENOSYS;  /* No file-backed mmap */
    if (fd != (uint64_t)-1 && !(flags & MAP_ANONYMOUS))
        return -EBADF;

    if (length == 0) return -EINVAL;

    /* Round up to page boundary */
    uint64_t npages = (length + 4095) / 4096;

    /* Find free VMA slot */
    int vi = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) { vi = i; break; }
    }
    if (vi < 0) return -ENOMEM;

    /* Allocate physical pages */
    void *pages = mem_alloc_pages(npages);
    if (!pages) return -ENOMEM;

    uint64_t base = (uint64_t)pages;

    /* Pages are already identity-mapped from paging_init (first 4GB at least).
     * For pages above the initial identity map range, we'd need to map them.
     * For now, paging_init maps all usable RAM, so allocated pages are mapped. */

    /* Zero the memory (MAP_ANONYMOUS guarantees zeroed pages) */
    memset(pages, 0, npages * 4096);

    /* Track the VMA */
    vma_table[vi].base   = base;
    vma_table[vi].pages  = npages;
    vma_table[vi].prot   = (uint32_t)prot;
    vma_table[vi].in_use = true;

    return (int64_t)base;
}

/* sys_munmap — unmap pages allocated by mmap */
static int64_t sys_munmap(uint64_t addr, uint64_t length)
{
    if (!addr || (addr & 0xFFF)) return -EINVAL;  /* must be page-aligned */
    if (length == 0) return -EINVAL;

    uint64_t npages = (length + 4095) / 4096;

    /* Find matching VMA */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (vma_table[i].base == addr && vma_table[i].pages == npages) {
            /* Free physical pages */
            mem_free_pages((void *)addr, npages);
            vma_table[i].in_use = false;
            return 0;
        }
    }

    /* Partial unmap: find VMA containing this range */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096;
        if (addr >= vma_table[i].base && addr + npages * 4096 <= vma_end) {
            /* For simplicity, free the pages and mark VMA as unused.
             * Full partial-unmap (splitting VMAs) not needed yet. */
            mem_free_pages((void *)addr, npages);
            vma_table[i].in_use = false;
            return 0;
        }
    }

    return -EINVAL;
}

/* sys_mprotect — change protection flags on mapped pages */
static int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    if (!addr || (addr & 0xFFF)) return -EINVAL;
    if (length == 0) return -EINVAL;

    uint64_t npages = (length + 4095) / 4096;

    /* Find VMA containing this range */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096;
        if (addr >= vma_table[i].base && addr + npages * 4096 <= vma_end) {
            uint64_t pte_flags = prot_to_pte_flags((uint32_t)prot);

            /* Update page table entries */
            for (uint64_t p = 0; p < npages; p++) {
                uint64_t va = addr + p * 4096;
                paging_set_flags(va, pte_flags);
            }

            /* Update VMA prot */
            vma_table[i].prot = (uint32_t)prot;
            return 0;
        }
    }

    /* If addr is in identity-mapped region (not from mmap),
     * still allow mprotect as a no-op for compatibility */
    return 0;
}

/* writev — gather write (used by printf/puts in newlib) */
typedef struct {
    uint64_t iov_base;
    uint64_t iov_len;
} iovec_t;

static int64_t sys_writev(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!iov_addr || iovcnt == 0) return 0;

    const iovec_t *iov = (const iovec_t *)iov_addr;
    int64_t total = 0;

    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t ret = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (ret < 0) return (total > 0) ? total : ret;
        total += ret;
    }

    return total;
}

/* ioctl stub — returns ENOTTY for everything */
static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    (void)fd; (void)request; (void)arg;
    return -ENOTTY;
}

/* access — check if file exists */
extern int osfs2_delete(const char *name);

static int64_t sys_access(uint64_t path_addr, uint64_t mode)
{
    (void)mode;
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;
    return osfs2_find(path) ? 0 : -ENOENT;
}

/* unlink — delete file from OsitoFS */
static int64_t sys_unlink(uint64_t path_addr)
{
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;
    return osfs2_delete(path) == 0 ? 0 : -ENOENT;
}

/* ── pipe(pipefd[2]) — create pipe ──────────────────────────── */

static int64_t sys_pipe(uint64_t pipefd_addr)
{
    if (!pipefd_addr) return -EFAULT;
    int *pipefd = (int *)pipefd_addr;

    /* Find free pipe buffer */
    int pi = -1;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) { pi = i; break; }
    }
    if (pi < 0) return -EMFILE;

    /* Find two free FDs */
    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_table[i].open) {
            if (rfd < 0) rfd = i;
            else if (wfd < 0) { wfd = i; break; }
        }
    }
    if (rfd < 0 || wfd < 0) return -EMFILE;

    /* Initialize pipe buffer */
    pipe_buf_t *p = &pipes[pi];
    memset(p, 0, sizeof(*p));
    p->in_use = true;
    p->read_open = true;
    p->write_open = true;

    /* Read end */
    fd_entry_t *rf = &fd_table[rfd];
    memset(rf, 0, sizeof(*rf));
    rf->open   = true;
    rf->type   = FD_TYPE_PIPE;
    rf->oflags = O_RDONLY;
    rf->file   = p;

    /* Write end */
    fd_entry_t *wf = &fd_table[wfd];
    memset(wf, 0, sizeof(*wf));
    wf->open   = true;
    wf->type   = FD_TYPE_PIPE;
    wf->oflags = O_WRONLY;
    wf->file   = p;

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

/* ── dup2(oldfd, newfd) — duplicate file descriptor ────────── */

static int64_t sys_dup2(uint64_t oldfd, uint64_t newfd)
{
    if (oldfd >= MAX_FDS || !fd_table[oldfd].open) return -EBADF;
    if (newfd >= MAX_FDS) return -EBADF;

    if (oldfd == newfd) return (int64_t)newfd;

    /* Close newfd if open */
    if (fd_table[newfd].open)
        sys_close(newfd);

    /* Copy fd entry */
    fd_table[newfd] = fd_table[oldfd];

    /* For pipes, both ends now reference same buffer */
    /* No refcount needed — pipe_buf tracks read_open/write_open */

    return (int64_t)newfd;
}

/* ── kill(pid, sig) — send signal ──────────────────────────── */

extern int32_t proc_current_pid(void);
extern int32_t proc_current_ppid(void);

static int64_t sys_kill(uint64_t pid, uint64_t sig)
{
    if (sig >= NSIG) return -EINVAL;

    int32_t cur_pid = proc_current_pid();

    /* Can only signal self or pid 0 (current process group) */
    if (pid != 0 && (int64_t)pid != cur_pid)
        return -ESRCH;

    if (sig == SIGKILL || sig == SIGTERM) {
        proc_exit(128 + (int32_t)sig);
        /* unreachable */
    }

    if (sig == 0) return 0;  /* Signal 0 = test if process exists */

    /* Queue signal for delivery */
    sig_pending |= (1U << sig);

    return 0;
}

/* ── sigaction(sig, act, oldact) — install signal handler ──── */

typedef struct {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
} sigaction_t;

static int64_t sys_sigaction(uint64_t sig, uint64_t act_addr, uint64_t oldact_addr)
{
    if (sig >= NSIG || sig == SIGKILL) return -EINVAL;

    if (oldact_addr) {
        sigaction_t *old = (sigaction_t *)oldact_addr;
        memset(old, 0, sizeof(*old));
        old->sa_handler = sig_handlers[sig];
    }

    if (act_addr) {
        const sigaction_t *act = (const sigaction_t *)act_addr;
        sig_handlers[sig] = act->sa_handler;
    }

    return 0;
}

/* ── Check and deliver pending signals ─────────────────────── */

void syscall_check_signals(void)
{
    if (!sig_pending) return;

    for (uint32_t s = 1; s < NSIG; s++) {
        if (!(sig_pending & (1U << s))) continue;
        sig_pending &= ~(1U << s);

        uint64_t handler = sig_handlers[s];

        if (handler == SIG_IGN) continue;

        if (handler == SIG_DFL) {
            /* Default action for most signals: terminate */
            if (s == SIGINT || s == SIGTERM || s == SIGPIPE) {
                serial_puts("[SIGNAL] Delivering signal ");
                serial_putdec(s);
                serial_puts(" (default: terminate)\n");
                proc_exit(128 + (int32_t)s);
            }
            continue;
        }

        /* Custom handler — call it (simple synchronous delivery) */
        void (*fn)(int) = (void (*)(int))handler;
        fn((int)s);
    }
}

/* ── New syscalls for musl libc (X-MUSL) ─────────────────────── */

/* arch_prctl — set/get FS/GS base (TLS support) */
#define ARCH_SET_FS  0x1002
#define ARCH_GET_FS  0x1003
#define ARCH_SET_GS  0x1001
#define ARCH_GET_GS  0x1004

static int64_t sys_arch_prctl(uint64_t code, uint64_t addr)
{
    switch (code) {
    case ARCH_SET_FS:
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!addr) return -EFAULT;
        *(uint64_t *)addr = rdmsr(MSR_FS_BASE);
        return 0;
    case ARCH_SET_GS:
    case ARCH_GET_GS:
        return -ENOSYS;  /* GS not needed for musl */
    default:
        return -EINVAL;
    }
}

/* set_tid_address — set pointer for child tid notification */
static int64_t sys_set_tid_address(uint64_t tidptr)
{
    (void)tidptr;  /* We don't implement CLONE_CHILD_CLEARTID yet */
    return (int64_t)proc_current_pid();  /* Return current TID */
}

/* getpid / gettid — return process/thread ID */
static int64_t sys_getpid(void)
{
    return (int64_t)proc_current_pid();
}

static int64_t sys_gettid(void)
{
    return (int64_t)proc_current_pid();  /* TID = PID (no threads yet) */
}

/* rt_sigprocmask — block/unblock signals (minimal stub) */
static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_addr,
                                   uint64_t oldset_addr, uint64_t sigsetsize)
{
    (void)how; (void)sigsetsize;

    /* Return old mask if requested */
    if (oldset_addr) {
        uint64_t *oldset = (uint64_t *)oldset_addr;
        *oldset = 0;  /* No signals blocked */
    }

    /* Accept but ignore the new mask for now */
    (void)set_addr;
    return 0;
}

/* sigaltstack — set alternate signal stack (stub) */
static int64_t sys_sigaltstack(uint64_t ss_addr, uint64_t old_ss_addr)
{
    if (old_ss_addr) {
        /* Return "no alternate stack" */
        memset((void *)old_ss_addr, 0, 24);  /* ss_sp, ss_flags=SS_DISABLE, ss_size */
        *(int *)((uint8_t *)old_ss_addr + 8) = 2;  /* SS_DISABLE */
    }
    (void)ss_addr;
    return 0;
}

/* exit_group — terminate all threads (alias to exit for now) */
static int64_t sys_exit_group(uint64_t status)
{
    return sys_exit(status);
}

/* clock_gettime — return monotonic/realtime clock */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_t;

static int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp_addr)
{
    if (!tp_addr) return -EFAULT;
    timespec_t *tp = (timespec_t *)tp_addr;

    /* Use APIC ticks (100Hz) for time base */
    uint64_t ticks = idt_get_ticks();
    uint64_t ms = ticks * 10;  /* 100Hz → 10ms per tick */

    tp->tv_sec  = (int64_t)(ms / 1000);
    tp->tv_nsec = (int64_t)((ms % 1000) * 1000000);

    (void)clk_id;  /* Same time for REALTIME and MONOTONIC */
    return 0;
}

/* nanosleep — sleep for specified time */
static int64_t sys_nanosleep(uint64_t req_addr, uint64_t rem_addr)
{
    if (!req_addr) return -EFAULT;
    const timespec_t *req = (const timespec_t *)req_addr;

    /* Convert to APIC ticks (100Hz = 10ms per tick) */
    uint64_t ms = (uint64_t)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
    uint64_t sleep_ticks = (ms + 9) / 10;  /* round up */
    if (sleep_ticks == 0) sleep_ticks = 1;

    uint64_t deadline = idt_get_ticks() + sleep_ticks;
    while (idt_get_ticks() < deadline)
        __asm__ volatile ("hlt");

    if (rem_addr) {
        timespec_t *rem = (timespec_t *)rem_addr;
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

/* getrandom — fill buffer with random bytes */
static int64_t sys_getrandom(uint64_t buf_addr, uint64_t buflen, uint64_t flags)
{
    (void)flags;
    if (!buf_addr) return -EFAULT;

    uint8_t *buf = (uint8_t *)buf_addr;
    for (uint64_t i = 0; i < buflen; i += 8) {
        uint64_t r = urandom_next();
        uint64_t n = buflen - i;
        if (n > 8) n = 8;
        memcpy(buf + i, &r, (size_t)n);
    }
    return (int64_t)buflen;
}

/* sched_yield — yield CPU (no-op in non-preemptive for now) */
static int64_t sys_sched_yield(void)
{
    __asm__ volatile ("hlt");  /* Wait for next timer tick */
    return 0;
}

/* fcntl — file control (minimal: F_GETFD, F_SETFD, F_GETFL, F_SETFL) */
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD  0
#define F_DUPFD_CLOEXEC 1030

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    (void)arg;

    switch (cmd) {
    case F_GETFD: return 0;  /* No close-on-exec */
    case F_SETFD: return 0;  /* Ignore */
    case F_GETFL: return (int64_t)fd_table[fd].oflags;
    case F_SETFL: fd_table[fd].oflags = (uint16_t)(arg & 0xFFFF); return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        /* Find lowest fd >= arg */
        for (uint64_t i = arg; i < MAX_FDS; i++) {
            if (!fd_table[i].open) {
                fd_table[i] = fd_table[fd];
                return (int64_t)i;
            }
        }
        return -EMFILE;
    }
    default: return -EINVAL;
    }
}

/* newfstatat / fstatat — stat by path relative to dirfd */
#define AT_FDCWD -100

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr,
                               uint64_t statbuf_addr, uint64_t flags)
{
    (void)dirfd; (void)flags;
    const char *path = (const char *)path_addr;
    if (!path || !statbuf_addr) return -EFAULT;

    /* Open, stat, close */
    int64_t fd = sys_open(path_addr, O_RDONLY, 0);
    if (fd < 0) return fd;
    int64_t ret = sys_fstat((uint64_t)fd, statbuf_addr);
    sys_close((uint64_t)fd);
    return ret;
}

/* pread64 — read from fd at offset without changing position */
static int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -ESPIPE;

    uint64_t saved_offset = f->offset;
    f->offset = offset;
    int64_t ret = sys_read(fd, buf, count);
    f->offset = saved_offset;
    return ret;
}

/* pwrite64 — write to fd at offset without changing position */
static int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -ESPIPE;

    uint64_t saved_offset = f->offset;
    f->offset = offset;
    int64_t ret = sys_write(fd, buf, count);
    f->offset = saved_offset;
    return ret;
}

/* futex — minimal WAIT/WAKE */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128

static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                          uint64_t timeout, uint64_t uaddr2)
{
    (void)timeout; (void)uaddr2;
    int cmd = (int)(op & ~FUTEX_PRIVATE_FLAG);

    if (cmd == FUTEX_WAIT) {
        /* Check if value matches, if so sleep briefly */
        volatile int *addr = (volatile int *)uaddr;
        if (*addr != (int)val) return -EAGAIN;
        /* Single-threaded: just yield once */
        __asm__ volatile ("hlt");
        return 0;
    }
    if (cmd == FUTEX_WAKE) {
        /* Single-threaded: nothing to wake */
        return 0;
    }
    return -ENOSYS;
}

/* set_robust_list — stub for thread-safety (musl calls at startup) */
static int64_t sys_set_robust_list(uint64_t head, uint64_t len)
{
    (void)head; (void)len;
    return 0;
}

/* prlimit64 — get/set resource limits */
typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} rlimit64_t;

#define RLIMIT_STACK 3
#define RLIMIT_NOFILE 7

static int64_t sys_prlimit64(uint64_t pid, uint64_t resource,
                              uint64_t new_rlim_addr, uint64_t old_rlim_addr)
{
    (void)pid; (void)new_rlim_addr;

    if (old_rlim_addr) {
        rlimit64_t *old = (rlimit64_t *)old_rlim_addr;
        switch (resource) {
        case RLIMIT_STACK:
            old->rlim_cur = 8 * 1024 * 1024;  /* 8MB */
            old->rlim_max = 8 * 1024 * 1024;
            break;
        case RLIMIT_NOFILE:
            old->rlim_cur = MAX_FDS;
            old->rlim_max = MAX_FDS;
            break;
        default:
            old->rlim_cur = (uint64_t)-1;  /* RLIM_INFINITY */
            old->rlim_max = (uint64_t)-1;
            break;
        }
    }
    return 0;
}

/* dup — duplicate fd to lowest available */
static int64_t sys_dup(uint64_t oldfd)
{
    if (oldfd >= MAX_FDS || !fd_table[oldfd].open) return -EBADF;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_table[i].open) {
            fd_table[i] = fd_table[oldfd];
            return (int64_t)i;
        }
    }
    return -EMFILE;
}

#define SYS_DUP 32

/* ── Busybox/POSIX syscalls (X-SYSCALL40) ────────────────────── */

/* openat — open relative to directory fd */
static int64_t sys_openat(uint64_t dirfd, uint64_t path_addr,
                           uint64_t flags, uint64_t mode)
{
    (void)dirfd;  /* AT_FDCWD (-100) = relative to cwd, same as open */
    return sys_open(path_addr, flags, mode);
}

/* Forward declaration for readlinkat */
static int64_t sys_readlink(uint64_t path_addr, uint64_t buf_addr, uint64_t bufsiz);

/* readlinkat — readlink relative to directory fd */
static int64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr,
                               uint64_t buf_addr, uint64_t bufsiz)
{
    (void)dirfd;
    return sys_readlink(path_addr, buf_addr, bufsiz);
}

/* uname — return system information */
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

static int64_t sys_uname(uint64_t buf_addr)
{
    if (!buf_addr) return -EFAULT;
    utsname_t *u = (utsname_t *)buf_addr;
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "OsitoK");
    strcpy(u->nodename, "osito");
    strcpy(u->release, "1.0.0");
    strcpy(u->version, "bare-metal x86-64");
    strcpy(u->machine, "x86_64");
    return 0;
}

/* prctl — process control */
#define PR_SET_NAME 15
#define PR_GET_NAME 16

static char prctl_name[16] = "kernel";

static int64_t sys_prctl(uint64_t option, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg3; (void)arg4; (void)arg5;
    switch (option) {
    case PR_SET_NAME:
        if (!arg2) return -EFAULT;
        memset(prctl_name, 0, 16);
        { const char *src = (const char *)arg2;
          for (int i = 0; i < 15 && src[i]; i++) prctl_name[i] = src[i]; }
        return 0;
    case PR_GET_NAME:
        if (!arg2) return -EFAULT;
        memcpy((void *)arg2, prctl_name, 16);
        return 0;
    default:
        return 0;  /* Silently accept unknown prctl options */
    }
}

/* sendfile — copy data between file descriptors */
static int64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd,
                             uint64_t offset_ptr, uint64_t count)
{
    if (out_fd >= MAX_FDS || !fd_table[out_fd].open) return -EBADF;
    if (in_fd >= MAX_FDS || !fd_table[in_fd].open) return -EBADF;

    /* If offset provided, seek input first */
    if (offset_ptr) {
        int64_t *offp = (int64_t *)offset_ptr;
        fd_entry_t *inf = &fd_table[in_fd];
        if (inf->type == FD_TYPE_FILE)
            inf->offset = (uint64_t)*offp;
    }

    /* Copy in chunks via stack buffer */
    char buf[2048];
    int64_t total = 0;
    while ((uint64_t)total < count) {
        uint64_t chunk = count - (uint64_t)total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        int64_t nr = sys_read(in_fd, (uint64_t)buf, chunk);
        if (nr <= 0) break;
        int64_t nw = sys_write(out_fd, (uint64_t)buf, (uint64_t)nr);
        if (nw < 0) return (total > 0) ? total : nw;
        total += nw;
        if (nw < nr) break;
    }

    /* Update offset if provided */
    if (offset_ptr) {
        int64_t *offp = (int64_t *)offset_ptr;
        *offp += total;
    }

    return total;
}

/* pipe2 — pipe with flags */
static int64_t sys_pipe2(uint64_t pipefd_addr, uint64_t flags)
{
    (void)flags;  /* ignore O_CLOEXEC/O_NONBLOCK for now */
    return sys_pipe(pipefd_addr);
}

/* dup3 — dup2 with flags */
static int64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags)
{
    (void)flags;
    return sys_dup2(oldfd, newfd);
}

/* close_range — close a range of file descriptors */
static int64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags)
{
    (void)flags;
    if (last >= MAX_FDS) last = MAX_FDS - 1;
    for (uint64_t i = first; i <= last; i++) {
        if (fd_table[i].open)
            sys_close(i);
    }
    return 0;
}

/* ── clone/fork + wait4 (X-SYSCALL40 process management) ─────── */

/* Process table access for clone/fork */
extern int32_t proc_fork(void);
extern int32_t proc_wait4(int32_t pid, int *wstatus, int options);

/*
 * sys_clone — create child process (fork semantics).
 * Busybox calls clone(SIGCHLD, NULL, NULL, NULL, 0) which is equivalent to fork().
 * Full CLONE_VM|CLONE_THREAD (threads) is NOT supported yet.
 */
static int64_t sys_clone(uint64_t flags, uint64_t child_stack,
                          uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    (void)child_stack; (void)ptid; (void)ctid; (void)tls;

    /* Check for unsupported thread flags */
    uint64_t thread_flags = 0x00010000 | 0x00000100 | 0x00002000;
    /* CLONE_VM | CLONE_THREAD | CLONE_SIGHAND */
    if (flags & thread_flags)
        return -ENOSYS;  /* No thread support yet */

    /* Fork semantics: SIGCHLD flag (or bare CLONE_CHILD_CLEARTID etc.) */
    int32_t ret = proc_fork();
    return (int64_t)ret;
}

/* wait4 — wait for child process */
static int64_t sys_wait4(uint64_t pid, uint64_t wstatus_addr,
                          uint64_t options, uint64_t rusage)
{
    (void)rusage;
    int wstatus = 0;
    int32_t ret = proc_wait4((int32_t)pid, &wstatus, (int)options);
    if (ret > 0 && wstatus_addr) {
        *(int *)wstatus_addr = wstatus;
    }
    return (int64_t)ret;
}

/* execve — replace process image (basic impl) */
extern int proc_execve(const char *path, char *const argv[]);

static int64_t sys_execve(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr)
{
    (void)envp_addr;
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;

    int ret = proc_execve(path, (char *const *)argv_addr);
    if (ret < 0) return -ENOENT;
    /* If execve succeeds, it doesn't return */
    return 0;
}

/* stat/lstat — stat by path (reuse newfstatat logic) */
static int64_t sys_stat(uint64_t path_addr, uint64_t statbuf_addr)
{
    return sys_newfstatat((uint64_t)AT_FDCWD, path_addr, statbuf_addr, 0);
}

/* ── VFS: getcwd, readlink, getdents64 (X-VFS) ──────────────── */

static int64_t sys_getcwd(uint64_t buf_addr, uint64_t size)
{
    if (!buf_addr || size < 2) return -EINVAL;
    char *buf = (char *)buf_addr;
    buf[0] = '/';
    buf[1] = '\0';
    return (int64_t)buf_addr;
}

static int64_t sys_readlink(uint64_t path_addr, uint64_t buf_addr, uint64_t bufsiz)
{
    const char *path = (const char *)path_addr;
    char *buf = (char *)buf_addr;
    if (!path || !buf || bufsiz == 0) return -EFAULT;

    /* /proc/self/exe → return current process name */
    if (str_startswith(path, "/proc/self/exe") ||
        str_startswith(path, "/proc/") /* /proc/<pid>/exe */) {
        const char *name = proc_current_name();
        if (!name) name = "unknown";
        uint64_t len = strlen(name);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, name, (size_t)len);
        return (int64_t)len;
    }

    return -EINVAL;
}

/* Linux getdents64 structure */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} linux_dirent64_t;

#define DT_REG  8
#define DT_CHR  2
#define DT_DIR  4

extern void *osfs2_file_at(uint32_t index);
extern const char *osfs2_file_name(void *file);

static int64_t sys_getdents64(uint64_t fd, uint64_t dirp_addr, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;

    fd_entry_t *f = &fd_table[fd];
    uint8_t *buf = (uint8_t *)dirp_addr;
    uint64_t pos = 0;
    int idx = (int)f->offset;  /* use offset as directory position */

    if (f->type == FD_TYPE_DEV) {
        /* Listing /dev/ directory */
        static const char *dev_names[] = { "null", "zero", "urandom", "console", "tty", "random" };
        int ndevs = 6;

        for (int i = idx; i < ndevs; i++) {
            uint64_t namelen = strlen(dev_names[i]);
            uint64_t reclen = (uint64_t)(((int)(19 + namelen + 1) + 7) & ~7);  /* align to 8 */
            if (pos + reclen > count) break;

            linux_dirent64_t *d = (linux_dirent64_t *)(buf + pos);
            d->d_ino = (uint64_t)(i + 100);
            d->d_off = (int64_t)(i + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = DT_CHR;
            memcpy(d->d_name, dev_names[i], (size_t)(namelen + 1));
            pos += reclen;
            f->offset = (uint64_t)(i + 1);
        }
        return (int64_t)pos;
    }

    if (f->type == FD_TYPE_FILE || f->type == FD_TYPE_PROC) {
        /* Listing OsitoFS root directory or /proc */
        for (int i = idx; ; i++) {
            void *file = osfs2_file_at(i);
            if (!file) break;
            const char *name = osfs2_file_name(file);
            if (!name) continue;
            uint64_t namelen = strlen(name);
            uint64_t reclen = (uint64_t)(((int)(19 + namelen + 1) + 7) & ~7);
            if (pos + reclen > count) break;

            linux_dirent64_t *d = (linux_dirent64_t *)(buf + pos);
            d->d_ino = (uint64_t)(i + 1);
            d->d_off = (int64_t)(i + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = DT_REG;
            memcpy(d->d_name, name, (size_t)(namelen + 1));
            pos += reclen;
            f->offset = (uint64_t)(i + 1);
        }
        return (int64_t)pos;
    }

    return -ENOTDIR;
}

/* ── Syscall dispatch (called from assembly) ─────────────────── */

/* Debug: serial port direct write (no function call, no relocation issues) */
static inline void dbg_serial_char(char c) {
    while (!(inb(0x3FD) & 0x20)) {}
    outb(0x3F8, c);
}
static inline void dbg_serial_hex8(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    dbg_serial_char(h[v >> 4]);
    dbg_serial_char(h[v & 0xF]);
}

int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5)
{
    /* Debug: emit syscall number as hex to serial (PIE-safe) */
    dbg_serial_char('<');
    dbg_serial_hex8((uint8_t)(nr >> 8));
    dbg_serial_hex8((uint8_t)nr);
    dbg_serial_char('>');

    switch (nr) {
    case SYS_READ:       return sys_read(a1, a2, a3);
    case SYS_WRITE:      return sys_write(a1, a2, a3);
    case SYS_OPEN:       return sys_open(a1, a2, a3);
    case SYS_CLOSE:      return sys_close(a1);
    case SYS_FSTAT:      return sys_fstat(a1, a2);
    case SYS_LSEEK:      return sys_lseek(a1, (int64_t)a2, a3);
    case SYS_MMAP:       return sys_mmap(a1, a2, a3, a4, a5, 0);
    case SYS_MPROTECT:   return sys_mprotect(a1, a2, a3);
    case SYS_MUNMAP:     return sys_munmap(a1, a2);
    case SYS_BRK:        return sys_brk(a1);
    case SYS_SIGACTION:  return sys_sigaction(a1, a2, a3);
    case SYS_SIGPROCMASK: return sys_rt_sigprocmask(a1, a2, a3, a4);
    case SYS_SIGRETURN:  return 0;  /* stub */
    case SYS_IOCTL:      return sys_ioctl(a1, a2, a3);
    case SYS_PREAD64:    return sys_pread64(a1, a2, a3, a4);
    case SYS_PWRITE64:   return sys_pwrite64(a1, a2, a3, a4);
    case SYS_WRITEV:     return sys_writev(a1, a2, a3);
    case SYS_ACCESS:     return sys_access(a1, a2);
    case SYS_SCHED_YIELD: return sys_sched_yield();
    case SYS_PIPE:       return sys_pipe(a1);
    case SYS_DUP:        return sys_dup(a1);
    case SYS_DUP2:       return sys_dup2(a1, a2);
    case SYS_NANOSLEEP:  return sys_nanosleep(a1, a2);
    case SYS_GETPID:     return sys_getpid();
    case SYS_MADVISE:    return 0;  /* ignore hints */
    case SYS_STAT:       return sys_stat(a1, a2);
    case SYS_LSTAT:      return sys_stat(a1, a2);  /* no symlinks */
    case SYS_SENDFILE:   return sys_sendfile(a1, a2, a3, a4);
    case SYS_CLONE:      return sys_clone(a1, a2, a3, a4, a5);
    case SYS_FORK:       return sys_clone(17 /* SIGCHLD */, 0, 0, 0, 0);
    case SYS_EXECVE:     return sys_execve(a1, a2, a3);
    case SYS_EXIT:       return sys_exit(a1);
    case SYS_WAIT4:      return sys_wait4(a1, a2, a3, a4);
    case SYS_KILL:       return sys_kill(a1, a2);
    case SYS_UNAME:      return sys_uname(a1);
    case SYS_FCNTL:      return sys_fcntl(a1, a2, a3);
    case SYS_FSYNC:      return 0;  /* no-op */
    case SYS_GETCWD:     return sys_getcwd(a1, a2);
    case SYS_UNLINK:     return sys_unlink(a1);
    case SYS_READLINK:   return sys_readlink(a1, a2, a3);
    case SYS_GETUID:     return 0;
    case SYS_GETGID:     return 0;
    case SYS_SETUID:     return 0;
    case SYS_SETGID:     return 0;
    case SYS_GETEUID:    return 0;
    case SYS_GETEGID:    return 0;
    case SYS_GETPPID:    return (int64_t)proc_current_ppid();
    case SYS_GETPGRP:    return (int64_t)proc_current_pid();
    case SYS_SETSID:     return (int64_t)proc_current_pid();
    case SYS_GETGROUPS:  return 0;  /* no supplementary groups */
    case SYS_SIGALTSTACK: return sys_sigaltstack(a1, a2);
    case SYS_PRCTL:      return sys_prctl(a1, a2, a3, a4, a5);
    case SYS_ARCH_PRCTL: return sys_arch_prctl(a1, a2);
    case SYS_GETTID:     return sys_gettid();
    case SYS_FUTEX:      return sys_futex(a1, a2, a3, a4, a5);
    case SYS_GETDENTS64: return sys_getdents64(a1, a2, a3);
    case SYS_SET_TID_ADDR: return sys_set_tid_address(a1);
    case SYS_CLOCK_GETTIME: return sys_clock_gettime(a1, a2);
    case SYS_EXIT_GROUP: return sys_exit_group(a1);
    case SYS_TGKILL:     return sys_kill(a2, a3);  /* reuse kill */
    case SYS_OPENAT:     return sys_openat(a1, a2, a3, a4);
    case SYS_NEWFSTATAT: return sys_newfstatat(a1, a2, a3, a4);
    case SYS_READLINKAT: return sys_readlinkat(a1, a2, a3, a4);
    case SYS_SET_ROBUST_LIST: return sys_set_robust_list(a1, a2);
    case SYS_DUP3:       return sys_dup3(a1, a2, a3);
    case SYS_PIPE2:      return sys_pipe2(a1, a2);
    case SYS_PRLIMIT64:  return sys_prlimit64(a1, a2, a3, a4);
    case SYS_GETRANDOM:  return sys_getrandom(a1, a2, a3);
    case SYS_RSEQ:       return -ENOSYS;  /* musl handles gracefully */
    case SYS_CLOSE_RANGE: return sys_close_range(a1, a2, a3);
    default:
        serial_puts("[SYSCALL] Unknown syscall ");
        serial_putdec(nr);
        serial_puts("\n");
        return -ENOSYS;
    }
}

/* ── Get current CS for STAR MSR ─────────────────────────────── */

static uint16_t get_cs(void)
{
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs;
}

/* ── Reset per-process syscall state ─────────────────────────── */

void syscall_reset_process(void)
{
    /* Close file/pipe FDs (keep console on 0/1/2) */
    for (int i = 3; i < MAX_FDS; i++) {
        if (fd_table[i].open)
            sys_close((uint64_t)i);
    }

    /* Reset signal state */
    memset(sig_handlers, 0, sizeof(sig_handlers));
    sig_pending = 0;

    /* Free brk heap */
    if (brk_base) {
        kfree(brk_base);
        brk_base = NULL;
        brk_current = NULL;
        brk_max = NULL;
    }

    /* Free mmap regions */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (vma_table[i].in_use) {
            mem_free_pages((void *)vma_table[i].base, vma_table[i].pages);
            vma_table[i].in_use = false;
        }
    }
}

/* ── Initialize syscall interface ────────────────────────────── */

/* Assembly entry point (defined in syscall_entry.S) */
extern void syscall_entry(void);
void syscall_init(void)
{
    serial_puts("[SYSCALL] Setting up SYSCALL/SYSRET...\n");

    /* Enable SYSCALL in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    /* STAR: kernel CS/SS in bits 47:32, user CS/SS in bits 63:48.
     * SYSCALL loads CS = STAR[47:32], SS = STAR[47:32]+8
     * SYSRET loads  CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *
     * With UEFI's typical GDT: CS=0x38, DS=0x30
     * Kernel: CS=0x38, SS=0x30 → STAR[47:32] = 0x38
     * User:   For SYSRET to give CS=user_cs, SS=user_ss
     *         CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *         We want user_cs=0x43 (ring 3), user_ss=0x3B (ring 3)
     *         → STAR[63:48] = 0x33 (so CS=0x33+16=0x43, SS=0x33+8=0x3B)
     *
     * But for now we only have ring 0, so user selectors don't matter yet.
     */
    uint16_t kernel_cs = get_cs();
    uint16_t kernel_ss;
    __asm__ volatile ("mov %%ss, %0" : "=r"(kernel_ss));

    /* SYSCALL requires CS and SS as adjacent GDT entries:
     * CS = STAR[47:32], SS = STAR[47:32]+8.
     * Use (SS-8) as the SYSCALL CS base so SS lands on the
     * actual data segment, not the TSS. */
    uint16_t syscall_cs_base = kernel_ss - 8;
    uint64_t star = ((uint64_t)(syscall_cs_base - 16) << 48) |
                    ((uint64_t)syscall_cs_base << 32);

    serial_puts("[SYSCALL] Kernel CS=0x");
    serial_puthex(kernel_cs, 4);
    serial_puts(" SS=0x");
    serial_puthex(kernel_ss, 4);
    serial_puts(" SYSCALL CS base=0x");
    serial_puthex(syscall_cs_base, 4);
    serial_puts("\n");

    wrmsr(MSR_STAR, star);

    /* LSTAR: RIP loaded on SYSCALL */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* FMASK: RFLAGS bits cleared on SYSCALL (mask IF + DF + TF) */
    wrmsr(MSR_FMASK, 0x700);  /* IF=0x200, DF=0x400, TF=0x100 */

    /* Initialize FD table with stdin/stdout/stderr */
    memset(fd_table, 0, sizeof(fd_table));

    fd_table[0].open  = true;
    fd_table[0].type  = FD_TYPE_CONSOLE;
    fd_table[0].read  = console_read;

    fd_table[1].open  = true;
    fd_table[1].type  = FD_TYPE_CONSOLE;
    fd_table[1].write = console_write;

    fd_table[2].open  = true;
    fd_table[2].type  = FD_TYPE_CONSOLE;
    fd_table[2].write = console_write;

    /* Reset brk state */
    brk_base = NULL;
    brk_current = NULL;
    brk_max = NULL;

    serial_puts("[SYSCALL] Ready (LSTAR=0x");
    serial_puthex((uint64_t)syscall_entry, 16);
    serial_puts(")\n");
    fb_puts(" Syscall: SYSCALL/SYSRET active\n");
}
