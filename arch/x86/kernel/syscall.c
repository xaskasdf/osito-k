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

/* Direct serial I/O (no relocation, no FB noise) */
static inline void dbg_serial_char(char c) {
    while (!(inb(0x3FD) & 0x20)) {}
    outb(0x3F8, c);
}
static inline void dbg_serial_hex8(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    dbg_serial_char(h[v >> 4]);
    dbg_serial_char(h[v & 0xF]);
}

/* CMOS RTC — read boot epoch once */
static inline uint8_t cmos_rd(uint8_t reg) {
    __asm__ volatile ("outb %0, %1" : : "a"(reg), "Nd"((uint16_t)0x70));
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)0x71));
    return val;
}
static inline uint8_t bcd2b(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint64_t rtc_to_epoch(void) {
    uint8_t sec=bcd2b(cmos_rd(0)),min=bcd2b(cmos_rd(2)),hour=bcd2b(cmos_rd(4));
    uint8_t day=bcd2b(cmos_rd(7)),mon=bcd2b(cmos_rd(8)),year=bcd2b(cmos_rd(9));
    uint32_t y = 2000 + year;
    static const uint16_t mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t days = 0;
    for (uint32_t i = 1970; i < y; i++)
        days += (i%4==0 && (i%100!=0 || i%400==0)) ? 366 : 365;
    for (uint8_t i = 1; i < mon; i++) {
        days += mdays[i];
        if (i==2 && (y%4==0 && (y%100!=0 || y%400==0))) days++;
    }
    days += day - 1;
    return (uint64_t)days*86400 + hour*3600 + min*60 + sec;
}
static uint64_t boot_epoch_sec;

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern void *osfs2_find_ci(const char *name);      /* case-insensitive lookup */
extern const char *osfs2_file_name(void *file);    /* get filename for hint */
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

/* Shared memory (shm.c) — weak symbols for optional linkage */
extern uint32_t shm_create(uint64_t size, uint32_t flags)        __attribute__((weak));
extern void    *shm_map(uint32_t handle)                         __attribute__((weak));
extern void     shm_unmap(uint32_t handle)                       __attribute__((weak));
extern void     shm_destroy(uint32_t handle)                     __attribute__((weak));
extern uint64_t shm_get_phys(uint32_t handle)                    __attribute__((weak));
extern uint64_t shm_get_size(uint32_t handle)                    __attribute__((weak));
extern uint32_t shm_create_surface(uint32_t w, uint32_t h, uint32_t f) __attribute__((weak));
extern void     shm_flush_surface(uint32_t handle)               __attribute__((weak));
extern bool     input_pop_event(void *out_evt)                   __attribute__((weak));

/* QoS scheduler (process.c) */
extern int     sched_set_qos(uint32_t pid, uint8_t qos)         __attribute__((weak));
extern uint8_t sched_get_qos(uint32_t pid)                      __attribute__((weak));

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
#define SYS_MREMAP      25
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
#define SYS_RT_SIGSUSPEND 130
#define SYS_SIGALTSTACK 131
#define SYS_GETTID      186
#define SYS_TKILL       200
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
#define SYS_POLL        7
#define SYS_PPOLL       271
#define SYS_SELECT      23
#define SYS_MADVISE     28
#define SYS_STAT        4
#define SYS_LSTAT       6
#define SYS_DUP         32
#define SYS_VFORK       58
#define SYS_PAUSE       34
#define SYS_CHDIR       80
#define SYS_RENAME      82
#define SYS_MKDIR       83
#define SYS_RMDIR       84
#define SYS_CHMOD       90
#define SYS_FCHMOD      91
#define SYS_CHOWN       92
#define SYS_FCHOWN      93
#define SYS_UMASK       95
#define SYS_GETTIMEOFDAY 96
#define SYS_GETRLIMIT   97
#define SYS_GETRUSAGE   98
#define SYS_SYSINFO     99
#define SYS_TIMES       100
#define SYS_GETRESUID   120
#define SYS_GETRESGID   122  /* Linux: 120=getresuid, 121=setresgid, 122=getresgid */
#define SYS_SETPGID     109
#define SYS_GETPGID     121
#define SYS_STATFS      137
#define SYS_FSTATFS     138
#define SYS_SETRLIMIT   160
#define SYS_SYNC        162
#define SYS_FCHDIR      81
#define SYS_FTRUNCATE   77
#define SYS_TRUNCATE    76
#define SYS_WAITID      247
#define SYS_UNLINKAT    263
#define SYS_MKDIRAT     258
#define SYS_FCHOWNAT    260
#define SYS_FCHMODAT    268
#define SYS_FACCESSAT   269
#define SYS_PSELECT6    270
#define SYS_UTIMENSAT   280
#define SYS_RENAMEAT2   316
#define SYS_STATX       332
#define SYS_FACCESSAT2  439
#define SYS_EPOLL_CREATE1 291
#define SYS_EPOLL_CTL     233
#define SYS_EPOLL_WAIT    232
#define SYS_EPOLL_PWAIT   281
#define SYS_EVENTFD2      290
#define SYS_SOCKET        41
#define SYS_CONNECT       42
#define SYS_ACCEPT        43
#define SYS_SENDTO        44
#define SYS_RECVFROM      45
#define SYS_SENDMSG       46
#define SYS_RECVMSG       47
#define SYS_SHUTDOWN      48
#define SYS_BIND          49
#define SYS_LISTEN        50
#define SYS_GETSOCKNAME   51
#define SYS_GETPEERNAME   52
#define SYS_SETSOCKOPT    54
#define SYS_GETSOCKOPT    55
#define SYS_ACCEPT4       288
#define SYS_ALARM         37
#define SYS_SETITIMER     38
#define SYS_GETITIMER     36
#define SYS_IOPL          172
#define SYS_IOPERM        173
#define SYS_SHMGET        29
#define SYS_SHMAT         30
#define SYS_SHMCTL        31
#define SYS_SHMDT         67
#define SYS_SEMGET        64
#define SYS_SEMOP         65
#define SYS_SEMCTL        66

/* OsitoK private syscalls (500+) */
#define SYS_SHM_CREATE      500
#define SYS_SHM_MAP         501
#define SYS_SHM_UNMAP       502
#define SYS_SHM_DESTROY     503
#define SYS_SHM_GETPHYS     504
#define SYS_SHM_GETSIZE     505
#define SYS_SHM_MKSURFACE   506
#define SYS_GUI_FLIP        507
#define SYS_SCHED_SETQOS    510
#define SYS_SCHED_GETQOS    511
#define SYS_GET_INPUT_EVENT 512

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
#define ENODEV  19
#define EINTR    4

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

#define MAX_FDS 128

#define FD_TYPE_CONSOLE 1
#define FD_TYPE_FILE    2
#define FD_TYPE_PIPE    3
#define FD_TYPE_DEV     4   /* virtual device (/dev/null, /dev/zero, etc.) */
#define FD_TYPE_PROC    5   /* virtual procfs (/proc/self/maps, etc.) */
#define FD_TYPE_TMPFS   6   /* tmpfs file (/tmp/*) */
#define FD_TYPE_FAT32   7   /* FAT32 file (/fat/*) */
#define FD_TYPE_EXT2    8   /* ext2 file (/ext2/*) */
#define FD_TYPE_ISO     9
#define FD_TYPE_EPOLL   10
#define FD_TYPE_EVENTFD 11
#define FD_TYPE_SOCKET  12

typedef ssize_t (*fd_write_fn)(const void *buf, size_t count);
typedef ssize_t (*fd_read_fn)(void *buf, size_t count);

/* osfs2_file_t is opaque here — we get size via osfs2_file_size() */
extern uint64_t osfs2_file_size(void *file);
extern uint32_t osfs2_file_ctime(void *file);
extern uint32_t osfs2_file_mtime(void *file);

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
#define SIGCHLD     17
#define SIGTERM     15
#define SIGKILL      9

#define SIG_DFL     ((uint64_t)0)
#define SIG_IGN     ((uint64_t)1)

static uint64_t sig_handlers[NSIG];     /* legacy — kept for save/restore compat */
static uint32_t sig_pending;            /* legacy — kept for save/restore compat */

/* Per-process signal accessors (process.c) */
typedef struct { uint64_t handler; uint64_t flags; uint64_t restorer; } sig_act_t;
extern void *proc_current(void);
extern sig_act_t *proc_get_sig_actions(void *proc);
extern uint64_t *proc_get_sig_pending_ptr(void *proc);
extern uint64_t *proc_get_sig_mask_ptr(void *proc);
extern void proc_signal_pid(uint32_t pid, int sig);

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

/* stdin → read from PS/2 keyboard ring buffer */
extern char kb_getchar(void);
extern char kb_trygetchar(void);
extern bool kb_has_input(void);

/* ── Terminal mode state (X-EDIT) ─────────────────────────── */
/* Tracks ICANON and ECHO flags from tcsetattr calls. */

static bool term_canonical = true;   /* ICANON: line-buffered input */
static bool term_echo      = true;   /* ECHO: echo input chars */

/* xHCI USB polling (weak: works without xHCI driver) */
extern void xhci_poll(void) __attribute__((weak));

/* Blocking keyboard read with interrupts enabled.
 * SYSCALL entry disables interrupts (FMASK clears IF). We must
 * re-enable them here so keyboard IRQs can actually fire. */
static char kb_getchar_safe(void)
{
    while (!kb_has_input()) {
        __asm__ volatile ("sti" ::: "memory");
        if (xhci_poll) xhci_poll();  /* Poll USB HID devices */
        if (kb_has_input()) { __asm__ volatile ("cli" ::: "memory"); break; }
        __asm__ volatile ("hlt; cli" ::: "memory");
    }
    return kb_getchar();  /* non-blocking now, data is ready */
}

static ssize_t console_read(void *buf, size_t count)
{
    if (count == 0) return 0;
    uint8_t *dst = (uint8_t *)buf;

    if (!term_canonical) {
        /* Raw mode: return individual characters, no line buffering.
         * Block for first char, then return as many as available. */
        dst[0] = (uint8_t)kb_getchar_safe();
        ssize_t n = 1;
        while (n < (ssize_t)count && kb_has_input()) {
            dst[n] = (uint8_t)kb_trygetchar();
            if (dst[n] == 0) break;
            n++;
        }
        return n;
    }

    /* Canonical mode: block for one character */
    dst[0] = (uint8_t)kb_getchar_safe();
    return 1;
}

/* ── brk state (process heap) ────────────────────────────────── */

/* BRK heap size: dynamic from sys_caps (scales with RAM) */
#include "../include/sys_caps.h"
#define BRK_HEAP_SIZE  (g_sys_caps.brk_heap_size ? g_sys_caps.brk_heap_size : (16ULL * 1024 * 1024))

static uint8_t *brk_base;      /* start of brk region */
static uint8_t *brk_current;   /* current break */
static uint8_t *brk_max;       /* end of brk region */

/* Reset brk to base for a new process — called from proc_exec before elf_exec.
 * Zeroes the heap so the new process starts with clean memory. */
void sys_brk_reset(void)
{
    if (brk_base) {
        memset(brk_base, 0, (uint64_t)(brk_current - brk_base));
        brk_current = brk_base;
    }
}

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
#define MAX_VMAS        4096

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
#define DEV_DSP         4
#define DEV_FB0         5   /* /dev/fb0 — framebuffer for direct pixel access */

/* PRNG for /dev/urandom — CCP TRNG if available, RDTSC fallback */
extern uint64_t ccp_random(void) __attribute__((weak));
extern bool     ccp_is_ready(void) __attribute__((weak));

static uint64_t urandom_state;

static uint64_t urandom_next(void)
{
    /* Use hardware TRNG when CCP driver is linked */
    if (ccp_is_ready && ccp_is_ready())
        return ccp_random();

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
        case DEV_DSP: {
            /* Write PCM samples (16-bit signed LE) to HDA audio output */
            extern void hda_play_buffer(const int16_t *, uint32_t)
                __attribute__((weak));
            if (hda_play_buffer) {
                uint32_t num_samples = (uint32_t)(count / 2);
                hda_play_buffer((const int16_t *)buf, num_samples);
                return (int64_t)count;
            }
            return -ENODEV;
        }
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

    if (f->type == FD_TYPE_SOCKET) {
        extern int sock_send(int, const void *, uint32_t, int);
        return sock_send((int)f->offset, (const void *)buf, (uint32_t)count, 0);
    }

    if (f->type == FD_TYPE_TMPFS) {
        extern int tmpfs_write(void *handle, uint64_t offset,
                               const void *buf, uint64_t len);
        int ret = tmpfs_write(f->file, f->offset, (const void *)buf, count);
        if (ret < 0) return -ENOSPC;
        f->offset += ret;
        return (int64_t)ret;
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

    if (f->type == FD_TYPE_SOCKET) {
        extern int sock_recv(int, void *, uint32_t, int);
        return sock_recv((int)f->offset, (void *)buf, (uint32_t)count, 0);
    }

    if (f->type == FD_TYPE_EVENTFD) {
        if (count < 8) return -EINVAL;
        uint64_t val = f->offset;
        if (val == 0) return -EAGAIN;
        *(uint64_t *)buf = val;
        f->offset = 0;
        return 8;
    }

    if (f->type == FD_TYPE_TMPFS) {
        extern int tmpfs_read(void *handle, uint64_t offset, void *buf, uint64_t len);
        int ret = tmpfs_read(f->file, f->offset, (void *)buf, count);
        if (ret <= 0) return ret == 0 ? 0 : -EFAULT;
        f->offset += ret;
        return (int64_t)ret;
    }

    if (f->type == FD_TYPE_FAT32) {
        extern int fat32_read_file(const char *name, uint64_t offset,
                                   void *buf, uint64_t len);
        int ret = fat32_read_file((const char *)f->file, f->offset,
                                  (void *)buf, count);
        if (ret <= 0) return ret == 0 ? 0 : -EFAULT;
        f->offset += ret;
        return (int64_t)ret;
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
        else if (strcmp(devname, "dsp") == 0)     dev_id = DEV_DSP;
        else if (strcmp(devname, "audio") == 0)   dev_id = DEV_DSP;
        else if (strcmp(devname, "fb0") == 0)     dev_id = DEV_FB0;
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
        /* Global /proc entries (strip leading /proc/) */
        else if (strcmp(path + 6, "cpuinfo") == 0 ||
                 strcmp(path + 6, "meminfo") == 0 ||
                 strcmp(path + 6, "uptime") == 0 ||
                 strcmp(path + 6, "mounts") == 0 ||
                 strcmp(path + 6, "version") == 0 ||
                 strcmp(path + 6, "filesystems") == 0)
            proc_id = 100;  /* Generic procfs */
        else return -ENOENT;

        /* Generate content on open */
        if (proc_id == PROC_MAPS)
            proc_buf_len = proc_gen_maps(proc_buf, PROC_BUF_SIZE);
        else if (proc_id == PROC_STATUS)
            proc_buf_len = proc_gen_status(proc_buf, PROC_BUF_SIZE);
        else {
            /* Generate global /proc entries */
            int p = 0;
            const char *name = path + 6;
            extern uint64_t mem_get_free(void);
            extern uint64_t mem_get_total(void);
            extern uint64_t mem_get_used(void);
            extern uint32_t ntp_get_utc(void) __attribute__((weak));

            /* Helper: append decimal to proc_buf */
            #define PBUF_DEC(v) do { \
                char _t[20]; int _n = 0; uint64_t _v = (v); \
                if (_v == 0) { if (p < PROC_BUF_SIZE-1) proc_buf[p++] = '0'; } \
                else { while (_v) { _t[_n++] = '0' + _v % 10; _v /= 10; } \
                       for (int _i = _n-1; _i >= 0 && p < PROC_BUF_SIZE-1; _i--) \
                           proc_buf[p++] = _t[_i]; } \
            } while(0)
            #define PBUF_STR(s) do { const char *_s = (s); \
                while (*_s && p < PROC_BUF_SIZE-1) proc_buf[p++] = *_s++; } while(0)

            if (strcmp(name, "cpuinfo") == 0) {
                PBUF_STR("processor\t: 0\nvendor_id\t: OsitoK\nmodel name\t: OsitoK Bare-Metal x86_64\ncpu MHz\t\t: 3000\n\n");
            } else if (strcmp(name, "meminfo") == 0) {
                PBUF_STR("MemTotal:    "); PBUF_DEC(mem_get_total()/1024); PBUF_STR(" kB\n");
                PBUF_STR("MemFree:     "); PBUF_DEC(mem_get_free()/1024); PBUF_STR(" kB\n");
                PBUF_STR("MemAvailable: "); PBUF_DEC(mem_get_free()/1024); PBUF_STR(" kB\n");
            } else if (strcmp(name, "uptime") == 0) {
                PBUF_DEC(idt_get_ticks()/100); PBUF_STR(".00 ");
                PBUF_DEC(idt_get_ticks()/100); PBUF_STR(".00\n");
            } else if (strcmp(name, "mounts") == 0) {
                const char *s = "ositofs / ositofs rw 0 0\ntmpfs /tmp tmpfs rw 0 0\n";
                while (*s && p < PROC_BUF_SIZE - 1) proc_buf[p++] = *s++;
            } else if (strcmp(name, "version") == 0) {
                const char *s = "OsitoK version 1.0 (bare-metal x86_64)\n";
                while (*s && p < PROC_BUF_SIZE - 1) proc_buf[p++] = *s++;
            } else if (strcmp(name, "filesystems") == 0) {
                const char *s = "\tositofs\n\tfat32\n\ttmpfs\n\text2\n\text4\n\tiso9660\n\texfat\n\tntfs\n\tudf\n\tsquashfs\n\thfsplus\n\tbtrfs\n\tapfs\n";
                while (*s && p < PROC_BUF_SIZE - 1) proc_buf[p++] = *s++;
            }
            proc_buf[p] = '\0';
            proc_buf_len = p;
        }

        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_PROC;
        f->oflags = O_RDONLY;
        f->offset = 0;  /* read position */
        return newfd;
    }

    /* ── VFS: tmpfs (/tmp/*) ──────────────────────────── */
    if (str_startswith(path, "/tmp/")) {
        extern void *tmpfs_open(const char *name);
        extern void *tmpfs_create(const char *name);
        const char *fname = path + 5;
        void *th = tmpfs_open(fname);
        if (!th && (flags & O_CREAT))
            th = tmpfs_create(fname);
        if (!th) return -ENOENT;
        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_TMPFS;
        f->oflags = (uint16_t)(flags & 0xFFFF);
        f->file   = th;
        return newfd;
    }

    /* ── VFS: FAT32 (/fat/*) ─────────────────────────── */
    if (str_startswith(path, "/fat/")) {
        extern int fat32_find(const char *name, uint32_t *cluster, uint32_t *size);
        extern bool fat32_is_mounted(void);
        if (!fat32_is_mounted()) return -ENOENT;
        const char *fname = path + 5;
        uint32_t fsize;
        if (fat32_find(fname, NULL, &fsize) < 0) return -ENOENT;
        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_FAT32;
        f->oflags = (uint16_t)(flags & 0xFFFF);
        f->file   = (void *)fname;
        return newfd;
    }

    /* ── VFS: ext2 (/ext2/*) ─────────────────────────── */
    if (str_startswith(path, "/ext2/")) {
        extern int ext2_find(const char *name, uint32_t *ino);
        extern bool ext2_is_mounted(void);
        if (!ext2_is_mounted()) return -ENOENT;
        const char *fname = path + 6;
        uint32_t ino;
        if (ext2_find(fname, &ino) < 0) return -ENOENT;
        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_EXT2;
        f->oflags = (uint16_t)(flags & 0xFFFF);
        f->file   = (void *)(uintptr_t)ino;
        return newfd;
    }

    /* ── VFS: ISO 9660 (/iso/*) ──────────────────────── */
    if (str_startswith(path, "/iso/")) {
        extern int iso9660_find(const char *name, uint32_t *lba, uint32_t *size);
        extern bool iso9660_is_mounted(void);
        if (!iso9660_is_mounted()) return -ENOENT;
        const char *fname = path + 5;
        uint32_t lba, fsize;
        if (iso9660_find(fname, &lba, &fsize) < 0) return -ENOENT;
        fd_entry_t *f = &fd_table[newfd];
        memset(f, 0, sizeof(*f));
        f->open   = true;
        f->type   = FD_TYPE_ISO;
        f->oflags = (uint16_t)(flags & 0xFFFF);
        f->file   = (void *)(uintptr_t)lba;
        return newfd;
    }

    /* ── OsitoFS: regular files ─────────────────────────── */

    void *file = osfs2_find(path);

    /* Strip leading "/" for OsitoFS lookup if not found */
    if (!file && path[0] == '/')
        file = osfs2_find(path + 1);

    /* Strip leading "./" for relative paths (e.g. "./baseq2/pak0.pak") */
    if (!file && path[0] == '.' && path[1] == '/')
        file = osfs2_find(path + 2);

    /* Try basename (flat FS: /bin/busybox → busybox, /etc/passwd → passwd) */
    if (!file) {
        const char *bn = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') bn = p + 1;
        if (bn != path && *bn)
            file = osfs2_find(bn);
    }

    if (!file && (flags & O_CREAT)) {
        file = osfs2_create(path, 0);
    }

    if (!file) {
        /* Case-insensitive fallback: "doom.wad" opens "DOOM.WAD" */
        void *ci = osfs2_find_ci(path);
        if (!ci && path[0] == '/')
            ci = osfs2_find_ci(path + 1);
        if (!ci && path[0] == '.' && path[1] == '/')
            ci = osfs2_find_ci(path + 2);
        if (!ci) {
            const char *bn = path;
            for (const char *q = path; *q; q++)
                if (*q == '/') bn = q + 1;
            if (bn != path && *bn)
                ci = osfs2_find_ci(bn);
        }
        if (ci) {
            serial_puts("[open] ci-match: '");
            serial_puts(path);
            serial_puts("' -> '");
            serial_puts(osfs2_file_name(ci));
            serial_puts("'\n");
            file = ci;
        } else {
            return -ENOENT;
        }
    }

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
        st->st_ctime_sec = (uint64_t)osfs2_file_ctime(f->file);
        st->st_mtime_sec = (uint64_t)osfs2_file_mtime(f->file);
        st->st_atime_sec = st->st_mtime_sec;
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
int64_t sys_brk(uint64_t addr)
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
    if (length == 0) return -EINVAL;

    /* Handle musl's mallocng guard page request */
    if ((flags & 0x10 /* MAP_FIXED */) && addr != 0) {
        return (int64_t)addr;
    }

    /* /dev/fb0 mmap: return pointer to display back buffer */
    if (!(flags & MAP_ANONYMOUS) && fd < MAX_FDS && fd_table[fd].open &&
        fd_table[fd].type == FD_TYPE_DEV && (int)fd_table[fd].offset == DEV_FB0) {
        extern uint32_t *display_get_back_buffer(void);
        uint32_t *bb = display_get_back_buffer();
        return bb ? (int64_t)(uint64_t)bb : -ENODEV;
    }

    /* File-backed mmap: allocate pages + read file content */
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
        fd_entry_t *f = &fd_table[fd];
        if (f->type != FD_TYPE_FILE && f->type != FD_TYPE_TMPFS &&
            f->type != FD_TYPE_FAT32) return -EBADF;

        uint64_t npages = (length + 4095) / 4096;

        int vi = -1;
        for (int i = 0; i < MAX_VMAS; i++) {
            if (!vma_table[i].in_use) { vi = i; break; }
        }
        if (vi < 0) return -ENOMEM;

        void *pages = mem_alloc_pages(npages);
        if (!pages) return -ENOMEM;

        memset(pages, 0, npages * 4096);

        /* Read file data into the allocated pages */
        uint64_t to_read = length;
        if (f->type == FD_TYPE_FILE) {
            uint64_t file_size = osfs2_file_size(f->file);
            if (offset + to_read > file_size)
                to_read = (offset < file_size) ? file_size - offset : 0;
            if (to_read > 0)
                osfs2_read(f->file, offset, pages, to_read);
        } else if (f->type == FD_TYPE_TMPFS) {
            extern int tmpfs_read(void *, uint64_t, void *, uint64_t);
            tmpfs_read(f->file, offset, pages, to_read);
        } else if (f->type == FD_TYPE_FAT32) {
            extern int fat32_read_file(const char *, uint64_t, void *, uint64_t);
            fat32_read_file((const char *)f->file, offset, pages, to_read);
        }

        vma_table[vi].base   = (uint64_t)pages;
        vma_table[vi].pages  = npages;
        vma_table[vi].prot   = (uint32_t)prot;
        vma_table[vi].in_use = true;

        return (int64_t)(uint64_t)pages;
    }

    /* Anonymous mapping */
    if (fd != (uint64_t)-1 && !(flags & MAP_ANONYMOUS))
        return -EBADF;

    /* Round up to page boundary */
    uint64_t npages = (length + 4095) / 4096;

    /* Find free VMA slot */
    int vi = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) { vi = i; break; }
    }
    if (vi < 0) return -ENOMEM;

    /* PROT_NONE: reserve virtual address space without allocating pages.
     * Used by PartitionAlloc, jemalloc, etc. to reserve large VA pools.
     * Pages are allocated later via mprotect(PROT_READ|PROT_WRITE).
     * Honor addr hint if given — PA needs specific alignment. */
    if (prot == 0 /* PROT_NONE */) {
        static uint64_t reserve_base = 0x500000000ULL;  /* 20GB — above RAM */
        uint64_t result;
        if (addr && (addr & 0xFFF) == 0) {
            result = addr;  /* Honor the hint (no physical pages, so any VA works) */
        } else {
            result = reserve_base;
            reserve_base += npages * 4096;
        }

        vma_table[vi].base   = result;
        vma_table[vi].pages  = npages;
        vma_table[vi].prot   = 0;
        vma_table[vi].in_use = true;

        return (int64_t)result;
    }

    /* Allocate physical pages */
    void *pages = mem_alloc_pages(npages);
    if (!pages) return -ENOMEM;

    uint64_t base = (uint64_t)pages;

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
            /* Only free physical pages if they were actually allocated
             * (prot != 0). PROT_NONE reservations have no backing pages. */
            if (vma_table[i].prot != 0) {
                /* Unmap page table entries for committed pages */
                for (uint64_t p = 0; p < npages; p++)
                    paging_unmap_page(addr + p * 4096);
            }
            vma_table[i].in_use = false;
            return 0;
        }
    }

    /* Partial unmap: find VMA containing this range */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096;
        if (addr >= vma_table[i].base && addr + npages * 4096 <= vma_end) {
            if (vma_table[i].prot != 0) {
                for (uint64_t p = 0; p < npages; p++)
                    paging_unmap_page(addr + p * 4096);
            }
            vma_table[i].in_use = false;
            return 0;
        }
    }

    return -EINVAL;
}

/* sys_mprotect — change protection flags on mapped pages */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

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

            /* Committing a PROT_NONE reservation: allocate real pages */
            if (vma_table[i].prot == 0 && prot != 0) {
                for (uint64_t p = 0; p < npages; p++) {
                    uint64_t va = addr + p * 4096;
                    void *page = mem_alloc_pages(1);
                    if (!page) return -ENOMEM;
                    memset(page, 0, 4096);
                    paging_map_page(va, (uint64_t)page, pte_flags);
                }
            } else {
                /* Update existing page table entries */
                for (uint64_t p = 0; p < npages; p++) {
                    uint64_t va = addr + p * 4096;
                    paging_set_flags(va, pte_flags);
                }
            }

            vma_table[i].prot = (uint32_t)prot;
            return 0;
        }
    }

    /* If addr is in identity-mapped region (not from mmap),
     * still allow mprotect as a no-op for compatibility */
    return 0;
}

/* sys_mremap — resize an existing mmap region.
 * Q2's Hunk_End calls mremap(base, old_size, new_size, 0) to shrink
 * a reservation to actual usage. We support in-place shrink only:
 * if new_size <= old_size, just update the VMA and return same address.
 * MREMAP_MAYMOVE (flag=1) with shrink still stays in place — we never
 * move, so if caller needs move and new_size > old_size, return ENOMEM. */
static int64_t sys_mremap(uint64_t old_addr, uint64_t old_size,
                          uint64_t new_size, uint64_t flags)
{
    if (!old_addr || (old_addr & 0xFFF)) return -EINVAL;
    if (new_size == 0) return -EINVAL;

    uint64_t old_pages = (old_size + 4095) / 4096;
    uint64_t new_pages = (new_size + 4095) / 4096;

    /* Find matching VMA */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (vma_table[i].base != old_addr) continue;
        /* Allow approximate match (old_size may differ from VMA pages) */
        if (new_pages <= vma_table[i].pages) {
            /* Shrink: release tail pages, update VMA */
            uint64_t free_start = old_addr + new_pages * 4096;
            for (uint64_t p = new_pages; p < vma_table[i].pages; p++)
                paging_unmap_page(free_start + (p - new_pages) * 4096);
            vma_table[i].pages = new_pages;
            return (int64_t)old_addr;
        }
        /* Grow: not supported without MREMAP_MAYMOVE + free space */
        return -ENOMEM;
    }
    return -EFAULT;
}

/* ── Demand paging — called from #PF handler in idt.c ──────────
 * If the faulting address is in a VMA (even PROT_NONE), allocate a
 * physical page and map it. This implements lazy page commitment
 * for mmap(PROT_NONE) reservations used by PartitionAlloc etc.
 * Returns 0 on success (page mapped, resume execution), -1 on failure. */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern void *mem_alloc_pages(uint64_t count);

static uint64_t prot_to_pte_flags(uint32_t prot);

int demand_page_fault(uint64_t addr, uint64_t error_code)
{
    uint64_t page_addr = addr & ~0xFFFULL;

    /* COW: write fault on present read-only page → copy and remap writable */
    if ((error_code & 0x03) == 0x03) { /* present + write fault */
        extern int paging_is_cow(uint64_t virt);
        extern int paging_cow_copy(uint64_t virt);
        if (paging_is_cow(page_addr)) {
            if (paging_cow_copy(page_addr) == 0)
                return 0;  /* COW resolved */
        }
        return -1;  /* not COW — real protection fault */
    }

    /* Not-present fault → demand page (lazy commit for mmap reservations) */
    if (error_code & 1) return -1;  /* present but not write → not our fault */

    /* Don't handle faults in low memory (kernel area) */
    if (addr < 0x100000000ULL) return -1;

    void *page = mem_alloc_pages(1);
    if (!page) return -1;
    memset(page, 0, 4096);

    if (paging_map_page(page_addr, (uint64_t)page, 0x03 /* RW */) != 0)
        return -1;

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

/* ioctl — terminal control */
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Framebuffer dimensions (from framebuffer.c) */
extern uint32_t fb_get_cols(void);
extern uint32_t fb_get_rows(void);

/* Linux termios c_lflag bits */
#define TERMIOS_ECHO    0x0008
#define TERMIOS_ICANON  0x0002
#define TERMIOS_ISIG    0x0001

/* Our tracking of the current termios state */
static uint32_t cur_c_iflag = 0x0500;   /* ICRNL|IXON */
static uint32_t cur_c_oflag = 0x0005;   /* OPOST|ONLCR */
static uint32_t cur_c_cflag = 0x00B2;   /* CS8|CREAD|HUPCL */
static uint32_t cur_c_lflag = 0x8A3B;   /* ISIG|ICANON|ECHO|... */

/* termios struct layout (matches Linux kernel_termios) */
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed, c_ospeed;
} termios_t;

static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg)
{
    (void)fd;
    switch (request) {
    case TIOCGWINSZ: {
        if (!arg) return -EFAULT;
        struct winsize *ws = (struct winsize *)arg;
        uint32_t cols = fb_get_cols();
        uint32_t rows = fb_get_rows();
        ws->ws_row = rows ? (uint16_t)rows : 25;
        ws->ws_col = cols ? (uint16_t)cols : 80;
        ws->ws_xpixel = ws->ws_col * 8;
        ws->ws_ypixel = ws->ws_row * 16;
        return 0;
    }
    case TIOCSWINSZ:
        return 0;  /* ignore set */
    case TCGETS: {
        if (!arg) return -EFAULT;
        termios_t *t = (termios_t *)arg;
        memset(t, 0, sizeof(*t));
        t->c_iflag  = cur_c_iflag;
        t->c_oflag  = cur_c_oflag;
        t->c_cflag  = cur_c_cflag;
        t->c_lflag  = cur_c_lflag;
        t->c_ispeed = 38400;
        t->c_ospeed = 38400;
        t->c_cc[0]  = 3;   /* VINTR = Ctrl-C */
        t->c_cc[1]  = 28;  /* VQUIT */
        t->c_cc[4]  = 1;   /* VMIN */
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!arg) return -EFAULT;
        termios_t *t = (termios_t *)arg;
        cur_c_iflag = t->c_iflag;
        cur_c_oflag = t->c_oflag;
        cur_c_cflag = t->c_cflag;
        cur_c_lflag = t->c_lflag;
        /* Update actual terminal behavior */
        term_canonical = !!(cur_c_lflag & TERMIOS_ICANON);
        term_echo      = !!(cur_c_lflag & TERMIOS_ECHO);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/* poll — check fd readiness */
#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

static int poll_check(struct pollfd *fds, uint64_t nfds)
{
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0 || (uint64_t)fds[i].fd >= MAX_FDS ||
            !fd_table[fds[i].fd].open) {
            fds[i].revents = POLLNVAL;
            continue;
        }
        fd_entry_t *f = &fd_table[fds[i].fd];
        /* Console stdin: POLLIN if keyboard has data */
        if (f->type == FD_TYPE_CONSOLE && f->read) {
            if ((fds[i].events & POLLIN) && kb_has_input())
                fds[i].revents |= POLLIN;
        }
        /* Console stdout/stderr: always writable */
        if (f->type == FD_TYPE_CONSOLE && f->write) {
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
        }
        /* Files/pipes: always ready */
        if (f->type == FD_TYPE_FILE || f->type == FD_TYPE_PIPE)
            fds[i].revents |= (fds[i].events & (POLLIN | POLLOUT));
        if (fds[i].revents) ready++;
    }
    return ready;
}

extern uint64_t idt_get_ticks(void);

static int64_t sys_poll(uint64_t fds_addr, uint64_t nfds, uint64_t timeout_ms)
{
    if (!fds_addr || nfds == 0) return 0;
    struct pollfd *fds = (struct pollfd *)fds_addr;

    int ready = poll_check(fds, nfds);
    if (ready > 0 || (int64_t)timeout_ms == 0)
        return ready;

    /* Block until data or timeout */
    uint64_t start = idt_get_ticks();
    int64_t tmo = (int64_t)timeout_ms;
    uint64_t max_ticks = (tmo < 0) ? 0xFFFFFFFFFFFFFFFFULL :
                          (uint64_t)tmo / 10;  /* 100Hz timer */
    for (;;) {
        __asm__ volatile ("sti; hlt; cli");
        ready = poll_check(fds, nfds);
        if (ready > 0) return ready;
        uint64_t elapsed = idt_get_ticks() - start;
        if (tmo >= 0 && elapsed >= max_ticks)
            return 0;  /* timeout */
    }
}

/* access — check if file exists */
extern int osfs2_delete(const char *name);

static int64_t sys_access(uint64_t path_addr, uint64_t mode)
{
    (void)mode;
    const char *path = (const char *)path_addr;
    if (!path) return -EFAULT;
    if (osfs2_find(path)) return 0;
    /* Try without leading slash */
    if (path[0] == '/' && osfs2_find(path + 1)) return 0;
    /* Try basename */
    const char *bn = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') bn = p + 1;
    if (bn != path && *bn && osfs2_find(bn)) return 0;
    /* Case-insensitive fallback */
    if (osfs2_find_ci(path)) return 0;
    if (path[0] == '/' && osfs2_find_ci(path + 1)) return 0;
    if (bn != path && *bn && osfs2_find_ci(bn)) return 0;
    /* Virtual paths that always "exist" */
    if (str_startswith(path, "/dev/") || str_startswith(path, "/proc/"))
        return 0;
    return -ENOENT;
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
    if (sig == 0) return 0;  /* Signal 0 = test if process exists */

    uint32_t target_pid = (uint32_t)pid;
    if (pid == 0) target_pid = (uint32_t)proc_current_pid(); /* signal self */

    /* SIGKILL/SIGTERM on self → immediate exit */
    if (target_pid == (uint32_t)proc_current_pid() &&
        (sig == SIGKILL || sig == SIGTERM)) {
        proc_exit(128 + (int32_t)sig);
    }

    /* Queue signal on target process */
    proc_signal_pid(target_pid, (int)sig);

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
    void *proc = proc_current();
    if (!proc) return -ESRCH;
    sig_act_t *actions = proc_get_sig_actions(proc);

    if (oldact_addr) {
        sigaction_t *old = (sigaction_t *)oldact_addr;
        memset(old, 0, sizeof(*old));
        old->sa_handler = actions[sig].handler;
        old->sa_flags   = actions[sig].flags;
        old->sa_restorer = actions[sig].restorer;
    }

    if (act_addr) {
        const sigaction_t *act = (const sigaction_t *)act_addr;
        actions[sig].handler  = act->sa_handler;
        actions[sig].flags    = act->sa_flags;
        actions[sig].restorer = act->sa_restorer;
    }

    return 0;
}

/* ── Check and deliver pending signals ─────────────────────── */

void syscall_check_signals(void)
{
    void *proc = proc_current();
    if (!proc) return;

    uint64_t *pending = proc_get_sig_pending_ptr(proc);
    uint64_t *mask = proc_get_sig_mask_ptr(proc);
    sig_act_t *actions = proc_get_sig_actions(proc);

    uint64_t deliverable = *pending & ~(*mask);
    if (!deliverable) return;

    for (uint32_t s = 1; s < NSIG; s++) {
        if (!(deliverable & (1ULL << s))) continue;
        *pending &= ~(1ULL << s);

        uint64_t handler = actions[s].handler;

        if (handler == SIG_IGN) continue;
        if (s == SIGCHLD && handler == SIG_DFL) continue; /* SIGCHLD default = ignore */

        if (handler == SIG_DFL) {
            if (s == SIGINT || s == SIGTERM || s == SIGPIPE || s == SIGKILL) {
                serial_puts("[SIGNAL] Delivering signal ");
                serial_putdec(s);
                serial_puts(" (default: terminate)\n");
                proc_exit(128 + (int32_t)s);
            }
            continue;
        }

        /* Custom handler — synchronous delivery (v1) */
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
    case ARCH_SET_FS: {
        extern void proc_set_fs_base(uint64_t addr);
        serial_puts("[TLS] arch_prctl SET_FS=0x");
        serial_puthex(addr, 16);
        serial_puts("\n");
        wrmsr(MSR_FS_BASE, addr);
        proc_set_fs_base(addr);
        return 0;
    }
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

/* set_tid_address — set pointer for child tid notification (X-THREAD) */
extern void proc_set_clear_child_tid(uint64_t *addr);
static int64_t sys_set_tid_address(uint64_t tidptr)
{
    proc_set_clear_child_tid((uint64_t *)tidptr);
    return (int64_t)proc_current_pid();  /* Return current TID */
}

/* getpid / gettid — return process/thread ID (X-THREAD) */
extern int32_t proc_current_tgid(void);
static int64_t sys_getpid(void)
{
    /* getpid returns TGID — all threads in a group see the same PID */
    return (int64_t)proc_current_tgid();
}

static int64_t sys_gettid(void)
{
    /* gettid returns the thread's unique TID (= PID in process table) */
    return (int64_t)proc_current_pid();
}

/* rt_sigprocmask — block/unblock signals */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

static int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_addr,
                                   uint64_t oldset_addr, uint64_t sigsetsize)
{
    (void)sigsetsize;
    void *proc = proc_current();
    if (!proc) return -ESRCH;
    uint64_t *mask = proc_get_sig_mask_ptr(proc);

    /* Return old mask if requested */
    if (oldset_addr) {
        uint64_t *oldset = (uint64_t *)oldset_addr;
        *oldset = *mask;
    }

    if (set_addr) {
        uint64_t new_set = *(uint64_t *)set_addr;
        /* Cannot block SIGKILL(9) or SIGSTOP(19) */
        new_set &= ~((1ULL << 9) | (1ULL << 19));
        switch (how) {
        case SIG_BLOCK:   *mask |= new_set;  break;
        case SIG_UNBLOCK: *mask &= ~new_set; break;
        case SIG_SETMASK: *mask = new_set;   break;
        default: return -EINVAL;
        }
    }
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

    uint64_t ticks = idt_get_ticks();
    uint64_t ms = ticks * 10;  /* 100Hz → 10ms per tick */

    tp->tv_sec  = (int64_t)(ms / 1000);
    tp->tv_nsec = (int64_t)((ms % 1000) * 1000000);

    if (clk_id == CLOCK_REALTIME)
        tp->tv_sec += (int64_t)boot_epoch_sec;

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

/* Futex — real wait queue implementation (X-THREAD) */
extern int futex_do_wait(uint64_t uaddr, int expected);
extern int futex_do_wake(uint64_t uaddr, int count);

static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                          uint64_t timeout, uint64_t uaddr2)
{
    (void)timeout; (void)uaddr2;
    int cmd = (int)(op & ~FUTEX_PRIVATE_FLAG);

    if (cmd == FUTEX_WAIT) {
        return (int64_t)futex_do_wait(uaddr, (int)val);
    }
    if (cmd == FUTEX_WAKE) {
        return (int64_t)futex_do_wake(uaddr, (int)val);
    }
    /* FUTEX_REQUEUE, etc. — stub for now */
    return 0;
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
    strcpy(u->release, "1.0.0-osito");
    strcpy(u->version, "OsitoK x86-64");
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
/* Clone flags (from Linux uapi) */
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_THREAD    0x00010000
#define CLONE_SYSVSEM   0x00040000
#define CLONE_SETTLS    0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

extern int32_t proc_clone_thread(uint64_t child_stack, uint64_t parent_tidptr,
                                 uint64_t child_tidptr, uint64_t tls);

static int64_t sys_clone(uint64_t flags, uint64_t child_stack,
                          uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    /* Thread creation: CLONE_VM | CLONE_THREAD (+ usually CLONE_SIGHAND etc.) */
    if (flags & CLONE_THREAD) {
        if (!child_stack) return -22; /* EINVAL: thread requires stack */
        int32_t tid = proc_clone_thread(
            child_stack,
            (flags & CLONE_PARENT_SETTID) ? ptid : 0,
            (flags & CLONE_CHILD_CLEARTID) ? ctid : 0,
            (flags & CLONE_SETTLS) ? tls : 0);
        return (int64_t)tid;
    }

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

extern char cwd[256];  /* defined below in sys_chdir section */

static int64_t sys_getcwd(uint64_t buf_addr, uint64_t size)
{
    if (!buf_addr || size < 2) return -EINVAL;
    uint64_t len = 0;
    while (cwd[len]) len++;
    if (len + 1 > size) return -ERANGE;
    memcpy((char *)buf_addr, cwd, len + 1);
    return (int64_t)buf_addr;
}

static int64_t sys_readlink(uint64_t path_addr, uint64_t buf_addr, uint64_t bufsiz)
{
    const char *path = (const char *)path_addr;
    char *buf = (char *)buf_addr;
    if (!path || !buf || bufsiz == 0) return -EFAULT;

    /* /proc/self/exe → return path to current binary */
    if (str_startswith(path, "/proc/self/exe") ||
        str_startswith(path, "/proc/") /* /proc/<pid>/exe */) {
        const char *name = proc_current_name();
        if (!name) name = "unknown";
        /* Return "/name" as path — needed for busybox applet re-exec */
        char tmp[128];
        tmp[0] = '/';
        uint64_t nlen = strlen(name);
        if (nlen > 126) nlen = 126;
        memcpy(tmp + 1, name, (size_t)nlen);
        uint64_t len = 1 + nlen;
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, tmp, (size_t)len);
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

/* ── Trivial POSIX stubs for busybox/musl compatibility ──────── */

extern uint64_t idt_get_ticks(void);

static uint64_t current_umask = 022;
char cwd[256] = "/";

static int64_t sys_pause(void)
{
    /* Sleep until signal — just do a short sleep and return -EINTR */
    __asm__ volatile ("sti; hlt");
    return -4;  /* EINTR */
}

static int64_t sys_chdir(uint64_t path_addr)
{
    const char *p = (const char *)path_addr;
    if (!p) return -EFAULT;

    /* Build new cwd */
    char newcwd[256];
    if (p[0] == '/') {
        /* Absolute path */
        int i = 0;
        while (p[i] && i < 254) { newcwd[i] = p[i]; i++; }
        if (i > 1 && newcwd[i-1] != '/') { newcwd[i++] = '/'; }
        newcwd[i] = '\0';
    } else {
        /* Relative: append to cwd */
        int ci = 0;
        while (cwd[ci]) { newcwd[ci] = cwd[ci]; ci++; }
        int pi = 0;
        while (p[pi] && ci < 254) { newcwd[ci++] = p[pi++]; }
        if (ci > 1 && newcwd[ci-1] != '/') newcwd[ci++] = '/';
        newcwd[ci] = '\0';
    }

    memcpy(cwd, newcwd, 256);
    return 0;
}

/* ── epoll: minimal implementation using poll semantics ─────── */

#define EPOLL_MAX_FDS  32

/* epoll instance: tracks watched FDs */
typedef struct {
    int    fds[EPOLL_MAX_FDS];
    uint32_t events[EPOLL_MAX_FDS];
    int    count;
} epoll_state_t;

static epoll_state_t epoll_instances[8];
static int epoll_count;

static int64_t sys_epoll_create1(uint64_t flags)
{
    (void)flags;
    int newfd = vfs_alloc_fd();
    if (newfd < 0) return -EMFILE;
    if (epoll_count >= 8) return -ENOMEM;

    int ep_idx = epoll_count++;
    memset(&epoll_instances[ep_idx], 0, sizeof(epoll_state_t));

    fd_entry_t *f = &fd_table[newfd];
    memset(f, 0, sizeof(*f));
    f->open = true;
    f->type = FD_TYPE_EPOLL;
    f->offset = (uint64_t)ep_idx;
    return newfd;
}

static int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op,
                              uint64_t fd, uint64_t event_addr)
{
    fd_entry_t *ef = &fd_table[epfd & 0xFF];
    if (!ef->open || ef->type != FD_TYPE_EPOLL) return -EBADF;
    epoll_state_t *ep = &epoll_instances[ef->offset];

    if (op == 1 /* EPOLL_CTL_ADD */) {
        if (ep->count >= EPOLL_MAX_FDS) return -ENOMEM;
        ep->fds[ep->count] = (int)fd;
        if (event_addr) ep->events[ep->count] = *(uint32_t *)event_addr;
        ep->count++;
    } else if (op == 2 /* EPOLL_CTL_DEL */) {
        for (int i = 0; i < ep->count; i++) {
            if (ep->fds[i] == (int)fd) {
                ep->fds[i] = ep->fds[ep->count - 1];
                ep->events[i] = ep->events[ep->count - 1];
                ep->count--;
                break;
            }
        }
    }
    return 0;
}

static int64_t sys_epoll_wait(uint64_t epfd, uint64_t events_addr,
                               uint64_t maxevents, uint64_t timeout_ms)
{
    fd_entry_t *ef = &fd_table[epfd & 0xFF];
    if (!ef->open || ef->type != FD_TYPE_EPOLL) return -EBADF;
    epoll_state_t *ep = &epoll_instances[ef->offset];

    /* Simple implementation: check each watched FD for readability.
     * For pipes/files with data, report EPOLLIN. */
    struct { uint32_t events; uint64_t data; } __attribute__((packed)) *out =
        (void *)events_addr;
    int ready = 0;

    for (int i = 0; i < ep->count && ready < (int)maxevents; i++) {
        int wfd = ep->fds[i];
        if (wfd < 0 || wfd >= MAX_FDS) continue;
        fd_entry_t *wf = &fd_table[wfd];
        if (!wf->open) continue;

        /* Pipes: check if data available */
        bool has_data = false;
        if (wf->type == FD_TYPE_PIPE) {
            pipe_buf_t *p = (pipe_buf_t *)wf->file;
            has_data = (p && p->count > 0);
        } else if (wf->type == FD_TYPE_EVENTFD) {
            has_data = (wf->offset > 0);  /* eventfd counter > 0 */
        } else if (wf->type == FD_TYPE_FILE || wf->type == FD_TYPE_TMPFS) {
            has_data = true;  /* Files always readable */
        }

        if (has_data) {
            out[ready].events = 1; /* EPOLLIN */
            out[ready].data = (uint64_t)wfd;
            ready++;
        }
    }

    /* If no events and timeout > 0, sleep briefly */
    if (ready == 0 && timeout_ms > 0) {
        uint64_t end = idt_get_ticks() + (timeout_ms / 10);
        while (idt_get_ticks() < end) {
            __asm__ volatile ("hlt");
            /* Re-check (simplified — full impl would re-scan) */
        }
    }

    return (int64_t)ready;
}

/* ── eventfd: simple counter-based IPC ───────────────────────── */

static int64_t sys_eventfd2(uint64_t initval, uint64_t flags)
{
    (void)flags;
    int newfd = vfs_alloc_fd();
    if (newfd < 0) return -EMFILE;

    fd_entry_t *f = &fd_table[newfd];
    memset(f, 0, sizeof(*f));
    f->open = true;
    f->type = FD_TYPE_EVENTFD;
    f->offset = initval;  /* Use offset as the counter */
    return newfd;
}

static int64_t sys_fchdir(uint64_t fd)
{
    (void)fd;
    return 0;  /* pretend success */
}

static int64_t sys_umask(uint64_t mask)
{
    uint64_t old = current_umask;
    current_umask = mask & 0777;
    return (int64_t)old;
}

static int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr)
{
    (void)tz_addr;
    if (tv_addr) {
        uint64_t ticks = idt_get_ticks();
        uint64_t secs = boot_epoch_sec + ticks / 100;
        uint64_t usecs = (ticks % 100) * 10000;
        uint64_t *tv = (uint64_t *)tv_addr;
        tv[0] = secs;    /* tv_sec */
        tv[1] = usecs;   /* tv_usec */
    }
    return 0;
}

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
};

extern uint64_t mem_get_free(void);
extern uint64_t mem_get_total(void);

static int64_t sys_sysinfo(uint64_t info_addr)
{
    struct linux_sysinfo *si = (struct linux_sysinfo *)info_addr;
    memset(si, 0, sizeof(*si));
    si->uptime = (int64_t)(idt_get_ticks() / 100);
    si->totalram = mem_get_total();
    si->freeram = mem_get_free();
    si->procs = 1;
    si->mem_unit = 1;
    return 0;
}

struct linux_statfs {
    int64_t f_type;
    int64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
};

static int64_t sys_statfs(uint64_t path_addr, uint64_t buf_addr)
{
    (void)path_addr;
    struct linux_statfs *st = (struct linux_statfs *)buf_addr;
    memset(st, 0, sizeof(*st));
    st->f_type = 0x4F534654;  /* "OSFT" magic */
    st->f_bsize = 4096;
    st->f_blocks = mem_get_total() / 4096;
    st->f_bfree = mem_get_free() / 4096;
    st->f_bavail = st->f_bfree;
    st->f_namelen = 64;
    st->f_frsize = 4096;
    return 0;
}

static int64_t sys_setpgid(uint64_t pid, uint64_t pgid)
{
    (void)pid; (void)pgid;
    return 0;  /* pretend success */
}

/* statx — modern stat replacement. Fill from fstat data. */
struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    /* timestamps follow but we zero them */
    uint8_t  __rest[128];
};

static int64_t sys_statx(uint64_t dirfd, uint64_t path_addr,
                          uint64_t flags, uint64_t mask, uint64_t buf_addr)
{
    (void)flags; (void)mask;
    struct linux_statx *sx = (struct linux_statx *)buf_addr;
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = 0x7FF;  /* STATX_BASIC_STATS */
    sx->stx_blksize = 4096;
    sx->stx_nlink = 1;

    /* Use fstat internally if we have an fd */
    const char *path = (const char *)path_addr;
    if (path && path[0]) {
        /* Try basename for flat FS */
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        if (*base) {
            void *f = osfs2_find(base);
            if (f) {
                sx->stx_size = osfs2_file_size(f);
                sx->stx_mode = 0100644;  /* S_IFREG|0644 */
                sx->stx_blocks = (sx->stx_size + 511) / 512;
                return 0;
            }
        }
        /* Check /dev/, /proc/ prefixes */
        if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') {
            sx->stx_mode = 020666;  /* S_IFCHR|0666 */
            return 0;
        }
        if (path[0] == '/' && path[1] == 'p' && path[2] == 'r') {
            sx->stx_mode = 0100444;  /* S_IFREG|0444 */
            return 0;
        }
        if (path[0] == '/' && path[1] == '\0') {
            sx->stx_mode = 040755;  /* S_IFDIR|0755 */
            return 0;
        }
    }
    return -ENOENT;
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
    case SYS_POLL:       return sys_poll(a1, a2, a3);
    case SYS_LSEEK:      return sys_lseek(a1, (int64_t)a2, a3);
    case SYS_MMAP:       return sys_mmap(a1, a2, a3, a4, a5, 0);
    case SYS_MPROTECT:   return sys_mprotect(a1, a2, a3);
    case SYS_MUNMAP:     return sys_munmap(a1, a2);
    case SYS_MREMAP:     return sys_mremap(a1, a2, a3, a4);
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
    case SYS_SELECT:     return sys_poll(0, 0, 0);  /* stub: pretend nothing ready */
    case SYS_CLONE:      return sys_clone(a1, a2, a3, a4, a5);
    case SYS_FORK:       return sys_clone(17 /* SIGCHLD */, 0, 0, 0, 0);
    case SYS_VFORK:      return sys_clone(17 /* SIGCHLD */, 0, 0, 0, 0);
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
    case SYS_RT_SIGSUSPEND: return -EINTR;  /* pretend a signal interrupted */
    case SYS_SIGALTSTACK: return sys_sigaltstack(a1, a2);
    case SYS_PRCTL:      return sys_prctl(a1, a2, a3, a4, a5);
    case SYS_ARCH_PRCTL: return sys_arch_prctl(a1, a2);
    case SYS_GETTID:     return sys_gettid();
    case SYS_FUTEX:      return sys_futex(a1, a2, a3, a4, a5);
    case SYS_GETDENTS64: return sys_getdents64(a1, a2, a3);
    case SYS_SET_TID_ADDR: return sys_set_tid_address(a1);
    case SYS_CLOCK_GETTIME: return sys_clock_gettime(a1, a2);
    case SYS_EXIT_GROUP: return sys_exit_group(a1);
    case SYS_TKILL:      return sys_kill(a1, a2);  /* tkill(tid, sig) */
    case SYS_TGKILL:     return sys_kill(a2, a3);  /* tgkill(tgid, tid, sig) */
    case SYS_OPENAT:     return sys_openat(a1, a2, a3, a4);
    case SYS_NEWFSTATAT: return sys_newfstatat(a1, a2, a3, a4);
    case SYS_READLINKAT: return sys_readlinkat(a1, a2, a3, a4);
    case SYS_PPOLL:      return sys_poll(a1, a2, -1);  /* ignore sigmask/timeout */
    case SYS_SET_ROBUST_LIST: return sys_set_robust_list(a1, a2);
    case SYS_DUP3:       return sys_dup3(a1, a2, a3);
    case SYS_PIPE2:      return sys_pipe2(a1, a2);
    case SYS_PRLIMIT64:  return sys_prlimit64(a1, a2, a3, a4);
    case SYS_GETRANDOM:  return sys_getrandom(a1, a2, a3);
    case SYS_RSEQ:       return -ENOSYS;  /* musl handles gracefully */
    case SYS_CLOSE_RANGE: return sys_close_range(a1, a2, a3);

    /* ── Trivial POSIX stubs (busybox compatibility) ───────── */
    case SYS_PAUSE:      return sys_pause();
    case SYS_CHDIR:      return sys_chdir(a1);
    case SYS_FCHDIR:     return sys_fchdir(a1);
    case SYS_RENAME: {
        /* Rename: only supported for tmpfs (/tmp/ prefix) */
        const char *oldpath = (const char *)a1;
        const char *newpath = (const char *)a2;
        if (oldpath && newpath &&
            str_startswith(oldpath, "/tmp/") && str_startswith(newpath, "/tmp/")) {
            extern void *tmpfs_open(const char *name);
            extern void *tmpfs_create(const char *name);
            extern int tmpfs_delete(const char *name);
            /* Create new, copy content reference, delete old */
            /* For now: just return success (tmpfs files are ephemeral) */
        }
        return 0;  /* pretend success for compatibility */
    }
    case SYS_MKDIR:
        /* mkdir: no-op success for /tmp (flat FS, dirs implicit) */
        return 0;
    case SYS_RMDIR:      return 0;
    case SYS_CHMOD:      return 0;   /* pretend success */
    case SYS_FCHMOD:     return 0;
    case SYS_CHOWN:      return 0;
    case SYS_FCHOWN:     return 0;
    case SYS_UMASK:      return sys_umask(a1);
    case SYS_GETTIMEOFDAY: return sys_gettimeofday(a1, a2);
    case 201: { /* time(time_t *) — return seconds since epoch */
        int64_t secs = sys_clock_gettime(0 /* CLOCK_REALTIME */, 0);
        if (secs < 0) secs = 0;
        /* clock_gettime with buf=0 just returns; use gettimeofday */
        struct { uint64_t tv_sec; uint64_t tv_usec; } tv;
        sys_gettimeofday((uint64_t)&tv, 0);
        if (a1) *(int64_t *)a1 = (int64_t)tv.tv_sec;
        return (int64_t)tv.tv_sec;
    }
    case SYS_GETRUSAGE: {
        /* getrusage(who, usage) — zero-fill struct rusage (144 bytes on x86-64) */
        if (a2) memset((void *)a2, 0, 144);
        return 0;
    }
    case SYS_GETRLIMIT:  return sys_prlimit64(0, a1, 0, a2);
    case SYS_SYSINFO:    return sys_sysinfo(a1);
    case SYS_TIMES:      return -1;  /* return -1 = no times data */
    case SYS_SETPGID:    return sys_setpgid(a1, a2);
    case SYS_GETPGID:    return (int64_t)proc_current_pid();
    case SYS_GETRESUID:  if (a1) *(uint32_t *)a1 = 0;
                         if (a2) *(uint32_t *)a2 = 0;
                         if (a3) *(uint32_t *)a3 = 0;
                         return 0;
    case SYS_GETRESGID:  if (a1) *(uint32_t *)a1 = 0;
                         if (a2) *(uint32_t *)a2 = 0;
                         if (a3) *(uint32_t *)a3 = 0;
                         return 0;
    case SYS_STATFS:     return sys_statfs(a1, a2);
    case SYS_FSTATFS:    return sys_statfs(0, a2);  /* reuse */
    case SYS_SETRLIMIT:  return 0;   /* pretend success */
    case SYS_SYNC:       return 0;   /* no-op */
    case SYS_TRUNCATE:   return 0;  /* pretend success */
    case SYS_FTRUNCATE:  return 0;  /* pretend success */
    case SYS_WAITID:     return sys_wait4(-1, a3, (uint64_t)(int)a4, 0);
    case SYS_UNLINKAT:   return sys_unlink(a2);  /* ignore dirfd */
    case SYS_MKDIRAT:    return 0;  /* pretend success (flat FS) */
    case SYS_FCHOWNAT:   return 0;
    case SYS_FCHMODAT:   return 0;
    case SYS_FACCESSAT:  return sys_access(a2, a3);  /* ignore dirfd */
    case SYS_FACCESSAT2: return sys_access(a2, a3);
    case SYS_PSELECT6:   return sys_poll(0, 0, 0);
    case SYS_UTIMENSAT:  return 0;   /* pretend success */
    /* ── Socket syscalls ──────────────────────────────────────── */
    case SYS_SOCKET: {
        extern int sock_socket(int, int, int);
        int si = sock_socket((int)a1, (int)a2, (int)a3);
        if (si < 0) return (int64_t)si;
        int nfd = vfs_alloc_fd();
        if (nfd < 0) return -EMFILE;
        fd_entry_t *sf = &fd_table[nfd];
        memset(sf, 0, sizeof(*sf));
        sf->open = true; sf->type = FD_TYPE_SOCKET;
        sf->offset = (uint64_t)si;  /* socket index */
        return nfd;
    }
    case SYS_BIND: {
        extern int sock_bind(int, const void *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_bind((int)sf->offset, (const void *)a2);
    }
    case SYS_LISTEN: {
        extern int sock_listen(int, int);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_listen((int)sf->offset, (int)a2);
    }
    case SYS_ACCEPT:
    case SYS_ACCEPT4: {
        extern int sock_accept(int, void *, uint32_t *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        int ni = sock_accept((int)sf->offset, (void *)a2, (uint32_t *)a3);
        if (ni < 0) return (int64_t)ni;
        int nfd = vfs_alloc_fd();
        if (nfd < 0) return -EMFILE;
        fd_entry_t *nf = &fd_table[nfd];
        memset(nf, 0, sizeof(*nf));
        nf->open = true; nf->type = FD_TYPE_SOCKET;
        nf->offset = (uint64_t)ni;
        return nfd;
    }
    case SYS_CONNECT: {
        extern int sock_connect(int, const void *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_connect((int)sf->offset, (const void *)a2);
    }
    case SYS_SENDTO: {
        extern int sock_sendto(int, const void *, uint32_t, int, const void *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        if (a5) return sock_sendto((int)sf->offset, (const void *)a2, (uint32_t)a3, (int)a4, (const void *)a5);
        extern int sock_send(int, const void *, uint32_t, int);
        return sock_send((int)sf->offset, (const void *)a2, (uint32_t)a3, (int)a4);
    }
    case SYS_RECVFROM: {
        extern int sock_recvfrom(int, void *, uint32_t, int, void *, uint32_t *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_recvfrom((int)sf->offset, (void *)a2, (uint32_t)a3, (int)a4, (void *)a5, NULL);
    }
    case SYS_SHUTDOWN: return 0;
    case SYS_GETSOCKNAME: {
        extern int sock_getsockname(int, void *, uint32_t *);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_getsockname((int)sf->offset, (void *)a2, (uint32_t *)a3);
    }
    case SYS_GETPEERNAME: return 0;
    case SYS_SETSOCKOPT: {
        extern int sock_setsockopt(int, int, int, const void *, uint32_t);
        fd_entry_t *sf = &fd_table[a1 & 0xFF];
        if (!sf->open || sf->type != FD_TYPE_SOCKET) return -EBADF;
        return sock_setsockopt((int)sf->offset, (int)a2, (int)a3, (const void *)a4, (uint32_t)a5);
    }
    case SYS_GETSOCKOPT: return 0;
    case SYS_SENDMSG:    return -ENOSYS;
    case SYS_RECVMSG:    return -ENOSYS;
    /* ── Timer syscalls ──────────────────────────────────────── */
    case SYS_ALARM: {
        extern uint32_t timer_alarm(uint32_t);
        return (int64_t)timer_alarm((uint32_t)a1);
    }
    case SYS_SETITIMER: {
        extern int timer_setitimer(int, const void *, void *);
        return timer_setitimer((int)a1, (const void *)a2, (void *)a3);
    }
    case SYS_GETITIMER: return 0;
    /* ── I/O port access ─────────────────────────────────────── */
    case SYS_IOPL:    return 0;  /* Grant full I/O privilege (bare-metal: always allowed) */
    case SYS_IOPERM:  return 0;  /* Same — all ports accessible */
    /* ── System V IPC ────────────────────────────────────────── */
    case SYS_SHMGET: {
        extern int sysv_shmget(int, uint64_t, int);
        return sysv_shmget((int)a1, a2, (int)a3);
    }
    case SYS_SHMAT: {
        extern void *sysv_shmat(int, const void *, int);
        return (int64_t)(uint64_t)sysv_shmat((int)a1, (const void *)a2, (int)a3);
    }
    case SYS_SHMDT: {
        extern int sysv_shmdt(const void *);
        return sysv_shmdt((const void *)a1);
    }
    case SYS_SHMCTL: {
        extern int sysv_shmctl(int, int, void *);
        return sysv_shmctl((int)a1, (int)a2, (void *)a3);
    }
    case SYS_SEMGET: {
        extern int sysv_semget(int, int, int);
        return sysv_semget((int)a1, (int)a2, (int)a3);
    }
    case SYS_SEMOP: {
        extern int sysv_semop(int, void *, uint32_t);
        return sysv_semop((int)a1, (void *)a2, (uint32_t)a3);
    }
    case SYS_SEMCTL: {
        extern int sysv_semctl(int, int, int);
        return sysv_semctl((int)a1, (int)a2, (int)a3);
    }
    case SYS_EPOLL_CREATE1: return sys_epoll_create1(a1);
    case SYS_EPOLL_CTL:     return sys_epoll_ctl(a1, a2, a3, a4);
    case SYS_EPOLL_WAIT:    return sys_epoll_wait(a1, a2, a3, a4);
    case SYS_EPOLL_PWAIT:   return sys_epoll_wait(a1, a2, a3, a4);
    case SYS_EVENTFD2:      return sys_eventfd2(a1, a2);
    case SYS_RENAMEAT2:  return -ENOSYS;
    case SYS_STATX:      return sys_statx(a1, a2, a3, a4, a5);

    /* ── OsitoK private: shared memory ────────────────────────── */
    case SYS_SHM_CREATE:
        return shm_create ? (int64_t)shm_create(a1, (uint32_t)a2) : -ENOSYS;
    case SYS_SHM_MAP:
        return shm_map ? (int64_t)(uint64_t)shm_map((uint32_t)a1) : -ENOSYS;
    case SYS_SHM_UNMAP:
        if (shm_unmap) { shm_unmap((uint32_t)a1); return 0; }
        return -ENOSYS;
    case SYS_SHM_DESTROY:
        if (shm_destroy) { shm_destroy((uint32_t)a1); return 0; }
        return -ENOSYS;
    case SYS_SHM_GETPHYS:
        return shm_get_phys ? (int64_t)shm_get_phys((uint32_t)a1) : -ENOSYS;
    case SYS_SHM_GETSIZE:
        return shm_get_size ? (int64_t)shm_get_size((uint32_t)a1) : -ENOSYS;
    case SYS_SHM_MKSURFACE:
        {
            /* Set owner PID so compositor_cleanup_process() can find the window */
            extern uint32_t shm_surface_owner_pid;
            extern uint32_t proc_exec_pid(void);
            shm_surface_owner_pid = proc_exec_pid();
            /* Capture keyboard: route events to input_events only, not kb_buf.
             * The focused graphical process (Q2, game) reads via SYS_GET_INPUT_EVENT. */
            extern void kbd_set_captured(bool);
            kbd_set_captured(true);
            return shm_create_surface ? (int64_t)shm_create_surface((uint32_t)a1, (uint32_t)a2, (uint32_t)a3) : -ENOSYS;
        }
    case SYS_GUI_FLIP:
        if (shm_flush_surface) { shm_flush_surface((uint32_t)a1); return 0; }
        return -ENOSYS;
    case SYS_GET_INPUT_EVENT:
        {
            extern void xhci_poll(void) __attribute__((weak));
            if (xhci_poll) xhci_poll();
            if (input_pop_event && a1) {
                return input_pop_event((void *)a1) ? 1 : 0;
            }
        }
        return -ENOSYS;

    /* ── OsitoK private: QoS scheduler ────────────────────────── */
    case SYS_SCHED_SETQOS:
        return sched_set_qos ? (int64_t)sched_set_qos((uint32_t)a1, (uint8_t)a2) : -ENOSYS;
    case SYS_SCHED_GETQOS:
        return sched_get_qos ? (int64_t)sched_get_qos((uint32_t)a1) : -ENOSYS;

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

/* ── Save/restore brk state for fork+execve ──────────────────── */
/*
 * In an identity-mapped OS, the brk globals are shared between parent
 * and child. When a forked child calls execve, syscall_reset_process
 * frees the brk heap — destroying the parent's malloc state. We save
 * the parent's brk pointers before the child's execve and restore them
 * after the child is reaped in proc_wait4.
 */

/* Saved parent state — all per-process globals that syscall_reset_process
 * would destroy when the forked child calls execve. */
static struct {
    uint8_t  *brk_base;
    uint8_t  *brk_current;
    uint8_t  *brk_max;
    uint64_t  fs_base;
    fd_entry_t fds[MAX_FDS];
    uint64_t  sigs[NSIG];
    uint32_t  sig_pend;
    vma_t     vmas[MAX_VMAS];
    bool      valid;
} saved_parent;

void syscall_save_brk(void)
{
    saved_parent.brk_base    = brk_base;
    saved_parent.brk_current = brk_current;
    saved_parent.brk_max     = brk_max;
    saved_parent.fs_base     = rdmsr(MSR_FS_BASE);
    memcpy(saved_parent.fds,  fd_table,     sizeof(fd_table));
    memcpy(saved_parent.sigs, sig_handlers, sizeof(sig_handlers));
    saved_parent.sig_pend = sig_pending;
    memcpy(saved_parent.vmas, vma_table,    sizeof(vma_table));
    saved_parent.valid = true;
}

void syscall_restore_brk(void)
{
    if (!saved_parent.valid) return;

    /* Free the child's brk region if it allocated a different one */
    if (brk_base && brk_base != saved_parent.brk_base)
        kfree(brk_base);

    brk_base    = saved_parent.brk_base;
    brk_current = saved_parent.brk_current;
    brk_max     = saved_parent.brk_max;

    /* Restore FS_BASE (TLS segment register) — the child's musl init
     * overwrites this via arch_prctl(ARCH_SET_FS). Without restoring,
     * the parent reads the child's TLS area (errno, malloc context). */
    if (saved_parent.fs_base)
        wrmsr(MSR_FS_BASE, saved_parent.fs_base);

    /* Restore FD table, signal handlers, VMA table */
    memcpy(fd_table,     saved_parent.fds,  sizeof(fd_table));
    memcpy(sig_handlers, saved_parent.sigs, sizeof(sig_handlers));
    sig_pending = saved_parent.sig_pend;
    memcpy(vma_table,    saved_parent.vmas, sizeof(vma_table));

    saved_parent.valid = false;
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

    /* Reset terminal to canonical+echo mode (X-EDIT safety) */
    term_canonical = true;
    term_echo = true;
    cur_c_lflag = 0x8A3B;
    cur_c_iflag = 0x0500;
    cur_c_oflag = 0x0005;

    /* Free brk heap — but NOT if it's the parent's saved brk.
     * saved_parent.valid is true when a forked child is doing execve. */
    if (brk_base && (!saved_parent.valid || brk_base != saved_parent.brk_base)) {
        kfree(brk_base);
    }
    brk_base = NULL;
    brk_current = NULL;
    brk_max = NULL;

    /* Free mmap regions — but NOT the parent's saved regions.
     * When a forked child does execve, saved_parent.valid is true and
     * the vma_table contains the parent's regions. Don't free those. */
    if (!saved_parent.valid) {
        for (int i = 0; i < MAX_VMAS; i++) {
            if (vma_table[i].in_use) {
                mem_free_pages((void *)vma_table[i].base, vma_table[i].pages);
                vma_table[i].in_use = false;
            }
        }
    }
    memset(vma_table, 0, sizeof(vma_table));
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

    boot_epoch_sec = rtc_to_epoch();

    serial_puts("[SYSCALL] Ready (LSTAR=0x");
    serial_puthex((uint64_t)syscall_entry, 16);
    serial_puts(")\n");
    fb_puts(" Syscall: SYSCALL/SYSRET active\n");
}
