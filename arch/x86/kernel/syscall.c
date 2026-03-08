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
 *   0 = read(fd, buf, count)
 *   1 = write(fd, buf, count)
 *   2 = open(path, flags, mode)
 *   3 = close(fd)
 *   5 = fstat(fd, statbuf)
 *   8 = lseek(fd, offset, whence)
 *   9 = mmap(addr, len, prot, flags, fd, offset)
 *  10 = mprotect(addr, len, prot)
 *  11 = munmap(addr, len)
 *  12 = brk(addr)
 *  13 = sigaction(sig, act, oldact)
 *  22 = pipe(pipefd[2])
 *  33 = dup2(oldfd, newfd)
 *  60 = exit(status)
 *  62 = kill(pid, sig)
 * 158 = arch_prctl             [stub]
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

/* Physical memory */
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

/* Paging */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_unmap_page(uint64_t virt);
extern int paging_set_flags(uint64_t virt, uint64_t flags);

/* ── MSR definitions ─────────────────────────────────────────── */

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
#define SYS_SIGRETURN   15

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

int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5)
{
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
    case SYS_IOCTL:      return sys_ioctl(a1, a2, a3);
    case SYS_WRITEV:     return sys_writev(a1, a2, a3);
    case SYS_ACCESS:     return sys_access(a1, a2);
    case SYS_PIPE:       return sys_pipe(a1);
    case SYS_DUP2:       return sys_dup2(a1, a2);
    case SYS_EXIT:       return sys_exit(a1);
    case SYS_KILL:       return sys_kill(a1, a2);
    case SYS_GETCWD:     return sys_getcwd(a1, a2);
    case SYS_UNLINK:     return sys_unlink(a1);
    case SYS_READLINK:   return sys_readlink(a1, a2, a3);
    case SYS_ARCH_PRCTL: return -ENOSYS;  /* stub */
    case SYS_GETDENTS64: return sys_getdents64(a1, a2, a3);
    case SYS_SIGACTION:  return sys_sigaction(a1, a2, a3);
    case SYS_SIGRETURN:  return 0;  /* stub */
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
