/*
 * OsitoK x86-64 — Syscall Interface
 *
 * X-OS4: Fast syscall via SYSCALL/SYSRET (MSR-based).
 * ABI: RAX = syscall number, RDI/RSI/RDX/R10/R8/R9 = args.
 * Return value in RAX. Error: negative errno.
 *
 * Syscall entry point is in assembly (syscall_entry.S), which
 * saves registers and calls syscall_dispatch() here.
 *
 * Initial syscalls:
 *   0 = read(fd, buf, count)
 *   1 = write(fd, buf, count)
 *   2 = open(path, flags)      [stub]
 *   3 = close(fd)              [stub]
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
#define SYS_BRK         12
#define SYS_EXIT        60
#define SYS_ARCH_PRCTL  158

/* errno values */
#define ENOSYS  38
#define EBADF    9
#define EFAULT  14
#define EINVAL  22

/* ── Simple FD table (kernel-only for now) ───────────────────── */

#define MAX_FDS 16

typedef ssize_t (*fd_write_fn)(const void *buf, size_t count);
typedef ssize_t (*fd_read_fn)(void *buf, size_t count);

typedef struct {
    bool        open;
    fd_read_fn  read;
    fd_write_fn write;
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

/* ── Syscall handlers ────────────────────────────────────────── */

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!fd_table[fd].write) return -EBADF;
    if (!buf && count > 0) return -EFAULT;
    return fd_table[fd].write((const void *)buf, (size_t)count);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd >= MAX_FDS || !fd_table[fd].open) return -EBADF;
    if (!fd_table[fd].read) return -EBADF;
    if (!buf && count > 0) return -EFAULT;
    return fd_table[fd].read((void *)buf, (size_t)count);
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

/* brk stub — returns current break (no-op for now, heap handles this) */
static int64_t sys_brk(uint64_t addr)
{
    (void)addr;
    return 0;
}

/* ── Syscall dispatch (called from assembly) ─────────────────── */

int64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a4; (void)a5;

    switch (nr) {
    case SYS_READ:       return sys_read(a1, a2, a3);
    case SYS_WRITE:      return sys_write(a1, a2, a3);
    case SYS_OPEN:       return -ENOSYS;  /* stub */
    case SYS_CLOSE:      return -ENOSYS;  /* stub */
    case SYS_BRK:        return sys_brk(a1);
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

    fd_table[0].open = true;   /* stdin */
    fd_table[0].read = console_read;

    fd_table[1].open = true;   /* stdout */
    fd_table[1].write = console_write;

    fd_table[2].open = true;   /* stderr */
    fd_table[2].write = console_write;

    serial_puts("[SYSCALL] Ready (LSTAR=0x");
    serial_puthex((uint64_t)syscall_entry, 16);
    serial_puts(")\n");
    fb_puts(" Syscall: SYSCALL/SYSRET active\n");
}
