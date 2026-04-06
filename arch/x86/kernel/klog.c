/*
 * OsitoK x86-64 — Kernel Log Ring Buffer (dmesg)
 *
 * Circular buffer that captures all serial_puts output.
 * Accessible via dmesg shell command and syslog syscall.
 */

#include "../include/types.h"

extern uint64_t idt_get_ticks(void);

/* ── Ring Buffer ─────────────────────────────────────────────── */

#define KLOG_SIZE  (64 * 1024)  /* 64KB ring buffer */

static char *klog_buf;  /* Lazy alloc (saves ~64KB BSS) */
static uint32_t klog_head;       /* Write position */
static uint32_t klog_total;      /* Total bytes ever written */
static bool     klog_initialized;

void klog_init(void)
{
    extern void *kmalloc(uint64_t);
    klog_buf = (char *)kmalloc(KLOG_SIZE);
    if (!klog_buf) return;
    memset(klog_buf, 0, KLOG_SIZE);
    klog_head = 0;
    klog_total = 0;
    klog_initialized = true;
}

/* Append a character to the ring buffer */
void klog_putc(char c)
{
    if (!klog_initialized) return;
    klog_buf[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    klog_total++;
}

/* Append a string to the ring buffer */
void klog_puts(const char *s)
{
    while (*s) klog_putc(*s++);
}

/* ── Read Interface ──────────────────────────────────────────── */

/* Read the last N bytes from the log.
 * Returns bytes copied into buf. */
uint32_t klog_read(char *buf, uint32_t max_len)
{
    if (!klog_initialized || max_len == 0) return 0;

    uint32_t available = (klog_total < KLOG_SIZE) ? klog_total : KLOG_SIZE;
    uint32_t to_copy = (available < max_len) ? available : max_len;

    /* Calculate start position in ring */
    uint32_t start;
    if (klog_total <= KLOG_SIZE) {
        start = 0;
        to_copy = klog_total < max_len ? klog_total : max_len;
    } else {
        start = klog_head;  /* Oldest data is at current head */
        /* Skip to show only the last max_len bytes */
        if (available > max_len) {
            start = (start + available - max_len) % KLOG_SIZE;
        }
    }

    /* Copy from ring to linear buffer */
    for (uint32_t i = 0; i < to_copy; i++) {
        buf[i] = klog_buf[(start + i) % KLOG_SIZE];
    }
    return to_copy;
}

/* Get total bytes logged */
uint32_t klog_total_bytes(void) { return klog_total; }

/* ── syslog Syscall Interface ────────────────────────────────── */

/* syslog(type, buf, len):
 *   type 2 = read last len bytes
 *   type 3 = read + clear
 *   type 10 = get log size */
int64_t klog_syslog(int type, char *buf, uint32_t len)
{
    switch (type) {
    case 2:  /* SYSLOG_ACTION_READ */
    case 3:  /* SYSLOG_ACTION_READ_ALL */
        return (int64_t)klog_read(buf, len);
    case 10: /* SYSLOG_ACTION_SIZE_BUFFER */
        return (int64_t)KLOG_SIZE;
    default:
        return 0;
    }
}
