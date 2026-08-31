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
#include "../include/paging.h"
#include "../include/hwbp.h"
#include "../fs/vfs.h"
#include "../fs/ositofs3.h"

#define EROFS 30

/* ── External functions ──────────────────────────────────────── */

extern void serial_puts(const char *s);
extern void serial_puthex(uint64_t val, int digits);
extern void serial_putdec(uint64_t val);
extern void serial_putc(char c);
extern void boot_diag_maybe_flush(const char *reason, uint64_t min_bytes,
                                  uint64_t min_ticks) __attribute__((weak));
extern void boot_diag_mark(const char *reason) __attribute__((weak));

extern void fb_puts(const char *s);
extern void fb_putc(char c, uint32_t color);
extern void fb_putdec(uint64_t val);
extern void fb_flush(void);

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

/* OsitoFS */
extern void *osfs2_find(const char *name);
extern void *osfs2_find_ci(const char *name);      /* case-insensitive lookup */
extern const char *osfs2_file_name(void *file);    /* get filename for hint */
extern int   osfs2_read(void *file, uint64_t offset, void *buf, uint64_t len);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_truncate(void *file, uint64_t size);
extern int   osfs2_rename(const char *from, const char *to, bool replace);
extern int   osfs2_file_retain(void *file);
extern void  osfs2_file_release(void *file);
extern void *osfs2_file_at(uint32_t index);
extern uint32_t osfs2_file_count(void);
extern uint32_t osfs2_file_ctime(void *file);
extern uint32_t osfs2_file_mtime(void *file);
extern uint32_t osfs2_free_blocks(void);
extern uint32_t osfs2_get_block_size(void);
extern uint32_t osfs2_total_blocks(void);
extern uint32_t osfs2_max_files(void);
extern int   disk_flush(void);

/* Heap */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* Timer */
extern uint64_t idt_get_ticks(void);
extern uint64_t idt_get_tsc_freq(void);
extern uint64_t idt_get_monotonic_ns(void);

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

/* Display modes */
#include "../include/sys/display_syscalls.h"
extern uint32_t display_get_mode_count(void)                    __attribute__((weak));
extern int      display_modeset_get_mode(uint32_t idx,
                                         display_mode_info_t *out) __attribute__((weak));
extern int      display_modeset_get_current(display_mode_info_t *out) __attribute__((weak));
extern int      display_modeset_set(uint32_t width, uint32_t height,
                                    uint32_t refresh_hz, uint32_t flags) __attribute__((weak));

/* GPU 3D driver (Vulkan Phase 1 / Wave 1) */
#include "../include/sys/gpu_syscalls.h"
extern int32_t proc_current_pid(void);
extern uint32_t vg3d_caps(void);
extern int32_t  vg3d_ctx_create(uint32_t pid, uint32_t flags);
extern int32_t  vg3d_ctx_destroy(uint32_t pid, uint32_t ctx_id);
extern int32_t  vg3d_res_create(uint32_t pid, uint32_t ctx_id,
                                const struct gpu_res_create_args *args);
extern uint64_t vg3d_res_map(uint32_t pid, uint32_t res_id);
extern int32_t  vg3d_res_destroy(uint32_t pid, uint32_t res_id);
extern int32_t  vg3d_submit(uint32_t pid, uint32_t ctx_id,
                            const void *cmd_bytes, uint64_t cmd_len,
                            uint64_t *out_fence);
extern int32_t  vg3d_fence_wait(uint64_t fence, uint64_t timeout_ns);
extern int32_t  vg3d_present(uint32_t pid, uint32_t ctx_id,
                             uint32_t res_id, uint32_t shm_handle);

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
#define SYS_SCHED_SETPARAM 142
#define SYS_SCHED_GETPARAM 143
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_SCHED_GET_PRIORITY_MAX 146
#define SYS_SCHED_GET_PRIORITY_MIN 147
#define SYS_SCHED_RR_GET_INTERVAL 148
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_SOCKET      41
#define SYS_CONNECT     42
#define SYS_ACCEPT      43
#define SYS_SENDTO      44
#define SYS_RECVFROM    45
#define SYS_SHUTDOWN    48
#define SYS_BIND        49
#define SYS_LISTEN      50
#define SYS_GETSOCKNAME 51
#define SYS_GETPEERNAME 52
#define SYS_SETSOCKOPT  54
#define SYS_GETSOCKOPT  55
#define SYS_CLONE       56
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_SETITIMER   38
#define SYS_FCNTL       72
#define SYS_FSYNC       74
#define SYS_RT_SIGSUSPEND 130
#define SYS_SIGALTSTACK 131
#define SYS_GETTID      186
#define SYS_TKILL       200
#define SYS_FUTEX       202
#define SYS_SCHED_GETAFFINITY 204
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
#define SYS_RENAMEAT    264
#define SYS_MKDIRAT     258
#define SYS_FCHOWNAT    260
#define SYS_FCHMODAT    268
#define SYS_FACCESSAT   269
#define SYS_PSELECT6    270
#define SYS_UTIMENSAT   280
#define SYS_RENAMEAT2   316
#define SYS_STATX       332
#define SYS_FACCESSAT2  439

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
#define SYS_BATCH           520  /* Execute array of syscalls in one trap */
#define SYS_CMDRING_INIT    521  /* Set up syscall-free command ring */
#define SYS_BOOT_DIAG_MARK  522  /* Persist boot diagnostics with a marker */
#define SYS_DEBUG_WATCH     523  /* GTA/Win32 buffer diagnostics */

/* errno values */
#define EPERM    1
#define EIO      5
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
#define EINTR    4
#define ENAMETOOLONG 36
#define EEXIST  17
#define EBUSY   16

/* open flags (Linux values) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000
#define O_ACCMODE   0x0003

#define AT_FDCWD    -100

/* lseek whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* ── File descriptor table ───────────────────────────────────── */

#include "../include/fd.h"

/* osfs2_file_t is opaque here — we get size via osfs2_file_size() */
extern uint64_t osfs2_file_size(void *file);

/* Per-process fd_table shim: `fd_table` resolves to the current
 * process's fds[] array via syscall_fds() (defined in process.c).
 * This lets all 66 existing `fd_table[fd]` call sites stay textually
 * unchanged while each process has its own copy of 8 KB. */
extern fd_entry_t *syscall_fds(void);
#define fd_table (syscall_fds())

/* BSD socket backend (socket.c). Socket functions return negative errno. */
extern int sock_socket(int domain, int type, int protocol);
extern int sock_bind(int sock_idx, const void *addr);
extern int sock_listen(int sock_idx, int backlog);
extern int sock_accept(int sock_idx, void *addr, uint32_t *addrlen);
extern int sock_connect(int sock_idx, const void *addr);
extern int sock_send(int sock_idx, const void *buf, uint32_t len, int flags);
extern int sock_recv(int sock_idx, void *buf, uint32_t len, int flags);
extern int sock_sendto(int sock_idx, const void *buf, uint32_t len,
                       int flags, const void *dest);
extern int sock_recvfrom(int sock_idx, void *buf, uint32_t len,
                         int flags, void *src, uint32_t *addrlen);
extern int sock_close(int sock_idx);
extern int sock_setsockopt(int sock_idx, int level, int optname,
                           const void *optval, uint32_t optlen);
extern int sock_getsockopt(int sock_idx, int level, int optname,
                           void *optval, uint32_t *optlen);
extern int sock_getsockname(int sock_idx, void *addr, uint32_t *addrlen);
extern int sock_getpeername(int sock_idx, void *addr, uint32_t *addrlen);
extern int sock_pending_bytes(int sock_idx);
extern void net_get_mac(uint8_t mac_out[6]);
extern uint8_t *net_get_ip_ptr(void);

/* Pipe pool stays global (only 8 slots), but each pipe_buf_t now
 * carries refcounts instead of open booleans. */
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
static bool console_screen_log_enabled = true;

void console_screen_log_set(bool enabled)
{
    console_screen_log_enabled = enabled;
}

bool console_screen_log_is_enabled(void)
{
    return console_screen_log_enabled;
}

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
        if (console_screen_log_enabled)
            fb_putc(s[i], 0x00CCCCCC);
        if (capture_buf && capture_pos < capture_max - 1)
            capture_buf[capture_pos++] = s[i];
        if ((i & 0xFFFF) == 0xFFFF) {
            if (console_screen_log_enabled)
                fb_flush();
            if (boot_diag_maybe_flush)
                boot_diag_maybe_flush("console", 64 * 1024, 50);
        }
    }
    if (console_screen_log_enabled)
        fb_flush();
    if (boot_diag_maybe_flush)
        boot_diag_maybe_flush("console", 64 * 1024, 100);
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

static bool sys_debug_is_fx_path(const char *name)
{
#ifdef OSITO_TRACE_FX_READS
    return name &&
           (strcmp(name, "shaders/win32_40_lq_final/im.fxc") == 0 ||
            strcmp(name, "rage/assets/tune/shaders/lib/win32_40/rage_im.fxc") == 0);
#else
    (void)name;
    return false;
#endif
}

/* Seed an fd table with stdio (fd 0/1/2 → console). Called by
 * proc_init() after it creates the kernel process, and by
 * syscall_reset_process() after wiping the current process's fds
 * on execve. Keeps console_read/write static to this file. */
void syscall_seed_stdio(fd_entry_t *fds)
{
    if (!fds) return;
    memset(&fds[0], 0, sizeof(fds[0]));
    memset(&fds[1], 0, sizeof(fds[1]));
    memset(&fds[2], 0, sizeof(fds[2]));

    fds[0].open = true;
    fds[0].type = FD_TYPE_CONSOLE;
    fds[0].read = console_read;

    fds[1].open = true;
    fds[1].type = FD_TYPE_CONSOLE;
    fds[1].write = console_write;

    fds[2].open = true;
    fds[2].type = FD_TYPE_CONSOLE;
    fds[2].write = console_write;
}

/* ── brk state (process heap) ────────────────────────────────── */

/* BRK heap size: dynamic from sys_caps (scales with RAM) */
#include "../include/sys_caps.h"
#define BRK_HEAP_SIZE  (g_sys_caps.brk_heap_size ? g_sys_caps.brk_heap_size : (16ULL * 1024 * 1024))

static void brk_reset_current(void);

/* Reset brk to base for a new process — called from proc_exec before elf_exec.
 * Zeroes the heap so the new process starts with clean memory. */
void sys_brk_reset(void)
{
    brk_reset_current();
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
#define MAP_STACK       0x20000
#define MAP_ANON        MAP_ANONYMOUS

/* Page table flags */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

#define MAP_FAILED      ((uint64_t)-1)

/* Anonymous mmap defaults to a 32-bit-clean VA window. Some native ports
 * still carry pointer-width assumptions in allocator metadata; returning
 * high addresses like 0x500000000 makes those bugs fault at the truncated
 * low alias. Keep the automatic pool above legacy ELF/PE load windows and
 * fail instead of silently crossing 4GB. Explicit MAP_FIXED/addr hints keep
 * their requested addresses. */
#define MMAP_ANON_LOW_BASE   0x30000000ULL
#define MMAP_ANON_LOW_LIMIT  0x100000000ULL
#define MMAP_ANON_HIGH_BASE  0x1000000000ULL
#define MMAP_ANON_HIGH_LIMIT 0x0000800000000000ULL

/* VMA tracking — per-process mmap regions */
#define MAX_VMAS        4096

#define VMA_ANON        0   /* Anonymous mapping (zero-fill on demand) */
#define VMA_FILE_ELF    1   /* ELF segment backed by file */
#define VMA_FILE_MMAP   2   /* mmap() file-backed mapping */
#define VMA_SHARED_STACK 3  /* pthread stack in the global upper-half mirror */
#define VMA_THREAD_STACK 4  /* demand-paged mmap assigned to CLONE_THREAD */
#define VMA_DEFERRED_STACK 5 /* munmapped stack, free after thread exit */
#define VMA_DEFERRED_SHARED_STACK 6
#define VMA_BRK         7   /* TGID-owned, demand-paged process break */

typedef struct {
    uint64_t    base;        /* virtual address */
    uint64_t    pages;       /* number of 4KB pages */
    uint32_t    prot;        /* PROT_READ|PROT_WRITE|PROT_EXEC */
    bool        in_use;
    uint8_t     type;        /* VMA_ANON, VMA_FILE_ELF, VMA_FILE_MMAP */
    uint32_t    magic;       /* VMA_MAGIC while the slot is initialized */
    vfs_node_t  file_node;   /* copy of VFS node for file-backed VMAs */
    uint64_t    file_offset; /* byte offset into file where this VMA starts */
    uint64_t    file_size;   /* bytes backed by file (rest is zero-fill) */
    void       *owner;       /* Debug hint only: process_t* that created it */
    uint32_t    owner_tgid;  /* Stable owner identity for CLONE_THREAD VMAs */
} vma_t;

static vma_t vma_table[MAX_VMAS];
#define VMA_MAGIC 0x564D4131u  /* "VMA1" */

bool syscall_file_is_mapped(void *file)
{
    if (!file) return false;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (vma_table[i].in_use &&
            (vma_table[i].type == VMA_FILE_ELF ||
             vma_table[i].type == VMA_FILE_MMAP) &&
            vma_table[i].file_node.fs_version == 2 &&
            vma_table[i].file_node.data == file)
            return true;
    }
    return false;
}

/* X-PGTBL: current process accessors (defined in process.c). */
extern void    *proc_current(void);
extern uint64_t proc_current_cr3(void);
extern int32_t  proc_current_tgid(void);
extern int32_t  proc_tgid_of(void *p);

/* GPU contexts/resources are process-wide objects.  CLONE_THREAD gives each
 * thread its own TID (proc_current_pid) while sharing the address space and
 * Vulkan device state with the thread-group leader.  Key the kernel GPU tables
 * by TGID so render threads can submit work to contexts created by the main
 * thread. */
static int32_t gpu_current_owner_pid(void)
{
    int32_t tgid = proc_current_tgid();
    return tgid > 0 ? tgid : proc_current_pid();
}

static void vma_set_owner(vma_t *v, void *owner)
{
    v->magic = VMA_MAGIC;
    v->owner = owner;
    v->owner_tgid = owner ? (uint32_t)proc_tgid_of(owner) : 0;
}

static void vma_clear_slot(vma_t *v)
{
    v->in_use = false;
    v->magic = 0;
    v->owner = NULL;
    v->owner_tgid = 0;
}

/* True if a VMA belongs to the currently-running process (or is
 * ownerless, for backward compat with legacy allocators). */
static inline bool vma_owned_by_current(const vma_t *v)
{
    static int warned_bad_vma;

    if (!v->in_use)
        return false;

    if (v->magic != VMA_MAGIC) {
        if (!warned_bad_vma) {
            warned_bad_vma = 1;
            serial_puts("[VMA] corrupt slot ignored: base=");
            serial_puthex(v->base, 16);
            serial_puts(" pages=");
            serial_puthex(v->pages, 16);
            serial_puts(" magic=");
            serial_puthex(v->magic, 8);
            serial_puts(" owner=");
            serial_puthex((uint64_t)(uintptr_t)v->owner, 16);
            serial_puts("\n");
        }
        return false;
    }

    void *cur = proc_current();
    if (v->owner == NULL || v->owner == cur) return true;
    /* CLONE_THREAD threads share one address space (and its VMAs) with their
     * thread-group siblings, but each is a distinct process_t. A worker/render
     * thread must be able to demand-fault VMAs the main thread (or another
     * sibling) registered — including its own musl-mmap'd stack — so match by
     * thread group, not the exact process_t. The owner pointer is kept only
     * for diagnostics; do not dereference it here. */
    int32_t ct = proc_current_tgid();
    return ct != 0 && v->owner_tgid != 0 && (uint32_t)ct == v->owner_tgid;
}

/* Return the furthest end of an owned VMA intersecting [base, base + size).
 * PE images and Win32 VirtualAlloc reservations live in separate registries;
 * their allocators call this helper before installing mappings. */
uint64_t syscall_vma_range_conflict_end(uint64_t base, uint64_t size)
{
    if (size == 0)
        return UINT64_MAX;
    uint64_t end = base + size;
    if (end < base)
        return UINT64_MAX;

    uint64_t conflict_end = 0;

    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (!vma_owned_by_current(&vma_table[i])) continue;

        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096ULL;
        if (vma_end < vma_table[i].base)
            return UINT64_MAX;

        if (base < vma_end && end > vma_table[i].base &&
            vma_end > conflict_end)
            conflict_end = vma_end;
    }

    return conflict_end;
}

int syscall_assign_thread_stack(void *thread, uint64_t stack_pointer)
{
    if (!thread || !stack_pointer) return -1;
    extern int32_t proc_tgid_of(void *p);
    int32_t tgid = proc_tgid_of(thread);
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t end = vma_table[i].base + vma_table[i].pages * 4096;
        if (stack_pointer < vma_table[i].base || stack_pointer >= end)
            continue;
        if (vma_table[i].owner && proc_tgid_of(vma_table[i].owner) != tgid)
            continue;
        /* pthread implementations may place a thread stack inside memory
         * obtained from brk. That storage remains allocator-owned by the
         * whole thread group; retyping or transferring the VMA would make
         * the next brk() lose its heap and let the thread reaper free it. */
        if (vma_table[i].type == VMA_BRK) {
            extern uint32_t proc_pid_of(void *p);
            serial_puts("[VMA-STACK] brk-backed pid=");
            serial_putdec(proc_pid_of(thread));
            serial_puts(" sp=0x");
            serial_puthex(stack_pointer, 16);
            serial_puts(" heap=0x");
            serial_puthex(vma_table[i].base, 16);
            serial_puts("\n");
            return 0;
        }
        if (vma_table[i].type != VMA_SHARED_STACK)
            vma_table[i].type = VMA_THREAD_STACK;
        vma_table[i].owner = thread;
        extern uint32_t proc_pid_of(void *p);
        serial_puts("[VMA-STACK] assign pid=");
        serial_putdec(proc_pid_of(thread));
        serial_puts(" base=0x");
        serial_puthex(vma_table[i].base, 16);
        serial_puts(" pages=");
        serial_putdec(vma_table[i].pages);
        serial_puts(" sp=0x");
        serial_puthex(stack_pointer, 16);
        serial_puts(" type=");
        serial_putdec(vma_table[i].type);
        serial_puts("\n");
        return 0;
    }
    return -1;
}

static int vma_find_free_slot_except(int except)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        if (i == except) continue;
        if (!vma_table[i].in_use) return i;
    }
    return -1;
}

static void vma_copy_range(vma_t *dst, const vma_t *src,
                           uint64_t base, uint64_t pages)
{
    *dst = *src;
    dst->base = base;
    dst->pages = pages;
    dst->in_use = true;

    if (src->type == VMA_FILE_ELF || src->type == VMA_FILE_MMAP) {
        uint64_t delta = base - src->base;
        uint64_t bytes = pages * 4096;
        if (delta < src->file_size) {
            uint64_t remaining = src->file_size - delta;
            dst->file_offset = src->file_offset + delta;
            dst->file_size = remaining < bytes ? remaining : bytes;
        } else {
            dst->file_offset = src->file_offset + src->file_size;
            dst->file_size = 0;
        }
    }
}

static int vma_split_for_range(int idx, uint64_t base, uint64_t pages)
{
    vma_t src = vma_table[idx];
    uint64_t start = src.base;
    uint64_t end = src.base + src.pages * 4096;
    uint64_t range_end = base + pages * 4096;

    if (base == start && range_end == end)
        return idx;

    int before_slot = -1;
    int after_slot = -1;
    bool has_before = base > start;
    bool has_after = range_end < end;

    if (has_before) {
        before_slot = vma_find_free_slot_except(idx);
        if (before_slot < 0) return -ENOMEM;
        vma_table[before_slot].in_use = true;
    }
    if (has_after) {
        after_slot = vma_find_free_slot_except(idx);
        if (after_slot < 0) {
            if (before_slot >= 0) vma_table[before_slot].in_use = false;
            return -ENOMEM;
        }
        vma_table[after_slot].in_use = true;
    }

    vma_copy_range(&vma_table[idx], &src, base, pages);
    if (has_before) {
        uint64_t before_pages = (base - start) / 4096;
        vma_copy_range(&vma_table[before_slot], &src, start, before_pages);
    }
    if (has_after) {
        uint64_t after_pages = (end - range_end) / 4096;
        vma_copy_range(&vma_table[after_slot], &src, range_end, after_pages);
    }

    return idx;
}

static int vma_set_present_page_flags(uint64_t cr3, uint64_t va,
                                      uint64_t flags)
{
    extern uint64_t *paging_get_pte(uint64_t virt);
    extern uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t virt);
    extern int paging_set_flags_in_cr3(uint64_t cr3, uint64_t virt,
                                       uint64_t flags);

    uint64_t *pte = cr3 ? paging_get_pte_in_cr3(cr3, va)
                        : paging_get_pte(va);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;

    return cr3 ? paging_set_flags_in_cr3(cr3, va, flags)
               : paging_set_flags(va, flags);
}

/* ── Demand paging: free individually-faulted pages in a VMA ───── */
extern uint64_t *paging_get_pte(uint64_t virt);
extern uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t virt);
extern int paging_unmap_page(uint64_t virt);
extern int paging_unmap_page_in_cr3(uint64_t cr3, uint64_t virt);
extern void mem_free_pages(void *addr, uint64_t count);

#define PTE_PRESENT_BIT  (1ULL << 0)
#define PTE_ADDR_MASK_   0x000FFFFFFFFFF000ULL

/* Free faulted-in pages for a VMA. X-PGTBL: walks the current process's
 * CR3 if it has one, falling back to kernel_pml4. Callers ensure the
 * owning process is current (cleanup runs from sys_exit on its own
 * context; sys_munmap is called by the owning process directly). */
/* ── Memory quarantine: catch use-after-free at page granularity ──
 *
 * When a VMA is freed, pages are unmapped (immediate #PF on access)
 * but the physical page is held in quarantine for QUARANTINE_TICKS.
 * If the process accesses it before expiry, the #PF handler detects
 * the UAF and reports the address + freed-RIP with symbolized backtrace.
 * Zero overhead in normal execution (only in munmap + #PF slow paths). */

#define QUARANTINE_SIZE   256
#define QUARANTINE_TICKS  500   /* 5 seconds at 100Hz */

typedef struct {
    uint64_t phys;
    uint64_t virt;
    uint64_t freed_tick;
    uint64_t freed_rip;     /* caller's return address at free time */
    uint32_t pid;
    bool     active;
} quarantine_entry_t;

static quarantine_entry_t quarantine_ring[QUARANTINE_SIZE];
static uint32_t quarantine_head;
static bool     quarantine_enabled;

/* Shell command: `quarantine on` / `quarantine off` */
void quarantine_set(bool enable) { quarantine_enabled = enable; }
bool quarantine_is_enabled(void) { return quarantine_enabled; }

static void quarantine_page(uint64_t phys, uint64_t virt, uint32_t pid)
{
    /* Recycle oldest entry if slot is occupied */
    quarantine_entry_t *slot = &quarantine_ring[quarantine_head % QUARANTINE_SIZE];
    if (slot->active && slot->phys)
        mem_free_pages((void *)slot->phys, 1);

    slot->phys = phys;
    slot->virt = virt;
    slot->pid = pid;
    slot->freed_tick = idt_get_ticks();
    slot->freed_rip = (uint64_t)__builtin_return_address(1);
    slot->active = true;
    quarantine_head++;
}

/* Expire old quarantine entries (called periodically) */
static void quarantine_expire(void)
{
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < QUARANTINE_SIZE; i++) {
        if (quarantine_ring[i].active &&
            now - quarantine_ring[i].freed_tick > QUARANTINE_TICKS) {
            mem_free_pages((void *)quarantine_ring[i].phys, 1);
            quarantine_ring[i].active = false;
        }
    }
}

/* Check if a faulting address hits a quarantined page → use-after-free */
int quarantine_check_uaf(uint64_t fault_addr, uint32_t pid)
{
    if (!quarantine_enabled) return 0;

    uint64_t fault_page = fault_addr & ~0xFFFULL;
    for (int i = 0; i < QUARANTINE_SIZE; i++) {
        quarantine_entry_t *q = &quarantine_ring[i];
        if (!q->active || q->pid != pid) continue;
        if (q->virt == fault_page) {
            serial_puts("\n[UAF] *** USE-AFTER-FREE DETECTED ***\n");
            serial_puts("[UAF] Address: 0x");
            serial_puthex(fault_addr, 16);
            serial_puts("\n[UAF] Page freed at RIP: 0x");
            serial_puthex(q->freed_rip, 16);
            serial_puts(" (");
            serial_putdec(idt_get_ticks() - q->freed_tick);
            serial_puts(" ticks ago)\n");
            /* Symbolize via usym if available */
            {
                extern bool user_symbolize(void *p, uint64_t addr,
                                           const char **name, uint64_t *off);
                const char *name = NULL;
                uint64_t off = 0;
                if (user_symbolize(proc_current(), q->freed_rip, &name, &off) && name) {
                    serial_puts("[UAF] Freed in: ");
                    serial_puts(name);
                    serial_puts("+0x");
                    serial_puthex(off, 4);
                    serial_puts("\n");
                }
            }
            return 1;  /* UAF confirmed */
        }
    }
    return 0;
}

static uint64_t vma_free_pages_in_cr3(vma_t *v, uint64_t cr3, uint32_t pid)
{
    if (v->type == VMA_SHARED_STACK ||
        v->type == VMA_DEFERRED_SHARED_STACK) {
        extern uint32_t proc_pid_of(void *p);
        extern uint32_t proc_state_of(void *p);
        serial_puts("[VMA-STACK] free direct owner=");
        serial_putdec(proc_pid_of(v->owner));
        serial_puts(" state=");
        serial_putdec(proc_state_of(v->owner));
        serial_puts(" caller-pid=");
        serial_putdec(pid);
        serial_puts(" base=0x");
        serial_puthex(v->base, 16);
        serial_puts(" pages=");
        serial_putdec(v->pages);
        serial_puts("\n");
        mem_free_pages((void *)VIRT_TO_PHYS(v->base), v->pages);
        return v->pages;
    }

    /* Expire old quarantine entries periodically */
    if (quarantine_enabled)
        quarantine_expire();

    uint64_t pages = v->pages;
    if (v->type == VMA_BRK)
        pages = (v->file_size + 4095ULL) / 4096ULL;

    uint64_t freed_pages = 0;
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t va = v->base + p * 4096;
        uint64_t *pte = cr3 ? paging_get_pte_in_cr3(cr3, va)
                            : paging_get_pte(va);
        if (pte && (*pte & PTE_PRESENT_BIT)) {
            uint64_t phys = *pte & PTE_ADDR_MASK_;
            if (cr3) paging_unmap_page_in_cr3(cr3, va);
            else     paging_unmap_page(va);

            if (quarantine_enabled)
                quarantine_page(phys, va, pid);
            else
                mem_free_pages((void *)phys, 1);
            freed_pages++;
        }
    }
    return freed_pages;
}

static uint64_t vma_free_pages(vma_t *v)
{
    extern int32_t proc_current_pid(void);
    uint64_t cr3 = v->owner ? proc_current_cr3() : 0;
    return vma_free_pages_in_cr3(v, cr3, (uint32_t)proc_current_pid());
}

static vma_t *brk_vma_current(void)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        if (vma_table[i].type == VMA_BRK &&
            vma_owned_by_current(&vma_table[i]))
            return &vma_table[i];
    }
    return NULL;
}

static void brk_decommit_tail(vma_t *v, uint64_t new_size,
                              uint64_t old_size)
{
    if (!v || new_size >= old_size)
        return;

    uint64_t first = (new_size + 4095ULL) & ~4095ULL;
    uint64_t end = (old_size + 4095ULL) & ~4095ULL;
    uint64_t capacity = v->pages * 4096ULL;
    if (end > capacity)
        end = capacity;

    uint64_t cr3 = proc_current_cr3();
    uint32_t pid = (uint32_t)proc_current_pid();
    if (quarantine_enabled)
        quarantine_expire();

    for (uint64_t offset = first; offset < end; offset += 4096ULL) {
        uint64_t va = v->base + offset;
        uint64_t *pte = cr3 ? paging_get_pte_in_cr3(cr3, va)
                            : paging_get_pte(va);
        if (!pte || !(*pte & PTE_PRESENT_BIT))
            continue;

        uint64_t phys = *pte & PTE_ADDR_MASK_;
        if (cr3) paging_unmap_page_in_cr3(cr3, va);
        else     paging_unmap_page(va);

        if (quarantine_enabled)
            quarantine_page(phys, va, pid);
        else
            mem_free_pages((void *)phys, 1);
    }
}

static void brk_reset_current(void)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        if (vma_table[i].type != VMA_BRK ||
            !vma_owned_by_current(&vma_table[i]))
            continue;
        uint64_t freed_pages = vma_free_pages(&vma_table[i]);
        serial_puts("[BRK] reset tgid=");
        serial_putdec(vma_table[i].owner_tgid);
        serial_puts(" committed=");
        serial_putdec(freed_pages * 4);
        serial_puts("KB\n");
        vma_clear_slot(&vma_table[i]);
    }
}

/* ── Public VMA registration (called from elf.c for demand paging) ── */
int vma_register_file(uint64_t base, uint64_t pages, uint32_t prot,
                      uint8_t type, vfs_node_t *node,
                      uint64_t file_offset, uint64_t file_size)
{
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) {
            vma_table[i].base        = base;
            vma_table[i].pages       = pages;
            vma_table[i].prot        = prot;
            vma_table[i].in_use      = true;
            vma_table[i].type        = type;
            vma_table[i].file_node   = *node;
            vma_table[i].file_offset = file_offset;
            vma_table[i].file_size   = file_size;
            /* Anchor ownership to the exec target (not the possibly-stale
             * proc_current during the ELF load) so the owner filter in
             * demand_page_fault matches once the binary runs. */
            {
                extern void *proc_exec_target(void);
                void *et = proc_exec_target();
                vma_set_owner(&vma_table[i], et ? et : proc_current());
            }
            return 0;
        }
    }
    return -1;
}

/* ── VFS device/proc forward declarations (X-VFS) ─────────────── */

/* Device IDs for virtual /dev entries */
#define DEV_NULL        0
#define DEV_ZERO        1
#define DEV_URANDOM     2
#define DEV_CONSOLE     3

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
    if (count == 0) return 0;

    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_SOCKET)
        return sock_send(f->socket_idx, (const void *)buf, (uint32_t)count, 0);

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
        /* Only v2 supports write for now */
        if (f->node.fs_version != 2) return -EROFS;
        int ret = osfs2_write(f->node.data, f->offset, (const void *)buf, count);
        if (ret < 0) return -EIO;
        f->offset += count;
        if (f->offset > f->node.size) f->node.size = f->offset;
        return (int64_t)count;
    }

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *p = (pipe_buf_t *)f->pipe;
        if (!p || p->read_refs == 0) return -EPIPE;

        bool nonblock = (f->oflags & 04000 /* O_NONBLOCK */) != 0;
        const uint8_t *src = (const uint8_t *)buf;
        uint64_t written = 0;
        while (written < count) {
            if (p->count >= PIPE_BUF_SIZE) {
                if (written > 0) return (int64_t)written;
                if (nonblock) return -EAGAIN;
                __asm__ volatile ("sti; hlt; cli" ::: "memory");
                if (p->read_refs == 0) return -EPIPE;
                continue;
            }
            /* Bulk copy: contiguous chunk from head to end-of-buffer */
            uint32_t space = PIPE_BUF_SIZE - p->count;
            uint32_t contig = PIPE_BUF_SIZE - p->head;
            uint32_t remain = (uint32_t)(count - written);
            uint32_t chunk = space < contig ? space : contig;
            if (chunk > remain) chunk = remain;
            memcpy(&p->buf[p->head], src + written, chunk);
            p->head = (p->head + chunk) % PIPE_BUF_SIZE;
            p->count += chunk;
            written += chunk;
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
    if (count == 0) return 0;
    fd_entry_t *f = &fd_table[fd];

    if (f->type == FD_TYPE_SOCKET)
        return sock_recv(f->socket_idx, (void *)buf, (uint32_t)count, 0);

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
        uint64_t file_size = f->node.fs_version == 2
                           ? osfs2_file_size(f->node.data) : f->node.size;
        f->node.size = file_size;
        bool dbg_fx = false;
        if (f->node.fs_version == 2) {
            const char *nm = osfs2_file_name(f->node.data);
            dbg_fx = sys_debug_is_fx_path(nm);
            if (dbg_fx) {
                serial_puts("[sys_read] fx name='");
                serial_puts(nm);
                serial_puts("' off=");
                serial_putdec(f->offset);
                serial_puts(" count=");
                serial_putdec(count);
                serial_puts(" size=");
                serial_putdec(file_size);
                serial_puts("\n");
            }
        }
        if (f->offset >= file_size) return 0;  /* EOF */
        uint64_t avail = file_size - f->offset;
        if (count > avail) count = avail;
        if (count == 0) return 0;

        /*
         * Do file I/O through a kernel bounce buffer. Some callers pass
         * buffers that are mapped only in the process page table; letting the
         * VFS/disk path write to those addresses directly can silently leave
         * the user buffer unchanged. NtReadFile already uses the same pattern.
         */
        const uint64_t max_chunk = 64 * 1024;
        uint64_t chunk_cap = count < max_chunk ? count : max_chunk;
        uint8_t *sys_buf = (uint8_t *)kmalloc(chunk_cap);
        if (!sys_buf) return -ENOMEM;

        volatile uint8_t *dst = (volatile uint8_t *)buf;
        uint64_t done = 0;
        while (done < count) {
            uint64_t chunk = count - done;
            if (chunk > chunk_cap) chunk = chunk_cap;

            int ret = vfs_read(&f->node, f->offset + done, sys_buf, chunk);
            if (ret < 0) {
                kfree(sys_buf);
                return done ? (int64_t)done : -EFAULT;
            }
            if (dbg_fx) {
                serial_puts("[sys_read] fx ret=");
                serial_putdec(ret);
                serial_puts(" bytes=");
                if (ret >= 4) {
                    serial_puthex(sys_buf[0], 2);
                    serial_puts(" ");
                    serial_puthex(sys_buf[1], 2);
                    serial_puts(" ");
                    serial_puthex(sys_buf[2], 2);
                    serial_puts(" ");
                    serial_puthex(sys_buf[3], 2);
                }
                serial_puts("\n");
            }
            if (ret == 0) break;

            for (int i = 0; i < ret; i++)
                dst[done + (uint64_t)i] = sys_buf[i];

            done += (uint64_t)ret;
            if ((uint64_t)ret < chunk) break;
        }

        kfree(sys_buf);
        f->offset += done;
        return (int64_t)done;
    }

    if (f->type == FD_TYPE_PIPE) {
        pipe_buf_t *p = (pipe_buf_t *)f->pipe;
        if (!p) return -EBADF;

        /* Block until data is available or write end is closed.
         * Non-blocking mode (O_NONBLOCK) returns EAGAIN immediately. */
        bool nonblock = (f->oflags & 04000 /* O_NONBLOCK */) != 0;
        while (p->count == 0) {
            if (p->write_refs == 0) return 0;  /* EOF */
            if (nonblock) return -EAGAIN;
            __asm__ volatile ("sti; hlt; cli" ::: "memory");
        }

        uint8_t *dst = (uint8_t *)buf;
        uint64_t nread = 0;
        while (nread < count && p->count > 0) {
            /* Bulk copy: contiguous chunk from tail to end-of-buffer */
            uint32_t contig = PIPE_BUF_SIZE - p->tail;
            uint32_t remain = (uint32_t)(count - nread);
            uint32_t chunk = p->count < contig ? p->count : contig;
            if (chunk > remain) chunk = remain;
            memcpy(dst + nread, &p->buf[p->tail], chunk);
            p->tail = (p->tail + chunk) % PIPE_BUF_SIZE;
            p->count -= chunk;
            nread += chunk;
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

/* Helper: write hex digits of `val` (always exactly `digits` chars) into buf */
static int write_hex(char *buf, uint64_t val, int digits)
{
    for (int d = (digits - 1) * 4; d >= 0; d -= 4)
        *buf++ = "0123456789abcdef"[(val >> d) & 0xF];
    return digits;
}

/* Generate /proc/self/maps content into buffer.
 * Format matches Linux: start-end perms offset dev inode pathname
 * For file-backed VMAs, offset is the real file offset and pathname is
 * the basename of the backing file (e.g. zsh.elf, GTA5.elf). */
static int proc_gen_maps(char *buf, int max)
{
    extern const char *osfs2_file_name(void *file);
    int pos = 0;

    for (int i = 0; i < MAX_VMAS && pos < max - 128; i++) {
        if (!vma_table[i].in_use) continue;
        if (!vma_owned_by_current(&vma_table[i])) continue;
        uint64_t start = vma_table[i].base;
        uint64_t end = start + vma_table[i].pages * 4096;
        uint32_t p = vma_table[i].prot;

        char line[128];
        int lp = 0;

        /* start-end (12 hex digits = 48 bits, enough for our address space) */
        lp += write_hex(line + lp, start, 12);
        line[lp++] = '-';
        lp += write_hex(line + lp, end, 12);
        line[lp++] = ' ';

        /* perms */
        line[lp++] = (p & PROT_READ)  ? 'r' : '-';
        line[lp++] = (p & PROT_WRITE) ? 'w' : '-';
        line[lp++] = (p & PROT_EXEC)  ? 'x' : '-';
        line[lp++] = 'p';
        line[lp++] = ' ';

        /* offset (file_offset for file-backed, 0 for anonymous) */
        lp += write_hex(line + lp, vma_table[i].file_offset, 8);
        line[lp++] = ' ';

        /* dev (we have no real block dev) */
        line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ':';
        line[lp++] = '0'; line[lp++] = '0'; line[lp++] = ' ';

        /* inode (use VMA index as a stable id) */
        {
            int idx = i;
            char d[8]; int nd = 0;
            if (idx == 0) d[nd++] = '0';
            else while (idx > 0) { d[nd++] = '0' + (idx % 10); idx /= 10; }
            while (nd-- > 0) line[lp++] = d[nd];
        }

        /* pathname (basename of backing file, or [anon]/[heap]) */
        line[lp++] = ' ';
        if (vma_table[i].type == VMA_FILE_ELF ||
            vma_table[i].type == VMA_FILE_MMAP) {
            const char *name = NULL;
            if (vma_table[i].file_node.fs_version == 2 &&
                vma_table[i].file_node.data) {
                name = osfs2_file_name(vma_table[i].file_node.data);
            }
            if (!name) name = "(file)";
            while (*name && lp < (int)sizeof(line) - 2) line[lp++] = *name++;
        } else {
            const char *anon = (vma_table[i].prot == 0) ? "[reserved]" : "[anon]";
            while (*anon && lp < (int)sizeof(line) - 2) line[lp++] = *anon++;
        }

        line[lp++] = '\n';

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

	uint64_t virtual_bytes = 0;
	uint64_t resident_bytes = 0;
	uint64_t cr3 = proc_current_cr3();
	for (int i = 0; i < MAX_VMAS; i++) {
		if (!vma_table[i].in_use || !vma_owned_by_current(&vma_table[i]))
			continue;
		uint64_t visible_bytes = vma_table[i].pages * 4096ULL;
		uint64_t resident_pages = vma_table[i].pages;
		if (vma_table[i].type == VMA_BRK) {
			visible_bytes = vma_table[i].file_size;
			resident_pages = (visible_bytes + 4095ULL) / 4096ULL;
		}
		virtual_bytes += visible_bytes;
		for (uint64_t page = 0; page < resident_pages; page++) {
			uint64_t address = vma_table[i].base + page * 4096;
			uint64_t *pte = cr3 ? paging_get_pte_in_cr3(cr3, address)
				: paging_get_pte(address);
			if (pte && (*pte & PTE_PRESENT))
				resident_bytes += 4096;
		}
	}

	const char *labels[2] = { "VmSize:\t", "VmRSS:\t" };
	uint64_t values[2] = { virtual_bytes / 1024, resident_bytes / 1024 };
	for (int field = 0; field < 2; field++) {
		s = labels[field];
		while (*s && pos < max - 1) buf[pos++] = *s++;
		char number[24];
		int count = 0;
		uint64_t value = values[field];
		if (value == 0) number[count++] = '0';
		while (value > 0 && count < (int)sizeof(number)) {
			number[count++] = '0' + (value % 10);
			value /= 10;
		}
		while (count-- > 0 && pos < max - 1) buf[pos++] = number[count];
		s = " kB\n";
		while (*s && pos < max - 1) buf[pos++] = *s++;
	}

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

static int socket_index_from_fd(uint64_t fd)
{
    if (fd >= MAX_FDS || !fd_table[fd].open ||
        fd_table[fd].type != FD_TYPE_SOCKET)
        return -EBADF;
    return fd_table[fd].socket_idx;
}

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol)
{
    int sock_idx = sock_socket((int)domain, (int)(type & 0xf), (int)protocol);
    if (sock_idx < 0) return sock_idx;

    int fd = vfs_alloc_fd();
    if (fd < 0) {
        sock_close(sock_idx);
        return -EMFILE;
    }

    fd_entry_t *entry = &fd_table[fd];
    memset(entry, 0, sizeof(*entry));
    entry->open = true;
    entry->type = FD_TYPE_SOCKET;
    entry->socket_idx = sock_idx;
    entry->oflags = (type & 0x800) ? 04000 : 0; /* SOCK_NONBLOCK */
    return fd;
}

static int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    (void)addrlen;
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    if (!addr) return -EFAULT;
    return sock_bind(idx, (const void *)addr);
}

static int64_t sys_listen(uint64_t fd, uint64_t backlog)
{
    int idx = socket_index_from_fd(fd);
    return idx < 0 ? idx : sock_listen(idx, (int)backlog);
}

static int64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;

    int accepted_idx = sock_accept(idx, (void *)addr, (uint32_t *)addrlen);
    if (accepted_idx < 0) return accepted_idx;

    int accepted_fd = vfs_alloc_fd();
    if (accepted_fd < 0) {
        sock_close(accepted_idx);
        return -EMFILE;
    }

    fd_entry_t *entry = &fd_table[accepted_fd];
    memset(entry, 0, sizeof(*entry));
    entry->open = true;
    entry->type = FD_TYPE_SOCKET;
    entry->socket_idx = accepted_idx;
    return accepted_fd;
}

static int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    (void)addrlen;
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    if (!addr) return -EFAULT;
    return sock_connect(idx, (const void *)addr);
}

static int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len,
                          uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    (void)addrlen;
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    if (!buf && len) return -EFAULT;
    if (!addr)
        return sock_send(idx, (const void *)buf, (uint32_t)len, (int)flags);
    return sock_sendto(idx, (const void *)buf, (uint32_t)len,
                       (int)flags, (const void *)addr);
}

static int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len,
                            uint64_t flags, uint64_t addr, uint64_t addrlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    if (!buf && len) return -EFAULT;
    if (!addr)
        return sock_recv(idx, (void *)buf, (uint32_t)len, (int)flags);
    return sock_recvfrom(idx, (void *)buf, (uint32_t)len, (int)flags,
                         (void *)addr, (uint32_t *)addrlen);
}

static int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    return sock_getsockname(idx, (void *)addr, (uint32_t *)addrlen);
}

static int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    return sock_getpeername(idx, (void *)addr, (uint32_t *)addrlen);
}

static int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                              uint64_t optval, uint64_t optlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    return sock_setsockopt(idx, (int)level, (int)optname,
                           (const void *)optval, (uint32_t)optlen);
}

static int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname,
                              uint64_t optval, uint64_t optlen)
{
    int idx = socket_index_from_fd(fd);
    if (idx < 0) return idx;
    return sock_getsockopt(idx, (int)level, (int)optname,
                           (void *)optval, (uint32_t *)optlen);
}

/* ── Path normalization for the flat OsitoFS namespace ──────────────
 *
 * cc1 / gcc bake HOST absolute include paths into the binary and emit
 * RELATIVE paths containing '.'/'..' (e.g. "usr/bin/../lib/gcc/.../include").
 * OsitoFS is a FLAT namespace (no directories, no '.'/'..' resolution),
 * so these paths never match real entries like "usr/include/stdc-predef.h".
 *
 * This helper canonicalizes a path into the flat OsitoFS name:
 *   1. Strip a known HOST sysroot / system prefix (small table).
 *   2. Strip the single leading '/'.
 *   3. Resolve '.' (drop) and '..' (pop one component) lexically.
 *
 * Result is written into out[] (NUL-terminated). Returns true if the
 * normalized path differs from a trivial leading-'/'-strip (i.e. some
 * actual rewriting happened) — callers can fall back to the raw path
 * otherwise. We always populate out[] with the canonical form.
 */
static const char *const k_host_prefixes[] = {
    /* User's gcc toolchain sysroot baked into the cc1 binary. */
    "/Users/pc/ok-ported/toolchain/sysroot/",
    /* Generic system prefixes that map 1:1 onto flat 'usr/...' entries. */
    "/usr/",
    NULL,
};

/* Non-static: elf.c (exec path normalization) calls this via an extern decl. */
bool path_normalize_flat(const char *path, char *out, int out_sz)
{
    if (!path || !out || out_sz <= 1) return false;

    const char *p = path;
    bool rewritten = false;

    /* 1. Strip a known HOST sysroot / system prefix. The "/usr/" entry
     *    maps an absolute "/usr/include/..." onto flat "usr/include/..."
     *    by leaving the trailing "usr/" in place (we strip only the
     *    leading slash, see below). To keep both behaviors simple we
     *    match the full prefix then re-prepend the tail. */
    for (int i = 0; k_host_prefixes[i]; i++) {
        const char *pre = k_host_prefixes[i];
        /* "/usr/" is special: we want the "usr/" to survive, so only the
         *  sysroot-style prefixes (which end the path at a real root) get
         *  fully stripped. Detect by whether the prefix is exactly the
         *  generic "/usr/" guard. */
        bool keep_tail_usr = (pre[0] == '/' && pre[1] == 'u' && pre[2] == 's' &&
                              pre[3] == 'r' && pre[4] == '/' && pre[5] == '\0');
        if (str_startswith(p, pre)) {
            if (keep_tail_usr) {
                /* "/usr/x" → leave "/usr/x"; leading '/' stripped below
                 *  yields "usr/x" which matches the flat entry. */
                /* no-op: fall through to leading-slash strip */
            } else {
                p += (int)strlen(pre);   /* drop the whole sysroot prefix */
                rewritten = true;
            }
            break;
        }
    }

    /* 2. Strip the single leading '/'. */
    if (*p == '/') { p++; rewritten = true; }

    /* 3. Lexically resolve '.' and '..' components into out[].
     *    We build a stack of component start offsets so '..' can pop. */
    int comp_off[64];   /* start offset (in out[]) of each kept component */
    int ncomp = 0;
    int w = 0;          /* write cursor in out[] */
    out[0] = '\0';

    while (*p) {
        /* Skip redundant separators. */
        while (*p == '/') { p++; rewritten = true; }
        if (!*p) break;

        /* Find component bounds. */
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (len == 1 && start[0] == '.') {
            /* '.' → drop */
            rewritten = true;
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* '..' → pop last kept component (if any) */
            rewritten = true;
            if (ncomp > 0) {
                ncomp--;
                w = comp_off[ncomp];
                out[w] = '\0';
            }
            continue;
        }

        /* Normal component: append, prefixing with '/' if not first. */
        if (ncomp >= 64) return false;            /* too many components */
        if (w > 0) {
            if (w + 1 >= out_sz) return false;
            out[w++] = '/';
        }
        comp_off[ncomp++] = w;
        if (w + len >= out_sz) return false;       /* overflow guard */
        for (int i = 0; i < len; i++) out[w++] = start[i];
        out[w] = '\0';
    }

    out[w] = '\0';
    return rewritten;
}

static const char *normalized_lookup(const char *path, char *out, int out_sz)
{
    return path_normalize_flat(path, out, out_sz) ? out : path;
}

/* Synthetic directories for the flat OsitoFS namespace. A path is a
 * "directory" if it is the slash-prefix of any stored file key (the FS has
 * no real directory entries — keys are full paths like "usr/include/foo").
 * gcc validates its include dirs by stat()ing them and DROPS any that aren't
 * S_ISDIR; on a flat FS every include dir ENOENT'd, so gcc reported
 * "no include path in which to search for stdc-predef.h". `flat` must already
 * be path_normalize_flat()'d. */
static bool osfs2_path_is_dir(const char *flat)
{
    if (!flat) return false;
    if (!flat[0]) return true;                 /* "" == root */
    uint64_t len = strlen(flat);
    if (len >= 64) return false;
    uint32_t n = osfs2_file_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = osfs2_file_name(osfs2_file_at(i));
        uint32_t name_len = 0;
        while (name && name_len < 64 && name[name_len]) name_len++;
        if (!name || name_len == 64) continue;
        if (name && str_startswith(name, flat) && name[len] == '/')
            return true;
    }
    return false;
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
        f->oflags = (uint32_t)flags;
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

    fd_entry_t *f = &fd_table[newfd];
    memset(f, 0, sizeof(*f));

    /* Normalize host/relative paths into the flat OsitoFS namespace.
     * cc1 opens e.g. "usr/bin/../lib/gcc/.../include" and the host-absolute
     * "/Users/pc/ok-ported/toolchain/sysroot/usr/include/stdc-predef.h";
     * both must collapse onto flat entries like "usr/include/stdc-predef.h".
     * Only the regular-file branch is affected — /dev/ and /proc/ above are
     * checked first and return before reaching here. */
    char norm_path[256];
    const char *lookup = normalized_lookup(path, norm_path, sizeof(norm_path));
    if (strlen(lookup) >= 64) return -ENAMETOOLONG;

    bool found = vfs_find(lookup, VFS_MODE_POSIX, &f->node);
    if (!found && osfs2_path_is_dir(lookup)) {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
            return -EISDIR;
        f->open = true;
        f->type = FD_TYPE_DIR;
        f->oflags = (uint32_t)flags;
        f->offset = 0;
        strcpy(f->dir_path, lookup);
        return newfd;
    }

    if (!found) {
        if (flags & O_DIRECTORY) return -ENOTDIR;
        if (flags & O_CREAT) {
            void *f2 = osfs2_create(lookup, 0);
            if (f2) {
                f->node.fs_version = 2;
                f->node.data = f2;
                f->node.size = 0;
            } else return -ENOENT;
        } else if (!vfs_find(lookup, VFS_MODE_WIN32, &f->node)) {
            return -ENOENT;
        }
    }

    if ((flags & O_DIRECTORY) && f->node.fs_version == 2)
        return -ENOTDIR;

    /* ETXTBSY: refuse write access to a binary that is being executed.
     * Linux returns -ETXTBSY (-26) for open(O_WRONLY/O_RDWR) on a running
     * binary. zsh and similar programs may try this — without the check,
     * they corrupt their own .text segment on disk. */
    if ((flags & O_ACCMODE) != O_RDONLY) {
        extern bool proc_is_executing(const char *name);
        if (proc_is_executing(lookup)) {
            memset(f, 0, sizeof(*f));
            return -26; /* ETXTBSY */
        }
    }

    f->open   = true;
    f->type   = FD_TYPE_FILE;
    f->oflags = (uint32_t)flags;
    f->offset = 0;

    if (flags & O_APPEND)
        f->offset = f->node.size;

    if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        if (f->node.fs_version != 2) {
            memset(f, 0, sizeof(*f));
            return -EROFS;
        }
        if (osfs2_truncate(f->node.data, 0) < 0) {
            memset(f, 0, sizeof(*f));
            return -EIO;
        }
        f->node.size = 0;
        f->offset = 0;
    }

    if (f->node.fs_version == 2) {
        const char *resolved = osfs2_file_name(f->node.data);
        if (sys_debug_is_fx_path(lookup) || sys_debug_is_fx_path(resolved)) {
            serial_puts("[sys_open] fx path='");
            serial_puts(path);
            serial_puts("' lookup='");
            serial_puts(lookup);
            serial_puts("' resolved='");
            serial_puts(resolved ? resolved : "(null)");
            serial_puts("' size=");
            serial_putdec(f->node.size);
            serial_puts(" fd=");
            serial_putdec((uint64_t)newfd);
            serial_puts("\n");
        }
        if (osfs2_file_retain(f->node.data) < 0) {
            memset(f, 0, sizeof(*f));
            return -EIO;
        }
    }

    return newfd;
}

static int64_t close_fd_in_table(fd_entry_t *table, uint64_t fd)
{
    if (!table || fd >= MAX_FDS || !table[fd].open) return -EBADF;

    fd_entry_t *f = &table[fd];

    if (f->type == FD_TYPE_PIPE && f->pipe) {
        pipe_buf_t *p = (pipe_buf_t *)f->pipe;
        /* Determine if this is read or write end via oflags */
        if ((f->oflags & O_ACCMODE) == O_RDONLY) {
            if (p->read_refs > 0) p->read_refs--;
        } else {
            if (p->write_refs > 0) p->write_refs--;
        }
        /* Free pipe when both ends have no more references */
        if (p->read_refs == 0 && p->write_refs == 0)
            p->in_use = false;
    }

    if (f->type == FD_TYPE_FILE) {
        if (f->node.fs_version == 2)
            osfs2_file_release(f->node.data);
        /* Embedded node, no need to free but we clear version for safety */
        f->node.fs_version = 0;
    }

    if (f->type == FD_TYPE_SOCKET)
        sock_close(f->socket_idx);

    f->open = false;
    return 0;
}

static int64_t sys_close(uint64_t fd)
{
    return close_fd_in_table(fd_table, fd);
}

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type == FD_TYPE_DIR) {
        if (whence != SEEK_SET || offset < 0) return -EINVAL;
        f->offset = (uint64_t)offset;
        return offset;
    }
    if (f->type != FD_TYPE_FILE) return -ESPIPE;

    int64_t new_off;
    uint64_t file_size = f->node.fs_version == 2
                       ? osfs2_file_size(f->node.data) : f->node.size;
    f->node.size = file_size;

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
        if (f->node.fs_version == 2)
            f->node.size = osfs2_file_size(f->node.data);
        st->st_mode = 0100644;  /* S_IFREG | 0644 */
        if (f->node.fs_version == 3 && osfs3_is_dir(f->node.ino))
            st->st_mode = 0040755; /* S_IFDIR | 0755 */
        st->st_size = (int64_t)f->node.size;
        st->st_blksize = 4096;
        st->st_blocks = (st->st_size + 511) / 512;
        st->st_nlink = 1;
        if (f->node.fs_version == 2) {
            st->st_mtime_sec = osfs2_file_mtime(f->node.data);
            st->st_ctime_sec = osfs2_file_ctime(f->node.data);
        }
    } else if (f->type == FD_TYPE_DIR) {
        st->st_mode = 0040755;  /* S_IFDIR | 0755 */
        st->st_nlink = 2;
        st->st_blksize = 4096;
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

static int64_t sys_fsync(uint64_t fd)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type == FD_TYPE_DIR)
        return disk_flush() == 0 ? 0 : -EIO;
    if (f->type != FD_TYPE_FILE) return -EINVAL;
    if (f->node.fs_version != 2) return 0;
    return disk_flush() == 0 ? 0 : -EIO;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t length)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -EINVAL;
    if ((f->oflags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->node.fs_version != 2) return -EROFS;
    if (osfs2_truncate(f->node.data, length) < 0) return -ENOTSUP;
    f->node.size = length;
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

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset);

/* brk uses a TGID-owned VMA. The full range is only a virtual reservation;
 * physical pages are committed by demand_page_fault when userspace touches
 * an address below the current break. */
int64_t sys_brk(uint64_t addr)
{
    vma_t *vma = brk_vma_current();

    if (!vma) {
        int64_t base = sys_mmap(0, BRK_HEAP_SIZE, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS,
                                (uint64_t)-1, 0);
        if (base < 0) {
            serial_puts("[BRK] reserve failed size=");
            serial_putdec(BRK_HEAP_SIZE / (1024 * 1024));
            serial_puts("MB rc=");
            serial_putdec((uint64_t)(-base));
            serial_puts("\n");
            return 0;
        }

        for (int i = 0; i < MAX_VMAS; i++) {
            if (!vma_table[i].in_use ||
                vma_table[i].base != (uint64_t)base ||
                vma_table[i].type != VMA_ANON ||
                !vma_owned_by_current(&vma_table[i]))
                continue;
            vma = &vma_table[i];
            break;
        }
        if (!vma) {
            serial_puts("[BRK] reserved range missing VMA\n");
            return 0;
        }

        vma->type = VMA_BRK;
        vma->prot = PROT_READ | PROT_WRITE;
        vma->file_size = 0; /* Current break offset from base. */

        serial_puts("[BRK] reserve base=0x");
        serial_puthex(vma->base, 16);
        serial_puts(" virtual=");
        serial_putdec(vma->pages * 4);
        serial_puts("KB resident=0KB tgid=");
        serial_putdec((uint64_t)(uint32_t)proc_current_tgid());
        serial_puts("\n");
    }

    uint64_t current = vma->base + vma->file_size;
    uint64_t limit = vma->base + vma->pages * 4096ULL;
    if (addr == 0)
        return (int64_t)current;

    if (addr < vma->base || addr > limit) {
        serial_puts("[BRK] reject addr=0x");
        serial_puthex(addr, 16);
        serial_puts(" cur=0x");
        serial_puthex(current, 16);
        serial_puts(" base=0x");
        serial_puthex(vma->base, 16);
        serial_puts(" max=0x");
        serial_puthex(limit, 16);
        serial_puts("\n");
        return (int64_t)current;
    }

    if (addr < current)
        brk_decommit_tail(vma, addr - vma->base, vma->file_size);
    vma->file_size = addr - vma->base;
    return (int64_t)addr;
}

/* ── mmap/munmap/mprotect (X-MMAP) ────────────────────────────── */

static uint64_t prot_to_pte_flags(uint32_t prot)
{
    /* Lower-half process mappings must not be global: CR3 switches need to
     * flush them, otherwise two processes can alias stale TLB entries. */
    uint64_t flags = PTE_PRESENT;
    if (prot & PROT_WRITE)
        flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC))
        flags |= PTE_NX;
    return flags;
}

extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);

static int mmap_commit_anon_first_page(uint64_t va, uint32_t prot)
{
    if (prot == PROT_NONE)
        return 0;

    void *phys = mem_alloc_pages(1);
    if (!phys)
        return -ENOMEM;

    memset(PHYS_TO_VIRT(phys), 0, 4096);

    uint64_t pte_flags = prot_to_pte_flags(prot);
    uint64_t cr3 = proc_current_cr3();
    int rc = cr3 ? paging_map_page_in_cr3(cr3, va, (uint64_t)phys, pte_flags)
                 : paging_map_page(va, (uint64_t)phys, pte_flags);
    if (rc != 0) {
        mem_free_pages(phys, 1);
        return -ENOMEM;
    }

    return 0;
}

/* mmap/brk, Win32 VirtualAlloc and PE images maintain distinct ownership and
 * teardown metadata, but they share one process page table. Consult all three
 * before selecting an automatic address so a reservation cannot cover a DLL
 * that is already mapped (or vice versa). */
extern uint64_t nt_vm_range_conflict_end(uint64_t base, uint64_t size)
    __attribute__((weak));
extern uint64_t pe_va_range_conflict_end(uint64_t base, uint64_t size)
    __attribute__((weak));

static uint64_t mmap_range_conflict_end_current(uint64_t base, uint64_t size)
{
    uint64_t conflict_end = syscall_vma_range_conflict_end(base, size);
    if (conflict_end == UINT64_MAX)
        return UINT64_MAX;

    if (nt_vm_range_conflict_end) {
        uint64_t nt_end = nt_vm_range_conflict_end(base, size);
        if (nt_end == UINT64_MAX)
            return UINT64_MAX;
        if (nt_end > conflict_end)
            conflict_end = nt_end;
    }

    if (pe_va_range_conflict_end) {
        uint64_t pe_end = pe_va_range_conflict_end(base, size);
        if (pe_end == UINT64_MAX)
            return UINT64_MAX;
        if (pe_end > conflict_end)
            conflict_end = pe_end;
    }

    uint64_t cr3 = proc_current_cr3();
    /* The kernel CR3 identity-maps low physical memory. Those PTEs are not
     * user allocations; the explicit VMA/NT/PE registries above are the
     * authoritative occupancy source in that legacy address space. */
    if (cr3 && cr3 != paging_get_kernel_cr3()) {
        uint64_t mapped_end =
            paging_first_mapped_end_in_cr3(cr3, base, size);
        if (mapped_end == UINT64_MAX)
            return UINT64_MAX;
        if (mapped_end > conflict_end)
            conflict_end = mapped_end;
    }

    return conflict_end;
}

static uint64_t mmap_find_free_range(uint64_t base, uint64_t limit,
                                     uint64_t size)
{
    uint64_t cursor = base;
    while (cursor < limit) {
        if (size > limit - cursor)
            return 0;

        uint64_t conflict_end =
            mmap_range_conflict_end_current(cursor, size);
        if (!conflict_end)
            return cursor;
        if (conflict_end == UINT64_MAX)
            return 0;

        uint64_t next = (conflict_end + 4095ULL) & ~4095ULL;
        if (next <= cursor)
            return 0;
        cursor = next;
    }
    return 0;
}

/*
 * sys_mmap — MAP_ANONYMOUS + MAP_FILE, demand-paged.
 * Reserves a virtual address range from the user-VA allocator and
 * returns it; physical pages are installed lazily by demand_page_fault
 * on first access (anonymous) or first read (file-backed).
 * Linux ABI: mmap(addr, length, prot, flags, fd, offset)
 *   args: a1=addr, a2=length, a3=prot, a4=flags, a5(R8)=fd, a6(R9)=offset
 *   R10 carries flags (a4), R8 carries fd (a5), and R9 carries offset (a6).
 */
static int64_t sys_munmap(uint64_t addr, uint64_t length);

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    if (length == 0) return -EINVAL;
    if (length > 0x0000800000000000ULL - 4095)
        return -EINVAL;

    uint64_t npages = (length + 4095) / 4096;
    if (npages == 0 || npages > (0x0000800000000000ULL / 4096))
        return -EINVAL;

    bool fixed = (flags & MAP_FIXED) != 0;
    if (fixed) {
        if (addr < 0x1000 || (addr & 0xFFF))
            return -EINVAL;
        if (addr + npages * 4096 < addr)
            return -EINVAL;
        /* Linux MAP_FIXED replaces existing mappings in the target range.
         * Keep the behavior simple: remove owned overlapping VMAs if present,
         * then install the new demand-paged VMA below. */
        (void)sys_munmap(addr, npages * 4096);
    }

    /* File-backed mmap: demand-paged (pages loaded on first access) */
    if (!(flags & MAP_ANONYMOUS)) {
        if (offset & 0xFFF) return -EINVAL;
        if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
        fd_entry_t *f = &fd_table[fd];
        if (f->type != FD_TYPE_FILE) return -EBADF;

        int vi = -1;
        for (int i = 0; i < MAX_VMAS; i++) {
            if (!vma_table[i].in_use) { vi = i; break; }
        }
        if (vi < 0) {
            serial_puts("[MMAP] fail file no-vma len=");
            serial_putdec(length);
            serial_puts("\n");
            return -ENOMEM;
        }

        /* Reserve virtual address range (no physical pages allocated) */
        static uint64_t mmap_file_base = 0x600000000ULL;
        uint64_t result;
        if (fixed) {
            result = addr;
        } else {
            result = mmap_file_base;
            mmap_file_base += npages * 4096;
        }

        /* How much of this mapping is backed by file data? */
        uint64_t fsize = f->node.size;
        uint64_t backing = offset < fsize ? fsize - offset : 0;
        if (backing > length) backing = length;

        vma_table[vi].base        = result;
        vma_table[vi].pages       = npages;
        vma_table[vi].prot        = (uint32_t)prot;
        vma_table[vi].in_use      = true;
        vma_table[vi].type        = VMA_FILE_MMAP;
        vma_table[vi].file_node   = f->node;
        vma_table[vi].file_offset = offset;
        vma_table[vi].file_size   = backing;
        vma_set_owner(&vma_table[vi], proc_current());

        return (int64_t)result;
    }

    /* Anonymous mapping — handles both PROT_NONE reservations and
     * PROT_READ|PROT_WRITE writable regions through the same VA
     * allocator. Physical pages are installed lazily by
     * demand_page_fault on first access (writable) or by a later
     * mprotect(PROT_READ|PROT_WRITE) upgrade (PROT_NONE). */
    if (fd != (uint64_t)-1 && !(flags & MAP_ANONYMOUS))
        return -EBADF;

    /* Find free VMA slot */
    int vi = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) { vi = i; break; }
    }
    if (vi < 0) {
        serial_puts("[MMAP] fail anon no-vma len=");
        serial_putdec(length);
        serial_puts("\n");
        return -ENOMEM;
    }

    /* Keep pthread stacks in the shared upper-half mirror while an ISR or
     * syscall frame is still unwinding across a scheduler CR3 switch. */
    if (flags & MAP_STACK) {
        void *phys = mem_alloc_pages(npages);
        if (!phys) return -ENOMEM;
        uint64_t result = (uint64_t)PHYS_TO_VIRT(phys);
        memset((void *)result, 0, npages * 4096);
        vma_table[vi].base = result;
        vma_table[vi].pages = npages;
        vma_table[vi].prot = (uint32_t)prot;
        vma_table[vi].in_use = true;
        vma_table[vi].type = VMA_SHARED_STACK;
        vma_set_owner(&vma_table[vi], proc_current());
        return (int64_t)result;
    }

    /* Allocate a user-space VA from the 32-bit-clean anonymous pool. Address
     * spaces are process-private, so scan each process from the pool floor;
     * a global monotonic cursor needlessly fragmented later processes. */
    uint64_t result;
    if (fixed) {
        result = addr;
    } else if (addr && (addr & 0xFFF) == 0 &&
               !mmap_range_conflict_end_current(addr, npages * 4096ULL)) {
        result = addr;
    } else {
        uint64_t bytes = npages * 4096ULL;
        result = mmap_find_free_range(MMAP_ANON_LOW_BASE,
                                      MMAP_ANON_LOW_LIMIT, bytes);
        if (!result) {
            result = mmap_find_free_range(MMAP_ANON_HIGH_BASE,
                                          MMAP_ANON_HIGH_LIMIT, bytes);
            if (!result) {
                serial_puts("[MMAP] fail anon no-range bytes=");
                serial_putdec(bytes);
                serial_puts("\n");
                return -ENOMEM;
            }
            serial_puts("[MMAP] anon high base=0x");
            serial_puthex(result, 16);
            serial_puts(" bytes=");
            serial_putdec(bytes);
            serial_puts("\n");
        }
    }

    vma_table[vi].base   = result;
    vma_table[vi].pages  = npages;
    vma_table[vi].prot   = (uint32_t)prot;
    vma_table[vi].in_use = true;
    vma_table[vi].type   = VMA_ANON;
    vma_set_owner(&vma_table[vi], proc_current());

    int commit_rc = mmap_commit_anon_first_page(result, (uint32_t)prot);
    if (commit_rc < 0) {
        serial_puts("[MMAP] fail anon commit addr=0x");
        serial_puthex(result, 16);
        serial_puts(" len=");
        serial_putdec(length);
        serial_puts(" prot=0x");
        serial_puthex(prot, 8);
        serial_puts("\n");
        vma_clear_slot(&vma_table[vi]);
        return commit_rc;
    }

    return (int64_t)result;
}

/* sys_munmap — unmap pages allocated by mmap.
 * Walks PTEs to free individually-faulted pages (demand paging safe). */
static int64_t sys_munmap(uint64_t addr, uint64_t length)
{
    if (!addr || (addr & 0xFFF)) return -EINVAL;
    if (length == 0) return -EINVAL;

    uint64_t npages = (length + 4095) / 4096;
    uint64_t unmap_end = addr + npages * 4096;
    bool unmapped_any = false;

    for (;;) {
        bool progress = false;

        for (int i = 0; i < MAX_VMAS; i++) {
            if (!vma_table[i].in_use) continue;
            if (!vma_owned_by_current(&vma_table[i])) continue;

            uint64_t vma_start = vma_table[i].base;
            uint64_t vma_end = vma_start + vma_table[i].pages * 4096;
            uint64_t overlap_start = addr > vma_start ? addr : vma_start;
            uint64_t overlap_end = unmap_end < vma_end ? unmap_end : vma_end;
            if (overlap_start >= overlap_end) continue;

            /* pthread_join wakes when clear_child_tid becomes zero, slightly
             * before the child has stopped executing its SYS_exit epilogue.
             * A parent munmap of that stack can therefore race with the final
             * child stack writes. Detached musl threads also unmap themselves.
             * In both cases keep the mapping until the zombie reaper runs. */
            extern bool proc_is_thread_of(void *p);
            bool thread_stack = vma_table[i].owner &&
                proc_is_thread_of(vma_table[i].owner) &&
                (vma_table[i].type == VMA_SHARED_STACK ||
                 vma_table[i].type == VMA_THREAD_STACK);
            extern volatile uint64_t syscall_user_rsp;
            bool active_stack = syscall_user_rsp >= overlap_start &&
                syscall_user_rsp < overlap_end;
            if (thread_stack || active_stack) {
                if (overlap_start != vma_start || overlap_end != vma_end) {
                    int ti = vma_split_for_range(
                        i, overlap_start, (overlap_end - overlap_start) / 4096);
                    if (ti < 0) return ti;
                    i = ti;
                }
                vma_table[i].type =
                    vma_table[i].type == VMA_SHARED_STACK
                    ? VMA_DEFERRED_SHARED_STACK : VMA_DEFERRED_STACK;
                if (!thread_stack)
                    vma_table[i].owner = proc_current();
                serial_puts("[MMAP] deferred active stack unmap base=0x");
                serial_puthex(vma_table[i].base, 16);
                serial_puts(" pages=");
                serial_putdec(vma_table[i].pages);
                serial_puts(" rsp=0x");
                serial_puthex(syscall_user_rsp, 16);
                serial_puts(" pid=");
                serial_putdec((uint64_t)proc_current_pid());
                serial_puts("\n");
                return 0;
            }

            if (overlap_start != vma_start || overlap_end != vma_end) {
                int ti = vma_split_for_range(i, overlap_start,
                                             (overlap_end - overlap_start) / 4096);
                if (ti < 0) return ti;
                i = ti;
            }

            vma_free_pages(&vma_table[i]);
            vma_clear_slot(&vma_table[i]);
            unmapped_any = true;
            progress = true;
            break;
        }

        if (!progress)
            break;
    }

    return unmapped_any ? 0 : -EINVAL;
}

/* sys_mprotect — change protection flags on mapped pages.
 *
 * Demand paging interaction: a partial mprotect first splits the VMA so only
 * the requested range changes protection. Pages that are already faulted-in get
 * their PTEs updated; pages still not-present pick up the new flags when
 * demand_page_fault() consults the range's VMA. */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);

static int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot)
{
    if (!addr || (addr & 0xFFF)) return -EINVAL;
    if (length == 0) return -EINVAL;

    uint64_t npages = (length + 4095) / 4096;

    /* Find VMA containing this range owned by current process */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (!vma_owned_by_current(&vma_table[i])) continue;
        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096;
        if (addr >= vma_table[i].base && addr + npages * 4096 <= vma_end) {
            int ti = vma_split_for_range(i, addr, npages);
            if (ti < 0) return ti;

            uint32_t old_prot = vma_table[ti].prot;
            uint64_t pte_flags = prot_to_pte_flags((uint32_t)prot);
            uint64_t cr3 = proc_current_cr3();

            /* Committing a PROT_NONE reservation: allocate real pages */
            if (old_prot == 0 && prot != 0) {
                for (uint64_t p = 0; p < npages; p++) {
                    uint64_t va = addr + p * 4096;
                    if (vma_set_present_page_flags(cr3, va, pte_flags) != 0)
                        return -ENOMEM;
                    uint64_t *pte = cr3 ? paging_get_pte_in_cr3(cr3, va)
                                        : paging_get_pte(va);
                    if (pte && (*pte & PTE_PRESENT_BIT))
                        continue;
                    void *phys = mem_alloc_pages(1);
                    if (!phys) return -ENOMEM;
                    memset(PHYS_TO_VIRT(phys), 0, 4096);
                    int rc = cr3 ? paging_map_page_in_cr3(cr3, va, (uint64_t)phys, pte_flags)
                                 : paging_map_page(va, (uint64_t)phys, pte_flags);
                    if (rc != 0) {
                        mem_free_pages(phys, 1);
                        return -ENOMEM;
                    }
                }
            } else {
                /* Update existing page table entries */
                for (uint64_t p = 0; p < npages; p++) {
                    uint64_t va = addr + p * 4096;
                    if (vma_set_present_page_flags(cr3, va, pte_flags) != 0)
                        return -ENOMEM;
                }
            }

            vma_table[ti].prot = (uint32_t)prot;
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

    /* Find matching VMA owned by current process */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (!vma_owned_by_current(&vma_table[i])) continue;
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
 * Validates fault address against VMA table. For file-backed VMAs,
 * reads the specific 4KB page from NVMe. For anonymous VMAs, returns
 * a zero-filled page. Returns 0 on success, -1 on failure (SIGSEGV). */
extern int paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
extern void *mem_alloc_pages(uint64_t count);
extern void  mem_free_pages(void *addr, uint64_t count);

static uint64_t prot_to_pte_flags(uint32_t prot);

extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                  uint64_t phys, uint64_t flags);

int demand_page_fault(uint64_t addr, uint64_t error_code)
{
    /* Only handle not-present faults (bit 0 clear) */
    if (error_code & 1) return -1;

    uint64_t page_addr = addr & ~0xFFFULL;
    static uint32_t dpf_fail_logs;

    /* Find VMA containing this address, owned by the current process.
     * X-PGTBL: multiple demand-paged processes may have overlapping VMA
     * ranges (e.g. two musl statics both loaded at 0x400000), so the
     * owner filter is essential to pick the right one. */
    vma_t *vma = NULL;
    vma_t *range_mismatch = NULL;
    int range_mismatch_idx = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        uint64_t vma_end = vma_table[i].base + vma_table[i].pages * 4096;
        if (addr >= vma_table[i].base && addr < vma_end) {
            if (vma_owned_by_current(&vma_table[i])) {
                vma = &vma_table[i];
                break;
            }
            if (!range_mismatch) {
                range_mismatch = &vma_table[i];
                range_mismatch_idx = i;
            }
        }
    }
    if (!vma) {
        if (dpf_fail_logs < 16) {
            serial_puts("[DPF] no VMA addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" page=0x");
            serial_puthex(page_addr, 16);
            serial_puts(" pid=");
            serial_putdec((uint64_t)(uint32_t)proc_current_pid());
            serial_puts(" tgid=");
            serial_putdec((uint64_t)(uint32_t)proc_current_tgid());
            serial_puts(" cr3=0x");
            serial_puthex(proc_current_cr3(), 16);
            if (range_mismatch) {
                serial_puts(" range-owner idx=");
                serial_putdec((uint64_t)range_mismatch_idx);
                serial_puts(" base=0x");
                serial_puthex(range_mismatch->base, 16);
                serial_puts(" pages=0x");
                serial_puthex(range_mismatch->pages, 8);
                serial_puts(" owner_tgid=");
                serial_putdec(range_mismatch->owner_tgid);
            }
            serial_puts("\n");
            dpf_fail_logs++;
        }
        /* Check quarantine before killing — may be use-after-free */
        extern int32_t proc_current_pid(void);
        quarantine_check_uaf(addr, (uint32_t)proc_current_pid());
        return -1;  /* No VMA → SIGSEGV (quarantine_check_uaf logged if UAF) */
    }
    if (vma->type == VMA_BRK &&
        addr >= vma->base + vma->file_size) {
        if (dpf_fail_logs < 16) {
            serial_puts("[DPF] above brk addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" break=0x");
            serial_puthex(vma->base + vma->file_size, 16);
            serial_puts("\n");
            dpf_fail_logs++;
        }
        return -1;
    }
    if (vma->prot == PROT_NONE) {
        if (dpf_fail_logs < 16) {
            serial_puts("[DPF] PROT_NONE addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" base=0x");
            serial_puthex(vma->base, 16);
            serial_puts("\n");
            dpf_fail_logs++;
        }
        return -1;
    }

    /* Allocate a physical page, zero-filled via the upper-half mirror.
     * CPU writes (memset + vfs_read) go through the kernel direct map;
     * the phys value below is what later gets written into the user
     * PTE by paging_map_page_in_cr3. */
    void *phys = mem_alloc_pages(1);
    if (!phys) {
        if (dpf_fail_logs < 16) {
            serial_puts("[DPF] alloc fail addr=0x");
            serial_puthex(addr, 16);
            serial_puts("\n");
            dpf_fail_logs++;
        }
        return -1;
    }
    void *page_virt = PHYS_TO_VIRT(phys);
    memset(page_virt, 0, 4096);

    /* For file-backed VMAs, read file data into the page */
    if (vma->type == VMA_FILE_ELF || vma->type == VMA_FILE_MMAP) {
        uint64_t offset_in_vma = page_addr - vma->base;
        if (offset_in_vma < vma->file_size) {
            uint64_t to_read = 4096;
            if (offset_in_vma + 4096 > vma->file_size)
                to_read = vma->file_size - offset_in_vma;
            vfs_node_t node_copy = vma->file_node;
            if (vfs_read(&node_copy, vma->file_offset + offset_in_vma,
                         page_virt, to_read) < 0) {
                mem_free_pages(phys, 1);
                if (dpf_fail_logs < 16) {
                    serial_puts("[DPF] read fail addr=0x");
                    serial_puthex(addr, 16);
                    serial_puts(" off=0x");
                    serial_puthex(vma->file_offset + offset_in_vma, 16);
                    serial_puts("\n");
                    dpf_fail_logs++;
                }
                return -1;
            }
        }
        /* Pages beyond file_size stay zero (BSS) */
    }

    /* Map with protection flags from VMA, into the current process's
     * page tables (X-PGTBL). Fall back to kernel mapping for processes
     * without a per-process CR3 (e.g. the idle/kernel task). */
    uint64_t pte_flags = prot_to_pte_flags(vma->prot);
    uint64_t cr3 = proc_current_cr3();
    int rc = cr3 ? paging_map_page_in_cr3(cr3, page_addr, (uint64_t)phys, pte_flags)
                 : paging_map_page(page_addr, (uint64_t)phys, pte_flags);
    if (rc != 0) {
        mem_free_pages(phys, 1);
        if (dpf_fail_logs < 16) {
            serial_puts("[DPF] map fail addr=0x");
            serial_puthex(addr, 16);
            serial_puts(" page=0x");
            serial_puthex(page_addr, 16);
            serial_puts(" cr3=0x");
            serial_puthex(cr3, 16);
            serial_puts(" base=0x");
            serial_puthex(vma->base, 16);
            serial_puts(" pages=0x");
            serial_puthex(vma->pages, 8);
            serial_puts(" prot=0x");
            serial_puthex(vma->prot, 4);
            serial_puts(" type=");
            serial_putdec(vma->type);
            serial_puts("\n");
            dpf_fail_logs++;
        }
        return -1;
    }

    /* Speculative prefetch: pre-fault the next 3 pages if they're within
     * the same VMA and not yet present. Spatial locality means sequential
     * code/data access patterns will hit these pages shortly. Each
     * prefault is ~10us of NVMe DMA vs ~10ms of demand fault latency. */
    if (cr3 && vma->type == VMA_FILE_ELF) {
        uint64_t vma_end = vma->base + vma->pages * 4096;
        for (int pf = 1; pf <= 3; pf++) {
            uint64_t next_va = page_addr + (uint64_t)pf * 4096;
            if (next_va >= vma_end) break;

            /* Check if page already mapped (avoid double-fault) */
            extern uint64_t *paging_get_pte_in_cr3(uint64_t cr3, uint64_t va);
            uint64_t *pte = paging_get_pte_in_cr3(cr3, next_va);
            if (pte && (*pte & 1)) continue;  /* already present */

            void *pf_phys = mem_alloc_pages(1);
            if (!pf_phys) break;
            void *pf_virt = PHYS_TO_VIRT(pf_phys);
            memset(pf_virt, 0, 4096);

            uint64_t off_in_vma = next_va - vma->base;
            if (off_in_vma < vma->file_size) {
                uint64_t to_read = 4096;
                if (off_in_vma + 4096 > vma->file_size)
                    to_read = vma->file_size - off_in_vma;
                vfs_node_t nc = vma->file_node;
                vfs_read(&nc, vma->file_offset + off_in_vma, pf_virt, to_read);
            }

            paging_map_page_in_cr3(cr3, next_va, (uint64_t)pf_phys, pte_flags);
        }
    }

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
    if (fd < MAX_FDS && fd_table[fd].open &&
        fd_table[fd].type == FD_TYPE_SOCKET) {
        int sock_idx = fd_table[fd].socket_idx;
        if (request == 0x541B) { /* FIONREAD */
            if (!arg) return -EFAULT;
            int pending = sock_pending_bytes(sock_idx);
            if (pending < 0) return pending;
            *(int *)arg = pending;
            return 0;
        }

        if (!arg) return -EFAULT;
        uint8_t *ifreq = (uint8_t *)arg;
        if (request == 0x8915) { /* SIOCGIFADDR */
            memset(ifreq + 16, 0, 24);
            *(uint16_t *)(ifreq + 16) = 2; /* AF_INET */
            memcpy(ifreq + 20, net_get_ip_ptr(), 4);
            return 0;
        }
        if (request == 0x8927) { /* SIOCGIFHWADDR */
            memset(ifreq + 16, 0, 24);
            *(uint16_t *)(ifreq + 16) = 1; /* ARPHRD_ETHER */
            net_get_mac(ifreq + 18);
            return 0;
        }
        if (request == 0x8921) { /* SIOCGIFMTU */
            *(int32_t *)(ifreq + 16) = 1500;
            return 0;
        }
    }

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
        if (f->type == FD_TYPE_SOCKET) {
            int pending = sock_pending_bytes(f->socket_idx);
            if ((fds[i].events & POLLIN) && pending > 0)
                fds[i].revents |= POLLIN;
            if (fds[i].events & POLLOUT)
                fds[i].revents |= POLLOUT;
            if (pending < 0)
                fds[i].revents |= POLLERR;
        }
        if (fds[i].revents) ready++;
    }
    return ready;
}

#define FDSET_WORDS 16

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} select_timeval_t;

static int64_t sys_select(uint64_t nfds, uint64_t readfds_addr,
                          uint64_t writefds_addr, uint64_t exceptfds_addr,
                          uint64_t timeout_addr)
{
    uint64_t read_req[FDSET_WORDS] = {0};
    uint64_t write_req[FDSET_WORDS] = {0};
    uint64_t except_req[FDSET_WORDS] = {0};
    uint64_t read_out[FDSET_WORDS];
    uint64_t write_out[FDSET_WORDS];
    uint64_t except_out[FDSET_WORDS];

    if (readfds_addr) memcpy(read_req, (void *)readfds_addr, sizeof(read_req));
    if (writefds_addr) memcpy(write_req, (void *)writefds_addr, sizeof(write_req));
    if (exceptfds_addr) memcpy(except_req, (void *)exceptfds_addr, sizeof(except_req));
    if (nfds > MAX_FDS) nfds = MAX_FDS;

    uint64_t timeout_ticks = UINT64_MAX;
    if (timeout_addr) {
        select_timeval_t *tv = (select_timeval_t *)timeout_addr;
        uint64_t usec = (tv->tv_sec > 0 ? (uint64_t)tv->tv_sec : 0) * 1000000ULL;
        if (tv->tv_usec > 0) usec += (uint64_t)tv->tv_usec;
        timeout_ticks = (usec + 9999) / 10000;
    }
    uint64_t start = idt_get_ticks();

    for (;;) {
        memset(read_out, 0, sizeof(read_out));
        memset(write_out, 0, sizeof(write_out));
        memset(except_out, 0, sizeof(except_out));
        int ready = 0;

        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint64_t mask = 1ULL << (fd & 63);
            uint64_t word = fd >> 6;
            bool want_read = (read_req[word] & mask) != 0;
            bool want_write = (write_req[word] & mask) != 0;
            bool want_except = (except_req[word] & mask) != 0;
            if (!want_read && !want_write && !want_except) continue;
            if (!fd_table[fd].open) return -EBADF;

            fd_entry_t *entry = &fd_table[fd];
            bool fd_ready = false;
            if (entry->type == FD_TYPE_SOCKET) {
                int pending = sock_pending_bytes(entry->socket_idx);
                if (want_read && pending > 0) {
                    read_out[word] |= mask;
                    fd_ready = true;
                }
                if (want_write && pending >= 0) {
                    write_out[word] |= mask;
                    fd_ready = true;
                }
                if (want_except && pending < 0) {
                    except_out[word] |= mask;
                    fd_ready = true;
                }
            } else {
                if (want_read && (entry->type == FD_TYPE_FILE ||
                    entry->type == FD_TYPE_PIPE || entry->type == FD_TYPE_DEV)) {
                    read_out[word] |= mask;
                    fd_ready = true;
                }
                if (want_write && entry->type != FD_TYPE_PROC &&
                    entry->type != FD_TYPE_DIR) {
                    write_out[word] |= mask;
                    fd_ready = true;
                }
            }
            if (fd_ready) ready++;
        }

        if (ready > 0 || timeout_ticks == 0 ||
            (timeout_ticks != UINT64_MAX && idt_get_ticks() - start >= timeout_ticks)) {
            if (readfds_addr) memcpy((void *)readfds_addr, read_out, sizeof(read_out));
            if (writefds_addr) memcpy((void *)writefds_addr, write_out, sizeof(write_out));
            if (exceptfds_addr) memcpy((void *)exceptfds_addr, except_out, sizeof(except_out));
            return ready;
        }

        __asm__ volatile ("sti; hlt; cli" ::: "memory");
    }
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
    char norm[256];
    const char *lookup = normalized_lookup(path, norm, sizeof(norm));
    if (strlen(lookup) >= 64) return -ENAMETOOLONG;
    if (osfs2_path_is_dir(lookup)) return -EISDIR;
    int result = osfs2_delete(lookup);
    if (result == 0) return 0;
    return result == -2 ? -EBUSY : -ENOENT;
}

static int64_t sys_rename_impl(uint64_t from_addr, uint64_t to_addr,
                               bool replace)
{
    const char *from = (const char *)from_addr;
    const char *to = (const char *)to_addr;
    if (!from || !to) return -EFAULT;

    char from_norm[256], to_norm[256];
    const char *from_lookup = normalized_lookup(from, from_norm, sizeof(from_norm));
    const char *to_lookup = normalized_lookup(to, to_norm, sizeof(to_norm));
    if (!from_lookup[0] || !to_lookup[0]) return -EISDIR;
    if (strlen(from_lookup) >= 64 || strlen(to_lookup) >= 64)
        return -ENAMETOOLONG;
    if (osfs2_path_is_dir(from_lookup) || osfs2_path_is_dir(to_lookup))
        return -EISDIR;
    if (!osfs2_find(from_lookup)) return -ENOENT;
    int result = osfs2_rename(from_lookup, to_lookup, replace);
    if (result == 0) return 0;
    if (result == -2) return -EEXIST;
    if (result == -3) return -EBUSY;
    return -EIO;
}

static int64_t sys_rename(uint64_t from_addr, uint64_t to_addr)
{
    return sys_rename_impl(from_addr, to_addr, true);
}

static int64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath,
                            uint64_t newdirfd, uint64_t newpath,
                            uint64_t flags)
{
    if ((int32_t)olddirfd != AT_FDCWD || (int32_t)newdirfd != AT_FDCWD)
        return -ENOTSUP;
    if (flags & ~1ULL) return -EINVAL;
    return sys_rename_impl(oldpath, newpath, !(flags & 1ULL));
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
    p->read_refs = 1;
    p->write_refs = 1;

    /* Read end */
    fd_entry_t *rf = &fd_table[rfd];
    memset(rf, 0, sizeof(*rf));
    rf->open   = true;
    rf->type   = FD_TYPE_PIPE;
    rf->oflags = O_RDONLY;
    rf->pipe   = p;

    /* Write end */
    fd_entry_t *wf = &fd_table[wfd];
    memset(wf, 0, sizeof(*wf));
    wf->open   = true;
    wf->type   = FD_TYPE_PIPE;
    wf->oflags = O_WRONLY;
    wf->pipe   = p;

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

    /* Bump pipe refcount: the new fd entry holds an additional
     * reference to the underlying pipe_buf_t. sys_close will
     * decrement it. */
    if (fd_table[newfd].type == FD_TYPE_PIPE && fd_table[newfd].pipe) {
        pipe_buf_t *p = (pipe_buf_t *)fd_table[newfd].pipe;
        if ((fd_table[newfd].oflags & O_ACCMODE) == O_RDONLY)
            p->read_refs++;
        else
            p->write_refs++;
    } else if (fd_table[newfd].type == FD_TYPE_FILE &&
               fd_table[newfd].node.fs_version == 2) {
        osfs2_file_retain(fd_table[newfd].node.data);
    }

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

/* ── proc_signal_pid — send signal to a process (used by timers) ── */
void proc_signal_pid(uint32_t pid, int sig)
{
    /* For now only support signaling current process */
    if ((int32_t)pid == proc_current_pid()) {
        sig_pending |= (1U << sig);
    }
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

static int64_t sys_sched_getaffinity(uint64_t pid, uint64_t cpusetsize,
                                     uint64_t mask_addr)
{
    const uint64_t kernel_mask_size = sizeof(uint64_t);
    if (!mask_addr) return -EFAULT;
    if (cpusetsize < kernel_mask_size) return -EINVAL;

    if ((int64_t)pid < 0) return -ESRCH;
    if (pid != 0 && (int64_t)pid != proc_current_pid()) {
        extern void *proc_find_ptr(uint16_t pid);
        if (pid > 0xFFFFu || !proc_find_ptr((uint16_t)pid))
            return -ESRCH;
    }

    extern uint32_t smp_cpu_count(void);
    uint32_t cpus = smp_cpu_count();
    uint64_t mask = cpus >= 64 ? UINT64_MAX : ((1ULL << cpus) - 1ULL);
    *(uint64_t *)mask_addr = mask;

    /* Linux returns the number of kernel cpumask bytes copied. Musl turns
     * this into API success and zero-fills any larger cpu_set_t tail. */
    return (int64_t)kernel_mask_size;
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

/* exit_group — terminate every thread in the caller's thread group.
 * musl's exit()/return-from-main routes here; all sibling pthreads must die,
 * not just the caller (and the shared CR3 must outlive the call). */
extern void proc_exit_group(int32_t code);
static int64_t sys_exit_group(uint64_t status)
{
    proc_exit_group((int32_t)status);
    return 0; /* unreachable */
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

    uint64_t ns = idt_get_monotonic_ns();

    tp->tv_sec  = (int64_t)(ns / 1000000000ULL);
    tp->tv_nsec = (int64_t)(ns % 1000000000ULL);

    (void)clk_id;  /* Same time for REALTIME and MONOTONIC */
    return 0;
}

/* nanosleep — sleep for specified time */
static int64_t sys_nanosleep(uint64_t req_addr, uint64_t rem_addr)
{
    if (!req_addr) return -EFAULT;
    const timespec_t *req = (const timespec_t *)req_addr;

    int64_t seconds = req->tv_sec;
    int64_t nanoseconds = req->tv_nsec;
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000LL)
        return -EINVAL;
    if ((uint64_t)seconds >
        (~0ULL - (uint64_t)nanoseconds) / 1000000000ULL)
        return -EINVAL;

    uint64_t duration_ns = (uint64_t)seconds * 1000000000ULL +
                           (uint64_t)nanoseconds;
    uint64_t start_ns = idt_get_monotonic_ns();
    while (idt_get_monotonic_ns() - start_ns < duration_ns) {
        /*
         * SYSCALL entry clears IF. The compat32 callback path may mask this
         * CPU's APIC timer, so use the invariant TSC clocksource and avoid HLT.
         */
        __asm__ volatile ("sti; pause; cli" ::: "memory");
    }

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

/* sched_yield — wait for the next timer tick. */
static int64_t sys_sched_yield(void)
{
    /*
     * SYSCALL masks IF; avoid HLT here for the same reason as nanosleep.
     * This is still a cooperative hint, but it cannot wedge the caller if
     * the current CPU's local timer is not delivering interrupts.
     */
    __asm__ volatile ("sti; pause; cli" ::: "memory");
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
    case F_SETFL: fd_table[fd].oflags = (uint32_t)arg; return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        /* Find lowest fd >= arg */
        for (uint64_t i = arg; i < MAX_FDS; i++) {
            if (!fd_table[i].open) {
                fd_table[i] = fd_table[fd];
                if (fd_table[i].type == FD_TYPE_PIPE && fd_table[i].pipe) {
                    pipe_buf_t *p = (pipe_buf_t *)fd_table[i].pipe;
                    if ((fd_table[i].oflags & O_ACCMODE) == O_RDONLY)
                        p->read_refs++;
                    else
                        p->write_refs++;
                } else if (fd_table[i].type == FD_TYPE_FILE &&
                           fd_table[i].node.fs_version == 2) {
                    osfs2_file_retain(fd_table[i].node.data);
                }
                return (int64_t)i;
            }
        }
        return -EMFILE;
    }
    default: return -EINVAL;
    }
}

/* newfstatat / fstatat — stat by path relative to dirfd */

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr,
                               uint64_t statbuf_addr, uint64_t flags)
{
    (void)dirfd; (void)flags;
    const char *path = (const char *)path_addr;
    if (!path || !statbuf_addr) return -EFAULT;

    /* Open, stat, close */
    int64_t fd = sys_open(path_addr, O_RDONLY, 0);
    if (fd < 0) {
        /* The flat FS has no directory entries, so a directory path fails
         * to open. Report it as a directory if it is the prefix of any
         * stored file — lets gcc keep its include dirs (S_ISDIR check). */
        char norm[256];
        const char *flat = normalized_lookup(path, norm, sizeof(norm));
        if (osfs2_path_is_dir(flat)) {
            linux_stat_t *st = (linux_stat_t *)statbuf_addr;
            memset(st, 0, sizeof(*st));
            st->st_mode   = 0040755;  /* S_IFDIR | 0755 */
            st->st_nlink  = 2;
            st->st_blksize = 4096;
            return 0;
        }
        return fd;
    }
    int64_t ret = sys_fstat((uint64_t)fd, statbuf_addr);
    sys_close((uint64_t)fd);
    return ret;
}

/* pread64 — read from fd at offset without changing position */
static int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!buf && count) return -EFAULT;
    if (count == 0) return 0;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -ESPIPE;
    if ((f->oflags & O_ACCMODE) == O_WRONLY) return -EBADF;
    uint64_t size = f->node.fs_version == 2
                  ? osfs2_file_size(f->node.data) : f->node.size;
    f->node.size = size;
    if (offset >= size) return 0;
    if (count > size - offset) count = size - offset;
    int ret = vfs_read(&f->node, offset, (void *)buf, (size_t)count);
    return ret < 0 ? -EIO : ret;
}

/* pwrite64 — write to fd at offset without changing position */
static int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count, uint64_t offset)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!buf && count) return -EFAULT;
    if (count == 0) return 0;
    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_FILE) return -ESPIPE;
    if ((f->oflags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->node.fs_version != 2) return -EROFS;
    if (osfs2_write(f->node.data, offset, (const void *)buf, count) < 0)
        return -EIO;
    f->node.size = osfs2_file_size(f->node.data);
    return (int64_t)count;
}

/* futex — Linux-compatible subset used by musl pthreads */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_WAIT_BITSET 9
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

/* Futex — real wait queue implementation (X-THREAD) */
extern int futex_do_wait(uint64_t uaddr, int expected, uint64_t space,
                         uint64_t timeout_ticks);
extern int futex_do_wake(uint64_t uaddr, uint64_t space, int count);
extern int futex_do_requeue(uint64_t uaddr, uint64_t space, int wake_count,
                            int requeue_count, uint64_t uaddr2,
                            uint64_t space2);
extern int futex_do_cmp_requeue(uint64_t uaddr, uint64_t space, int wake_count,
                                int requeue_count, uint64_t uaddr2,
                                uint64_t space2, int expected);
extern uint64_t proc_current_cr3(void);

/* userspace struct timespec */
typedef struct { int64_t tv_sec; int64_t tv_nsec; } futex_timespec_t;

static bool futex_unsupported_logged;

static void futex_log_unsupported(const char *what, uint64_t op)
{
    if (futex_unsupported_logged) return;
    futex_unsupported_logged = true;
    serial_puts("[FUTEX] unsupported ");
    serial_puts(what);
    serial_puts(" op=");
    serial_puthex(op, 4);
    serial_puts("\n");
}

static uint64_t futex_relative_ticks(const futex_timespec_t *ts)
{
    int64_t sec = ts->tv_sec, nsec = ts->tv_nsec;
    if (sec < 0) sec = 0;
    if (nsec < 0) nsec = 0;
    uint64_t total_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
    uint64_t ticks = total_ns / 10000000ULL;   /* 10 ms per tick */
    return ticks ? ticks : 1;                  /* round any nonzero up */
}

static uint64_t futex_absolute_ticks(const futex_timespec_t *ts)
{
    int64_t sec = ts->tv_sec, nsec = ts->tv_nsec;
    if (sec < 0) sec = 0;
    if (nsec < 0) nsec = 0;
    uint64_t total_ticks = (uint64_t)sec * 100ULL +
                           (uint64_t)nsec / 10000000ULL;
    uint64_t now = idt_get_ticks();
    return (total_ticks > now) ? (total_ticks - now) : 1;
}

static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                          uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
    int cmd = (int)(op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME));

    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET) {
        futex_log_unsupported("realtime", op);
        return -ENOSYS;
    }

    /* PRIVATE futexes are scoped to the calling address space (its CR3 —
     * threads share it). SHARED (no PRIVATE flag) futexes are cross-process,
     * keyed globally with space 0. musl's pthread primitives are PRIVATE. */
    uint64_t space = (op & FUTEX_PRIVATE_FLAG) ? proc_current_cr3() : 0;

    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        if (cmd == FUTEX_WAIT_BITSET && val3 == 0)
            return -EINVAL;
        uint64_t ticks = 0;
        if (timeout) {
            const futex_timespec_t *ts = (const futex_timespec_t *)timeout;
            ticks = (cmd == FUTEX_WAIT_BITSET) ?
                futex_absolute_ticks(ts) : futex_relative_ticks(ts);
        }
        return (int64_t)futex_do_wait(uaddr, (int)val, space, ticks);
    }
    if (cmd == FUTEX_WAKE) {
        return (int64_t)futex_do_wake(uaddr, space, (int)val);
    }
    if (cmd == FUTEX_REQUEUE) {
        if (!uaddr2) return -EINVAL;
        return (int64_t)futex_do_requeue(uaddr, space, (int)val,
                                         (int)timeout, uaddr2, space);
    }
    if (cmd == FUTEX_CMP_REQUEUE) {
        if (!uaddr2) return -EINVAL;
        return (int64_t)futex_do_cmp_requeue(uaddr, space, (int)val,
                                             (int)timeout, uaddr2, space,
                                             (int)val3);
    }
    if (cmd == FUTEX_WAKE_OP) {
        futex_log_unsupported("cmd", op);
        return -ENOSYS;
    }

    futex_log_unsupported("cmd", op);
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
            if (fd_table[i].type == FD_TYPE_PIPE && fd_table[i].pipe) {
                pipe_buf_t *p = (pipe_buf_t *)fd_table[i].pipe;
                if ((fd_table[i].oflags & O_ACCMODE) == O_RDONLY)
                    p->read_refs++;
                else
                    p->write_refs++;
            } else if (fd_table[i].type == FD_TYPE_FILE &&
                       fd_table[i].node.fs_version == 2) {
                osfs2_file_retain(fd_table[i].node.data);
            }
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
extern int32_t proc_fork(uint64_t child_stack);
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
    serial_puts("[CLONE] flags=0x"); serial_puthex(flags, 16);
    serial_puts(child_stack ? " cstk!=0" : " cstk=0");
    serial_puts((flags & CLONE_THREAD) ? " THREAD\n" : " FORK\n");
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

    /* Fork / vfork semantics. Pass child_stack: vfork()/posix_spawn() supply a
     * ready-made child stack (CLONE_VM|CLONE_VFORK) and the child must run on
     * it, not on a copy of the parent's stack. */
    int32_t ret = proc_fork(child_stack);
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

static uint64_t bounded_name_len(const char *name, uint64_t limit)
{
    uint64_t len = 0;
    while (len < limit && name[len]) len++;
    return len;
}

/* Return the immediate child of `dir` represented by a flat OsitoFS key.
 * Invalid legacy 64-byte unterminated keys are intentionally omitted. */
static bool osfs2_dir_child(const char *dir, const char *name,
                            char child[64], uint8_t *type)
{
    uint64_t name_len = bounded_name_len(name, 64);
    if (name_len == 64) return false;

    uint64_t dir_len = strlen(dir);
    uint64_t start = 0;
    if (dir_len) {
        if (name_len <= dir_len || memcmp(name, dir, dir_len) != 0 ||
            name[dir_len] != '/')
            return false;
        start = dir_len + 1;
    }
    if (start >= name_len) return false;

    uint64_t end = start;
    while (end < name_len && name[end] != '/') end++;
    uint64_t child_len = end - start;
    if (!child_len || child_len >= 64) return false;
    memcpy(child, name + start, child_len);
    child[child_len] = '\0';
    *type = end < name_len ? DT_DIR : DT_REG;
    return true;
}

static bool osfs2_child_seen(const char *dir, const char *child,
                             uint32_t before)
{
    for (uint32_t i = 0; i < before; i++) {
        void *file = osfs2_file_at(i);
        const char *name = file ? osfs2_file_name(file) : NULL;
        char previous[64];
        uint8_t type;
        if (name && osfs2_dir_child(dir, name, previous, &type) &&
            strcmp(previous, child) == 0)
            return true;
    }
    return false;
}

static int append_dirent(uint8_t *buf, uint64_t count, uint64_t *pos,
                         uint64_t ino, uint64_t next, uint8_t type,
                         const char *name)
{
    uint64_t namelen = strlen(name);
    uint64_t reclen = (19 + namelen + 1 + 7) & ~7ULL;
    if (*pos + reclen > count) return 0;

    linux_dirent64_t *entry = (linux_dirent64_t *)(buf + *pos);
    memset(entry, 0, reclen);
    entry->d_ino = ino;
    entry->d_off = (int64_t)next;
    entry->d_reclen = (uint16_t)reclen;
    entry->d_type = type;
    memcpy(entry->d_name, name, namelen + 1);
    *pos += reclen;
    return 1;
}

static int64_t sys_getdents64(uint64_t fd, uint64_t dirp_addr, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!dirp_addr) return -EFAULT;

    fd_entry_t *f = &fd_table[fd];
    if (f->type != FD_TYPE_DIR) return -ENOTDIR;

    uint8_t *buf = (uint8_t *)dirp_addr;
    uint64_t pos = 0;

    static const char *dots[] = { ".", ".." };
    for (uint64_t i = f->offset; i < 2; i++) {
        if (!append_dirent(buf, count, &pos, i + 1, i + 1,
                           DT_DIR, dots[i]))
            return pos ? (int64_t)pos : -EINVAL;
        f->offset = i + 1;
    }

    uint32_t files = osfs2_file_count();
    uint32_t start = f->offset > 2 ? (uint32_t)(f->offset - 2) : 0;
    for (uint32_t i = start; i < files; i++) {
        void *file = osfs2_file_at(i);
        const char *name = file ? osfs2_file_name(file) : NULL;
        char child[64];
        uint8_t type;
        if (!name || !osfs2_dir_child(f->dir_path, name, child, &type) ||
            osfs2_child_seen(f->dir_path, child, i)) {
            f->offset = (uint64_t)i + 3;
            continue;
        }
        if (!append_dirent(buf, count, &pos, i + 3, (uint64_t)i + 3,
                           type, child))
            return pos ? (int64_t)pos : -EINVAL;
        f->offset = (uint64_t)i + 3;
    }
    return (int64_t)pos;
}

/* ── Trivial POSIX stubs for busybox/musl compatibility ──────── */

extern uint64_t idt_get_ticks(void);

static uint64_t current_umask = 022;

static int64_t sys_pause(void)
{
    /* Sleep until signal — just do a short sleep and return -EINTR */
    __asm__ volatile ("sti; hlt");
    return -4;  /* EINTR */
}

static int64_t sys_chdir(uint64_t path_addr)
{
    (void)path_addr;
    /* Single flat filesystem — chdir to "/" always succeeds, anything else ENOENT */
    const char *p = (const char *)path_addr;
    if (p && p[0] == '/' && p[1] == '\0') return 0;
    return -ENOENT;
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
        uint64_t ns = idt_get_monotonic_ns();
        uint64_t *tv = (uint64_t *)tv_addr;
        tv[0] = ns / 1000000000ULL;          /* tv_sec */
        tv[1] = (ns % 1000000000ULL) / 1000; /* tv_usec */
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
    uint32_t block_size = osfs2_get_block_size();
    st->f_bsize = block_size ? block_size : 4096;
    st->f_blocks = block_size ? osfs2_total_blocks() : mem_get_total() / 4096;
    st->f_bfree = block_size ? osfs2_free_blocks() : mem_get_free() / 4096;
    st->f_bavail = st->f_bfree;
    st->f_files = block_size ? osfs2_max_files() : 0;
    uint64_t used_files = block_size ? osfs2_file_count() : 0;
    st->f_ffree = used_files < st->f_files ? st->f_files - used_files : 0;
    st->f_namelen = 63;
    st->f_frsize = st->f_bsize;
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

/* ── Batched syscall: execute multiple operations in one trap ── */

typedef struct {
    uint64_t nr;         /* syscall number */
    uint64_t args[6];    /* arguments */
    int64_t  result;     /* filled by kernel */
    uint32_t flags;      /* BATCH_STOP_ON_ERROR, etc */
    uint32_t _pad;
} batch_entry_t;

#define BATCH_STOP_ON_ERROR    (1 << 0)
#define BATCH_USE_PREV_RESULT  0xFFFFFFFFFFFFFFFFULL  /* magic arg value */
#define BATCH_MAX_ENTRIES      32

/* Forward decl — defined immediately below */
int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5,
                         uint64_t a6);

static int64_t sys_batch(uint64_t entries_addr, uint64_t count)
{
    if (count == 0 || count > BATCH_MAX_ENTRIES) return -EINVAL;
    batch_entry_t *entries = (batch_entry_t *)entries_addr;
    int64_t prev_result = 0;

    for (uint64_t i = 0; i < count; i++) {
        batch_entry_t *e = &entries[i];

        /* Substitute BATCH_USE_PREV_RESULT magic in args */
        uint64_t a[6];
        for (int j = 0; j < 6; j++)
            a[j] = (e->args[j] == BATCH_USE_PREV_RESULT) ? (uint64_t)prev_result : e->args[j];

        e->result = syscall_dispatch(e->nr, a[0], a[1], a[2], a[3], a[4], a[5]);
        prev_result = e->result;

        if ((e->flags & BATCH_STOP_ON_ERROR) && e->result < 0)
            return -(int64_t)(i + 1);  /* negative = 1-based failed entry index */
    }
    return (int64_t)count;
}

/* ── Syscall-free command ring for trusted processes ──────────
 *
 * A process allocates a cmd_ring_t in its address space and registers
 * it via SYS_CMDRING_INIT. Instead of trapping into the kernel per
 * I/O operation, it writes cmd_entry_t records to the ring and the
 * kernel drains them during the process's quantum (called from
 * sched_tick context where current_proc is already correct, so
 * fd_table resolves to this process's fds). Zero SYSCALL overhead.
 *
 * Userspace fallback: if the ring isn't set up, regular SYSCALL works.
 * ────────────────────────────────────────────────────────────────── */

#define CMDRING_SIZE     64
#define CMD_FREE         0
#define CMD_PENDING      1
#define CMD_DONE         2

#define CMD_WRITE        1
#define CMD_READ         2
#define CMD_CLOSE        3
#define CMD_LSEEK        4

typedef struct {
    uint32_t opcode;
    uint32_t flags;
    int32_t  fd;
    uint32_t _pad;
    uint64_t addr;       /* buffer address (in shared address space) */
    uint64_t len;
    int64_t  result;     /* written by kernel when CMD_DONE */
    volatile uint32_t state;  /* CMD_FREE / CMD_PENDING / CMD_DONE */
    uint32_t _pad2;
} cmd_entry_t;

typedef struct {
    volatile uint32_t head;    /* process writes here (mod CMDRING_SIZE) */
    volatile uint32_t tail;    /* kernel reads here */
    uint32_t          magic;   /* 0x434D4452 = "CMDR" */
    uint32_t          _pad;
    cmd_entry_t       entries[CMDRING_SIZE];
} cmd_ring_t;

#define CMDRING_MAGIC 0x434D4452

/* Per-process ring pointer — stored alongside process_t.
 * For now, use a simple global indexed by proctab slot.
 * Only one process at a time can have an active ring. */
#define MAX_CMD_RINGS 64
static cmd_ring_t *cmd_rings[MAX_CMD_RINGS];

/* Drain pending commands from a process's ring.
 * Called from sched_tick context (current_proc valid, interrupts off). */
void cmdring_drain(int proc_idx)
{
    if (proc_idx < 0 || proc_idx >= MAX_CMD_RINGS) return;
    cmd_ring_t *ring = cmd_rings[proc_idx];
    if (!ring || ring->magic != CMDRING_MAGIC) return;

    /* Process up to 8 commands per tick to avoid hogging the ISR */
    int drained = 0;
    while (ring->tail != ring->head && drained < 8) {
        uint32_t idx = ring->tail % CMDRING_SIZE;
        cmd_entry_t *e = &ring->entries[idx];

        if (e->state != CMD_PENDING) break;

        switch (e->opcode) {
        case CMD_WRITE:
            e->result = sys_write((uint64_t)e->fd, e->addr, e->len);
            break;
        case CMD_READ:
            e->result = sys_read((uint64_t)e->fd, e->addr, e->len);
            break;
        case CMD_CLOSE:
            e->result = sys_close((uint64_t)e->fd);
            break;
        case CMD_LSEEK:
            e->result = sys_lseek((uint64_t)e->fd, e->addr, e->len);
            break;
        default:
            e->result = -ENOSYS;
            break;
        }

        __asm__ volatile ("mfence" ::: "memory");
        e->state = CMD_DONE;
        ring->tail++;
        drained++;
    }
}

static int64_t sys_cmdring_init(uint64_t ring_addr)
{
    if (!ring_addr) return -EINVAL;

    cmd_ring_t *ring = (cmd_ring_t *)ring_addr;
    ring->magic = CMDRING_MAGIC;
    ring->head = 0;
    ring->tail = 0;
    memset(ring->entries, 0, sizeof(ring->entries));

    /* Register for current process via PID-indexed slot */
    extern int32_t proc_current_pid(void);
    int32_t pid = proc_current_pid();
    if (pid <= 0 || pid >= MAX_CMD_RINGS) return -EINVAL;
    cmd_rings[pid] = ring;

    return (int64_t)(uint64_t)ring;  /* return ring address as confirmation */
}

/* ── VDSO — kernel-maintained shared data page ─────────────────
 *
 * One physical page, mapped read-only into every process at
 * VDSO_USER_VA.  Kernel updates it on every timer tick.  Processes
 * read time/system info directly — zero syscalls.
 *
 * Seqlock: odd seq = update in progress.  Reader retries if seq
 * changed or is odd. */

#define VDSO_USER_VA  0x7FFFE000ULL

typedef struct __attribute__((aligned(4096))) {
    volatile uint32_t seq;
    uint32_t _pad0;
    uint64_t monotonic_ns;
    uint64_t tsc_at_update;
    uint64_t tsc_per_sec;
    int64_t  unix_timestamp;
    uint64_t boot_ticks;
    uint32_t cpu_count;
    uint32_t page_size;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t random_seed;
    uint8_t  _reserved[4096 - 80];
} vdso_data_t;

static vdso_data_t *vdso_page;
static uint64_t     vdso_phys;

static inline uint64_t vdso_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void vdso_init(void)
{
    void *page = mem_alloc_pages(1);
    if (!page) { serial_puts("[VDSO] alloc failed\n"); return; }
    vdso_phys = (uint64_t)page;
    vdso_page = (vdso_data_t *)PHYS_TO_VIRT(page);
    memset(vdso_page, 0, 4096);

    extern uint32_t smp_cpu_count(void);
    extern uint64_t mem_get_total(void);

    vdso_page->cpu_count    = smp_cpu_count();
    vdso_page->page_size    = 4096;
    vdso_page->total_memory = mem_get_total();

    /* Reuse the PIT-calibrated kernel clocksource. */
    vdso_page->tsc_per_sec = idt_get_tsc_freq();
    vdso_page->seq = 0;

    serial_puts("[VDSO] Initialized, TSC ");
    serial_putdec(vdso_page->tsc_per_sec / 1000000);
    serial_puts(" MHz\n");
}

void vdso_update(void)
{
    if (!vdso_page) return;

    uint32_t s = vdso_page->seq;
    vdso_page->seq = s + 1;  /* odd = updating */
    __asm__ volatile ("" ::: "memory");

    vdso_page->boot_ticks   = idt_get_ticks();
    vdso_page->monotonic_ns = idt_get_monotonic_ns();
    vdso_page->tsc_at_update = vdso_rdtsc();

    extern uint32_t ntp_get_utc(void);
    extern bool     ntp_is_synced(void);
    if (ntp_is_synced())
        vdso_page->unix_timestamp = (int64_t)ntp_get_utc();

    vdso_page->free_memory  = mem_get_free();
    vdso_page->random_seed ^= vdso_page->tsc_at_update;

    __asm__ volatile ("" ::: "memory");
    vdso_page->seq = s + 2;  /* even = consistent */
}

int vdso_map_process(uint64_t cr3)
{
    if (!vdso_phys) return -1;
    extern int paging_map_page_in_cr3(uint64_t cr3, uint64_t virt,
                                      uint64_t phys, uint64_t flags);
    #define PTE_PRESENT_LOCAL  (1ULL << 0)
    #define PTE_USER_LOCAL     (1ULL << 2)
    return paging_map_page_in_cr3(cr3, VDSO_USER_VA, vdso_phys,
                                  PTE_PRESENT_LOCAL | PTE_USER_LOCAL);
}

/* ── Syscall memoization with buffer replay ─────────────────── */

#define MEMO_SLOTS     32
#define MEMO_SLOTS_MASK (MEMO_SLOTS - 1)
#define MEMO_BUF_SIZE  400   /* utsname = 390 bytes, largest memoized */

typedef struct {
    uint64_t nr;
    uint64_t args_hash;
    int64_t  result;
    uint8_t  buf[MEMO_BUF_SIZE];
    uint32_t buf_len;          /* bytes to replay (0 = return-only) */
    uint64_t buf_dst_arg;      /* which syscall arg holds the dest pointer */
    uint64_t valid_until;      /* tick expiry, 0 = never */
    bool     active;
} memo_entry_t;

static memo_entry_t memo_cache[MEMO_SLOTS];

static uint64_t memo_hash(uint64_t nr, uint64_t a1, uint64_t a2)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    h ^= nr;   h *= 0x100000001b3ULL;
    h ^= a1;   h *= 0x100000001b3ULL;
    h ^= a2;   h *= 0x100000001b3ULL;
    return h;
}

static bool memo_lookup(uint64_t nr, uint64_t hash, uint64_t dst_addr,
                        int64_t *result)
{
    uint32_t slot = (uint32_t)(hash & MEMO_SLOTS_MASK);
    memo_entry_t *e = &memo_cache[slot];
    if (!e->active || e->nr != nr || e->args_hash != hash) return false;
    if (e->valid_until && idt_get_ticks() > e->valid_until) {
        e->active = false;
        return false;
    }
    /* Replay buffer write to user destination */
    if (e->buf_len > 0 && dst_addr)
        memcpy((void *)dst_addr, e->buf, e->buf_len);
    *result = e->result;
    return true;
}

static void memo_store(uint64_t nr, uint64_t hash, int64_t result,
                       const void *buf_src, uint32_t buf_len, uint64_t ttl)
{
    uint32_t slot = (uint32_t)(hash & MEMO_SLOTS_MASK);
    memo_entry_t *e = &memo_cache[slot];
    e->nr = nr;
    e->args_hash = hash;
    e->result = result;
    if (buf_len > 0 && buf_len <= MEMO_BUF_SIZE && buf_src)
        memcpy(e->buf, buf_src, buf_len);
    e->buf_len = (buf_len <= MEMO_BUF_SIZE) ? buf_len : 0;
    e->valid_until = ttl ? (idt_get_ticks() + ttl) : 0;
    e->active = true;
}

/* Invalidate all entries for a given syscall number */
static void memo_invalidate_nr(uint64_t nr)
{
    for (int i = 0; i < MEMO_SLOTS; i++)
        if (memo_cache[i].active && memo_cache[i].nr == nr)
            memo_cache[i].active = false;
}

typedef struct {
    uint64_t dr0;
    uint64_t dr6;
    uint64_t dr7;
    uint64_t cr3;
    uint64_t phys;
} debug_watch_state_t;

static int64_t sys_debug_watch(uint64_t operation, uint64_t address,
                               uint64_t state_addr)
{
    uint64_t cr3 = proc_current_cr3();
    uint64_t phys = paging_translate_in_cr3(cr3, address);
    extern volatile uint64_t syscall_user_rsp;
    uint64_t rsp_phys = paging_translate_in_cr3(cr3, syscall_user_rsp);

    if (operation == 1) {
        if ((address & 3) || phys == UINT64_MAX) return -EINVAL;
        if (hwbp_set(0, address, HWBP_WRITE, HWBP_LEN_4, "gtavbuf") < 0)
            return -EINVAL;
        extern void nvme_debug_watch_set(uint64_t phys, uint64_t virt);
        nvme_debug_watch_set(phys, address);
        extern void mem_debug_dump_page(uint64_t phys);
        mem_debug_dump_page(phys);
        hwbp_set(1, (uint64_t)PHYS_TO_VIRT(phys), HWBP_WRITE,
                 HWBP_LEN_4, "gtavphys");
        serial_puts("[GTAV-WATCH] arm pid=");
        serial_putdec((uint64_t)proc_current_pid());
        serial_puts(" va=0x");
        serial_puthex(address, 16);
        serial_puts(" phys=0x");
        serial_puthex(phys, 16);
        serial_puts(" user-rsp=0x");
        serial_puthex(syscall_user_rsp, 16);
        serial_puts(" rsp-phys=0x");
        serial_puthex(rsp_phys, 16);
        serial_puts(" alias=");
        serial_putdec(phys != UINT64_MAX && rsp_phys != UINT64_MAX &&
                      ((phys ^ rsp_phys) < 4096));
        serial_puts("\n");

        extern uint32_t proc_pid_of(void *p);
        extern uint32_t proc_state_of(void *p);
        for (int i = 0; i < MAX_VMAS; i++) {
            if (!vma_table[i].in_use) continue;
            if (vma_table[i].type != VMA_SHARED_STACK &&
                vma_table[i].type != VMA_THREAD_STACK &&
                vma_table[i].type != VMA_DEFERRED_STACK &&
                vma_table[i].type != VMA_DEFERRED_SHARED_STACK)
                continue;
            uint64_t stack_phys = vma_table[i].type == VMA_SHARED_STACK ||
                                  vma_table[i].type == VMA_DEFERRED_SHARED_STACK
                ? VIRT_TO_PHYS(vma_table[i].base)
                : paging_translate_in_cr3(cr3, vma_table[i].base);
            if (stack_phys == UINT64_MAX || phys == UINT64_MAX ||
                (phys & ~0xFFFULL) < (stack_phys & ~0xFFFULL) ||
                (phys & ~0xFFFULL) >= (stack_phys & ~0xFFFULL) +
                                      vma_table[i].pages * 4096)
                continue;
            serial_puts("[GTAV-WATCH] stack owner=");
            serial_putdec(proc_pid_of(vma_table[i].owner));
            serial_puts(" state=");
            serial_putdec(proc_state_of(vma_table[i].owner));
            serial_puts(" base=0x");
            serial_puthex(vma_table[i].base, 16);
            serial_puts(" phys=0x");
            serial_puthex(stack_phys, 16);
            serial_puts(" pages=");
            serial_putdec(vma_table[i].pages);
            serial_puts(" type=");
            serial_putdec(vma_table[i].type);
            serial_puts("\n");
        }
    } else if (operation != 0) {
        return -EINVAL;
    }

    if (state_addr) {
        debug_watch_state_t *state = (debug_watch_state_t *)state_addr;
        __asm__ volatile ("mov %%dr0, %0" : "=r"(state->dr0));
        __asm__ volatile ("mov %%dr6, %0" : "=r"(state->dr6));
        __asm__ volatile ("mov %%dr7, %0" : "=r"(state->dr7));
        state->cr3 = cr3;
        state->phys = phys;
    }
    return phys == UINT64_MAX ? -EFAULT : (int64_t)phys;
}

/* ── Syscall dispatch (called from assembly) ─────────────────── */

static int64_t __hot syscall_dispatch_inner(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5,
                         uint64_t a6);

/* FS-base (TLS) transparency wrapper.
 *
 * A syscall that yields mid-flight (sys_inference's sched_yield, or any
 * blocking path that re-schedules) resumes the caller on a context-switch
 * RESTORE that reloads the caller's *stored* per-process fs_base. If a
 * transient current_proc/sched desync ever left that stored copy at 0 (its
 * arch_prctl write landed on the wrong process_t — the live MSR was still set
 * correctly by arch_prctl's own wrmsr, but the process_t copy was not), the
 * caller comes back from the syscall with FS=0 and the next fs:[0] read
 * (errno / __pthread_self) faults at CR2=0.
 *
 * Make every syscall FS-transparent instead of trusting current_proc: snapshot
 * the caller's LIVE FS base on entry into this frame's local (which travels
 * with the process's kernel stack across any yield) and re-assert it on
 * return. This is immune to the desync because it reads/writes the hardware
 * MSR for *this* call, never a process_t field. arch_prctl is the one syscall
 * that legitimately changes FS, so skip the re-assert for it. */
int64_t __hot syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5,
                               uint64_t a6)
{
    uint64_t entry_fs = rdmsr(MSR_FS_BASE);
    int64_t  ret = syscall_dispatch_inner(nr, a1, a2, a3, a4, a5, a6);
    if (nr != SYS_ARCH_PRCTL && entry_fs && rdmsr(MSR_FS_BASE) != entry_fs)
        wrmsr(MSR_FS_BASE, entry_fs);
    return ret;
}

static int64_t __hot syscall_dispatch_inner(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5,
                         uint64_t a6)
{
    /* Memoization: check cache for known-memoizable syscalls.
     * Buffer-writing syscalls replay via memcpy on cache hit. */
    if (nr == SYS_UNAME || nr == SYS_GETCWD) {
        uint64_t hash = memo_hash(nr, a1, a2);
        /* For uname/getcwd: buf is a1. For stat/fstat: buf is a2. */
        uint64_t dst = (nr == SYS_UNAME || nr == SYS_GETCWD) ? a1 : a2;
        int64_t cached;
        if (memo_lookup(nr, hash, dst, &cached))
            return cached;
    }

    switch (nr) {
    case SYS_READ:       return sys_read(a1, a2, a3);
    case SYS_WRITE: {
        int64_t r = sys_write(a1, a2, a3);
        /* Invalidate fstat cache for this fd on write */
        if (r >= 0) memo_invalidate_nr(SYS_FSTAT);
        return r;
    }
    case SYS_OPEN:       return sys_open(a1, a2, a3);
    case SYS_CLOSE:      return sys_close(a1);
    case SYS_FSTAT:      return sys_fstat(a1, a2);
    case SYS_POLL:       return sys_poll(a1, a2, a3);
    case SYS_LSEEK:      return sys_lseek(a1, (int64_t)a2, a3);
    case SYS_MMAP:       return sys_mmap(a1, a2, a3, a4, a5, a6);
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
    case SYS_SCHED_SETPARAM: return 0;  /* keep default SCHED_OTHER */
    case SYS_SCHED_GETPARAM:
        if (a2) *(int32_t *)a2 = 0;     /* struct sched_param.sched_priority */
        return 0;
    case SYS_SCHED_SETSCHEDULER:
        return 0;                       /* accept as no-op */
    case SYS_SCHED_GETSCHEDULER:
        return 0;                       /* SCHED_OTHER */
    case SYS_SCHED_GET_PRIORITY_MAX:
        return (a1 == 1 || a1 == 2) ? 99 : 0; /* FIFO/RR vs normal */
    case SYS_SCHED_GET_PRIORITY_MIN:
        return (a1 == 1 || a1 == 2) ? 1 : 0;
    case SYS_SCHED_RR_GET_INTERVAL:
        if (a2) {
            int64_t *ts = (int64_t *)a2;
            ts[0] = 0;
            ts[1] = 10000000;           /* 10 ms scheduler tick */
        }
        return 0;
    case SYS_PIPE:       return sys_pipe(a1);
    case SYS_DUP:        return sys_dup(a1);
    case SYS_DUP2:       return sys_dup2(a1, a2);
    case SYS_NANOSLEEP:  return sys_nanosleep(a1, a2);
    case SYS_GETPID:     return sys_getpid();
    case SYS_MADVISE:    return 0;  /* ignore hints */
    case SYS_STAT:       return sys_stat(a1, a2);
    case SYS_LSTAT:      return sys_stat(a1, a2);  /* no symlinks */
    case SYS_SENDFILE:   return sys_sendfile(a1, a2, a3, a4);
    case SYS_SELECT:     return sys_select(a1, a2, a3, a4, a5);
    case SYS_SOCKET:     return sys_socket(a1, a2, a3);
    case SYS_CONNECT:    return sys_connect(a1, a2, a3);
    case SYS_ACCEPT:     return sys_accept(a1, a2, a3);
    case SYS_SENDTO:     return sys_sendto(a1, a2, a3, a4, a5, a6);
    case SYS_RECVFROM:   return sys_recvfrom(a1, a2, a3, a4, a5, a6);
    case SYS_SHUTDOWN: {
        int idx = socket_index_from_fd(a1);
        return idx < 0 ? idx : 0;
    }
    case SYS_BIND:       return sys_bind(a1, a2, a3);
    case SYS_LISTEN:     return sys_listen(a1, a2);
    case SYS_GETSOCKNAME: return sys_getsockname(a1, a2, a3);
    case SYS_GETPEERNAME: return sys_getpeername(a1, a2, a3);
    case SYS_SETSOCKOPT: return sys_setsockopt(a1, a2, a3, a4, a5);
    case SYS_GETSOCKOPT: return sys_getsockopt(a1, a2, a3, a4, a5);
    case SYS_CLONE:      return sys_clone(a1, a2, a3, a4, a5);
    case SYS_FORK:       return sys_clone(17 /* SIGCHLD */, 0, 0, 0, 0);
    case SYS_VFORK:      return sys_clone(17 /* SIGCHLD */, 0, 0, 0, 0);
    case SYS_EXECVE:     return sys_execve(a1, a2, a3);
    case SYS_EXIT:       return sys_exit(a1);
    case SYS_WAIT4:      return sys_wait4(a1, a2, a3, a4);
    case SYS_KILL:       return sys_kill(a1, a2);
    case SYS_UNAME: {
        int64_t r = sys_uname(a1);
        if (r == 0) memo_store(nr, memo_hash(nr, a1, 0), r, (void *)a1, 390, 0);
        return r;
    }
    case SYS_FCNTL:      return sys_fcntl(a1, a2, a3);
    case SYS_FSYNC:      return sys_fsync(a1);
    case SYS_GETCWD: {
        int64_t r = sys_getcwd(a1, a2);
        if (r > 0) memo_store(nr, memo_hash(nr, a1, a2), r, (void *)a1, 2, 0);
        return r;
    }
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
    case SYS_SETITIMER:  return 0;  /* stub: pretend timer set */
    case SYS_SIGALTSTACK: return sys_sigaltstack(a1, a2);
    case SYS_PRCTL:      return sys_prctl(a1, a2, a3, a4, a5);
    case SYS_ARCH_PRCTL: return sys_arch_prctl(a1, a2);
    case SYS_GETTID:     return sys_gettid();
    case SYS_FUTEX:      return sys_futex(a1, a2, a3, a4, a5, a6);
    case SYS_SCHED_GETAFFINITY: return sys_sched_getaffinity(a1, a2, a3);
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
    case SYS_RENAME:     return sys_rename(a1, a2);
    case SYS_MKDIR:      return -ENOSYS;  /* no directories */
    case SYS_RMDIR:      return -ENOSYS;
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
    case SYS_GETRLIMIT:  return sys_prlimit64(0, a1, 0, a2);
    case SYS_GETRUSAGE: {
        /* getrusage(who, usage) — zero-fill struct rusage (144 bytes on x86-64) */
        if (a2) memset((void *)a2, 0, 144);
        return 0;
    }
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
    case SYS_SYNC:       disk_flush(); return 0;
    case SYS_TRUNCATE:   return -ENOSYS;
    case SYS_FTRUNCATE:  return sys_ftruncate(a1, a2);
    case SYS_WAITID:     return sys_wait4(-1, a3, (uint64_t)(int)a4, 0);
    case SYS_UNLINKAT:   return sys_unlink(a2);  /* ignore dirfd */
    case SYS_RENAMEAT:   return sys_renameat(a1, a2, a3, a4, 0);
    case SYS_MKDIRAT:    return -ENOSYS;
    case SYS_FCHOWNAT:   return 0;
    case SYS_FCHMODAT:   return 0;
    case SYS_FACCESSAT:  return sys_access(a2, a3);  /* ignore dirfd */
    case SYS_FACCESSAT2: return sys_access(a2, a3);
    case SYS_PSELECT6:   return sys_poll(0, 0, 0);
    case SYS_UTIMENSAT:  return 0;   /* pretend success */
    case SYS_RENAMEAT2:  return sys_renameat(a1, a2, a3, a4, a5);
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

    /* ── OsitoK private: batched syscalls + command ring ─────── */
    case SYS_BATCH:
        return sys_batch(a1, a2);
    case SYS_CMDRING_INIT:
        return sys_cmdring_init(a1);
    case SYS_BOOT_DIAG_MARK: {
        char reason[80];
        const char *src = a1 ? (const char *)a1 : "user";
        uint32_t i = 0;
        if (!boot_diag_mark)
            return -ENOSYS;
        while (i + 1 < sizeof(reason) && src[i]) {
            reason[i] = src[i];
            i++;
        }
        reason[i] = '\0';
        boot_diag_mark(reason);
        return 0;
    }
    case SYS_DEBUG_WATCH:
        return sys_debug_watch(a1, a2, a3);

    /* ── OsitoK private: inference as a kernel syscall (530-534).
     * Numbers chosen to sit above SYS_BATCH (520) / SYS_CMDRING_INIT (521)
     * and clear of the SYS_SHM_* block at 500-506. */
    case 530: {
        extern int64_t sys_inference(uint32_t *, uint32_t, uint32_t *,
                                      uint32_t, uint32_t);
        return sys_inference((uint32_t *)a1, (uint32_t)a2,
                             (uint32_t *)a3, (uint32_t)a4, (uint32_t)a5);
    }
    case 531: {
        extern int64_t sys_inference_reset(void);
        return sys_inference_reset();
    }
    case 532: {
        extern int64_t sys_inference_state(void *, uint64_t);
        return sys_inference_state((void *)a1, a2);
    }
    case 533: {
        extern int64_t sys_inference_tokenize(const char *, uint32_t *, uint32_t);
        return sys_inference_tokenize((const char *)a1, (uint32_t *)a2, (uint32_t)a3);
    }
    case 534: {
        extern int64_t sys_inference_detokenize(const uint32_t *, uint32_t,
                                                 char *, uint64_t);
        return sys_inference_detokenize((const uint32_t *)a1, (uint32_t)a2,
                                         (char *)a3, a4);
    }

    /* -- Display modes mini-KMS syscalls (620..623) -- */
    case SYS_DISPLAY_GET_MODE_COUNT:
        return display_get_mode_count ? (int64_t)display_get_mode_count() : -ENOSYS;
    case SYS_DISPLAY_GET_MODE:
        if (!a2) return -EINVAL;
        return display_modeset_get_mode
            ? (int64_t)display_modeset_get_mode((uint32_t)a1,
                                                (display_mode_info_t *)a2)
            : -ENOSYS;
    case SYS_DISPLAY_GET_CURRENT_MODE:
        if (!a1) return -EINVAL;
        return display_modeset_get_current
            ? (int64_t)display_modeset_get_current((display_mode_info_t *)a1)
            : -ENOSYS;
    case SYS_DISPLAY_SET_MODE:
        return display_modeset_set
            ? (int64_t)display_modeset_set((uint32_t)a1, (uint32_t)a2,
                                           (uint32_t)a3, (uint32_t)a4)
            : -ENOSYS;

    /* -- Vulkan Phase 1 / Wave 1: GPU 3D syscalls (600..607) -- */
    case SYS_GPU_CAPS: {         /* 600: caps(*out) */
        uint32_t caps = 0;
        if (vg3d_caps() & GPU_CAP_VENUS_READY) caps |= GPU_CAP_VENUS_READY;
        if (a1) *(uint32_t *)a1 = caps;
        return caps;
    }
    case SYS_GPU_CTX_CREATE: {   /* 601: gpu_ctx_create(flags) */
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;  /* EPERM */
        if ((uint32_t)a1 != GPU_CTX_VENUS)
            return -ENOSYS;
        return vg3d_ctx_create((uint32_t)pid, (uint32_t)a1);
    }
    case SYS_GPU_CTX_DESTROY: {  /* 602: gpu_ctx_destroy(ctx_id) */
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;
        return vg3d_ctx_destroy((uint32_t)pid, (uint32_t)a1);
    }
    case SYS_GPU_RES_CREATE: {   /* 603: (ctx_id, args *) */
        if (!a2) return -22;
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;
        const struct gpu_res_create_args *a =
            (const struct gpu_res_create_args *)a2;
        return vg3d_res_create((uint32_t)pid, (uint32_t)a1, a);
    }
    case SYS_GPU_RES_MAP: {      /* 604: (res_id) -> user VA */
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;
        return (int64_t)vg3d_res_map((uint32_t)pid, (uint32_t)a1);
    }
    case SYS_GPU_SUBMIT: {       /* 605: (args *) */
        const struct gpu_submit_args *a =
            (const struct gpu_submit_args *)a1;
        if (!a) return -22; /* EINVAL */
        if (!a->cmd_bytes || !a->out_fence) return -22;
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;
        int32_t result = vg3d_submit((uint32_t)pid, a->ctx_id,
                                     a->cmd_bytes, a->cmd_len, a->out_fence);
        if (result < 0) {
            serial_puts("[VG3D] submit failed tgid=");
            serial_putdec((uint32_t)pid);
            serial_puts(" ctx=");
            serial_putdec(a->ctx_id);
            serial_puts(" rc=0x");
            serial_puthex((uint64_t)(int64_t)result, 16);
            serial_puts("\n");
        }
        return result;
    }
    case SYS_GPU_FENCE_WAIT: {   /* 606: (fence, timeout_ns) */
        return vg3d_fence_wait((uint64_t)a1, (uint64_t)a2);
    }
    case SYS_GPU_PRESENT: {      /* 607: (args *) */
        const struct gpu_present_args *a =
            (const struct gpu_present_args *)a1;
        if (!a) return -22;
        int32_t pid = gpu_current_owner_pid();
        if (pid < 0) return -1;
        return vg3d_present((uint32_t)pid,
                            a->ctx_id, a->res_id, a->shm_handle);
    }
    case SYS_GPU_RES_DESTROY: {  /* 608: (res_id) */
        int32_t pid = proc_current_tgid();
        if (pid < 0) return -1;
        return vg3d_res_destroy((uint32_t)pid, (uint32_t)a1);
    }

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

enum {
    SYSCALL_KERNEL_CS = 0x90,
    SYSCALL_KERNEL_SS = 0x98,
    SYSRET_USER_BASE  = 0x33
};

/* ── Save/restore brk state for fork+execve ──────────────────── */
/*
 * In an identity-mapped OS, the brk globals are shared between parent
 * and child. When a forked child calls execve, syscall_reset_process
 * frees the brk heap — destroying the parent's malloc state. We save
 * the parent's brk pointers before the child's execve and restore them
 * after the child is reaped in proc_wait4.
 *
 * NOTE: fd_table is now per-process (lives inline in process_t), so it
 * no longer needs save/restore — the parent's fds are naturally untouched
 * when the child runs execve. Same for vmas (owner-filtered cleanup).
 */

/* Saved parent state — globals that syscall_reset_process would destroy
 * when the forked child calls execve. */
static struct {
    uint64_t  fs_base;
    uint64_t  sigs[NSIG];
    uint32_t  sig_pend;
    vma_t     vmas[MAX_VMAS];
    bool      valid;
} saved_parent;

void syscall_save_brk(void)
{
    saved_parent.fs_base     = rdmsr(MSR_FS_BASE);
    memcpy(saved_parent.sigs, sig_handlers, sizeof(sig_handlers));
    saved_parent.sig_pend = sig_pending;
    memcpy(saved_parent.vmas, vma_table,    sizeof(vma_table));
    saved_parent.valid = true;
}

/* True between syscall_save_brk and syscall_restore_brk: a forked child
 * is about to run (or has just run) execve. Used by the ELF loader to
 * refuse the demand-paged path during fork+execve — demand paging shares
 * the parent's PML4 and would corrupt the parent's RW data when the
 * child page-faults rewrite the shared PTEs. */
bool syscall_in_fork_exec(void)
{
    return saved_parent.valid;
}

void syscall_restore_brk(void)
{
    if (!saved_parent.valid) return;

    /* Restore FS_BASE (TLS segment register) — the child's musl init
     * overwrites this via arch_prctl(ARCH_SET_FS). Without restoring,
     * the parent reads the child's TLS area (errno, malloc context). */
    if (saved_parent.fs_base)
        wrmsr(MSR_FS_BASE, saved_parent.fs_base);

    /* Restore signal handlers, VMA table. fd_table is per-process so no
     * restore needed — parent's fds were never touched by the child. */
    memcpy(sig_handlers, saved_parent.sigs, sizeof(sig_handlers));
    sig_pending = saved_parent.sig_pend;
    memcpy(vma_table,    saved_parent.vmas, sizeof(vma_table));

    saved_parent.valid = false;
}

/* ── Reset per-process syscall state ─────────────────────────── */

/* Tear down state owned by a process that is no longer current. Process
 * reaping commonly happens after the parent CR3/current_proc are restored,
 * so cleanup must not infer either value from global scheduler state. */
void syscall_cleanup_process(void *owner, uint64_t cr3,
                             fd_entry_t *fds, uint32_t pid)
{
    if (fds) {
        for (int i = 3; i < MAX_FDS; i++) {
            if (fds[i].open)
                close_fd_in_table(fds, (uint64_t)i);
        }
    }

    extern bool proc_is_thread_of(void *p);
    bool owner_is_thread = proc_is_thread_of(owner);
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use || vma_table[i].owner != owner)
            continue;
        /* CLONE_THREAD siblings share one address space. VMAs created by a
         * worker remain group-owned even if that worker exits; otherwise its
         * reaper can free a live sibling's stack or heap page. The one safe
         * exception is a stack whose own munmap was deferred above. */
        if (owner_is_thread &&
            vma_table[i].type != VMA_SHARED_STACK &&
            vma_table[i].type != VMA_THREAD_STACK &&
            vma_table[i].type != VMA_DEFERRED_STACK &&
            vma_table[i].type != VMA_DEFERRED_SHARED_STACK)
            continue;
        uint8_t type = vma_table[i].type;
        uint32_t owner_tgid = vma_table[i].owner_tgid;
        uint64_t freed_pages =
            vma_free_pages_in_cr3(&vma_table[i], cr3, pid);
        if (type == VMA_BRK) {
            serial_puts("[BRK] release pid=");
            serial_putdec(pid);
            serial_puts(" tgid=");
            serial_putdec(owner_tgid);
            serial_puts(" committed=");
            serial_putdec(freed_pages * 4);
            serial_puts("KB\n");
        }
        vma_clear_slot(&vma_table[i]);
    }
}

void syscall_reset_process(void)
{
    /* Close all FDs >= 3 */
    for (int i = 3; i < MAX_FDS; i++) {
        if (fd_table[i].open)
            close_fd_in_table(fd_table, (uint64_t)i);
    }

    /* Force-reset stdin/stdout/stderr to console.
     * A child process may have done dup2(file_fd, 1) to redirect stdout
     * to a file. Without this reset, the parent shell would write to
     * that file when it next prints (corrupting on-disk data).
     * Per-process fd_table: this only affects the current process. */
    syscall_seed_stdio(fd_table);

    /* Reset signal state */
    memset(sig_handlers, 0, sizeof(sig_handlers));
    sig_pending = 0;

    /* Reset terminal to canonical+echo mode (X-EDIT safety) */
    term_canonical = true;
    term_echo = true;
    cur_c_lflag = 0x8A3B;
    cur_c_iflag = 0x0500;
    cur_c_oflag = 0x0005;

    /* Free mmap regions belonging to the current process — walk PTEs
     * for demand-paged VMAs. The owner filter keeps sibling processes'
     * VMAs intact (X-PGTBL). Unlike the old code, we run this cleanup
     * even during fork+execve: the owner filter protects the parent's
     * VMAs while freeing the child's own VMAs, preventing physical-
     * page leaks when a fork'd child execve's a demand-paged binary. */
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vma_table[i].in_use) continue;
        if (!vma_owned_by_current(&vma_table[i])) continue;
        vma_free_pages(&vma_table[i]);
        vma_clear_slot(&vma_table[i]);
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
     * Kernel: use the private 0x90/0x98 GDT pair installed by gdt_init().
     * Do not derive this from the bootloader's SS: on real hardware UEFI can
     * leave selector 0x10 as a compat descriptor, and our near syscall return
     * would then run 64-bit ELF code with legacy instruction decoding.
     * User:   For SYSRET to give CS=user_cs, SS=user_ss
     *         CS = STAR[63:48]+16, SS = STAR[63:48]+8
     *         We want user_cs=0x43 (ring 3), user_ss=0x3B (ring 3)
     *         -> STAR[63:48] = 0x33 (so CS=0x33+16=0x43, SS=0x33+8=0x3B)
     *
     * But for now we only have ring 0, so user selectors don't matter yet.
     */
    uint16_t kernel_cs = get_cs();
    uint16_t kernel_ss;
    __asm__ volatile ("mov %%ss, %0" : "=r"(kernel_ss));

    uint64_t star = ((uint64_t)SYSRET_USER_BASE << 48) |
                    ((uint64_t)SYSCALL_KERNEL_CS << 32);

    serial_puts("[SYSCALL] Kernel CS=0x");
    serial_puthex(kernel_cs, 4);
    serial_puts(" SS=0x");
    serial_puthex(kernel_ss, 4);
    serial_puts(" SYSCALL CS=0x");
    serial_puthex(SYSCALL_KERNEL_CS, 4);
    serial_puts(" SS=0x");
    serial_puthex(SYSCALL_KERNEL_SS, 4);
    serial_puts(" SYSRET base=0x");
    serial_puthex(SYSRET_USER_BASE, 4);
    serial_puts("\n");

    wrmsr(MSR_STAR, star);

    /* LSTAR: RIP loaded on SYSCALL */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* FMASK: RFLAGS bits cleared on SYSCALL (mask IF + DF + TF) */
    wrmsr(MSR_FMASK, 0x700);  /* IF=0x200, DF=0x400, TF=0x100 */

    /* FD table is now per-process (inline in process_t). The kernel
     * process's stdio fds are seeded by proc_init() after it calls
     * proc_alloc("kernel"). */

    serial_puts("[SYSCALL] Ready (LSTAR=0x");
    serial_puthex((uint64_t)syscall_entry, 16);
    serial_puts(")\n");
    fb_puts(" Syscall: SYSCALL/SYSRET active\n");
}
