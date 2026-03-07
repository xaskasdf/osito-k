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
 *  12 = brk(addr)
 *  60 = exit(status)
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
#define SYS_BRK         12
#define SYS_IOCTL       16
#define SYS_WRITEV      20
#define SYS_EXIT        60
#define SYS_ARCH_PRCTL  158

/* errno values */
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

/* stdout/stderr → serial + framebuffer */
static ssize_t console_write(const void *buf, size_t count)
{
    const char *s = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        serial_putc(s[i]);
        fb_putc(s[i], 0x00CCCCCC);
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

/* ── Syscall handlers ────────────────────────────────────────── */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!buf && count > 0) return -EFAULT;

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_FILE) {
        if ((f->oflags & O_ACCMODE) == O_RDONLY) return -EBADF;
        int ret = osfs2_write(f->file, f->offset, (const void *)buf, count);
        if (ret < 0) return -EFAULT;
        f->offset += count;
        return (int64_t)count;
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

    /* Console */
    if (!f->read) return -EBADF;
    return f->read((void *)buf, (size_t)count);
}

static int64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode)
{
    (void)mode;
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;

    /* Find lowest free fd */
    int newfd = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_table[i].open) { newfd = i; break; }
    }
    if (newfd < 0) return -EMFILE;

    /* Try to find existing file */
    void *file = osfs2_find(path);

    if (!file && (flags & O_CREAT)) {
        /* Create new file — start with 1MB allocation */
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
        /* Truncate not supported by OsitoFS — just reset offset */
        f->offset = 0;
    }

    return newfd;
}

static int64_t sys_close(uint64_t fd)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_table[fd].open = false;
    fd_table[fd].file = NULL;
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

/* ── Syscall dispatch (called from assembly) ─────────────────── */

int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;

    switch (nr) {
    case SYS_READ:       return sys_read(a1, a2, a3);
    case SYS_WRITE:      return sys_write(a1, a2, a3);
    case SYS_OPEN:       return sys_open(a1, a2, a3);
    case SYS_CLOSE:      return sys_close(a1);
    case SYS_FSTAT:      return sys_fstat(a1, a2);
    case SYS_LSEEK:      return sys_lseek(a1, (int64_t)a2, a3);
    case SYS_BRK:        return sys_brk(a1);
    case SYS_IOCTL:      return sys_ioctl(a1, a2, a3);
    case SYS_WRITEV:     return sys_writev(a1, a2, a3);
    case SYS_EXIT:       return sys_exit(a1);
    case SYS_ARCH_PRCTL: return -ENOSYS;  /* stub */
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
    /* Close file FDs (keep console on 0/1/2) */
    for (int i = 3; i < MAX_FDS; i++) {
        fd_table[i].open = false;
        fd_table[i].file = NULL;
    }

    /* Free brk heap */
    if (brk_base) {
        kfree(brk_base);
        brk_base = NULL;
        brk_current = NULL;
        brk_max = NULL;
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
