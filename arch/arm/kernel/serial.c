/*
 * serial.c -- Common serial wrappers over platform UART
 *
 * Provides \n -> \r\n conversion and hex/decimal formatting.
 */

#include "../include/hal.h"
#include "../include/spinlock.h"
#include "task.h"

/* Kernel log: the last KLOG_SIZE bytes of console output, for `dmesg` and
 * the screen's history view (console_scroll). On SM8350 the boot messages
 * are printed before the host can open the USB console, and the
 * framebuffer console scrolls them away. */
#define KLOG_SIZE 16384
static char klog[KLOG_SIZE];
static uint32_t klog_total;         /* bytes ever logged; wraps the ring */
static int klog_muted;
static volatile int host_held;      /* console_host_hold */

/* console_capture: what one task prints goes to a function instead */
static void (*capture_fn)(char c);
static void *capture_task;

static void out_raw(char c)
{
    fb_putc(c);
    if (!host_held)
        uart_putc(c);
}

/* Every byte also goes to the framebuffer console (a no-op until
 * fb_init_platform, so always on virt). Framebuffer first: if the UART
 * stalls, the screen still shows the text. */
static void out(char c)
{
    if (capture_fn && current_task == capture_task) {
        if (c != '\r')              /* the '\n' -> "\r\n" is the console's */
            capture_fn(c);
        return;
    }
    if (!klog_muted)
        klog[klog_total++ % KLOG_SIZE] = c;
    out_raw(c);
}

/* Console lock: the USB gadget, the framebuffer console and the log are
 * not reentrant. IRQs stay off while it is held, so an IRQ handler that
 * prints can't spin on its own CPU's lock; a CPU that already holds it
 * (an exception while printing) just nests. */
static spinlock_t con_lock;
static volatile int con_owner = -1;
static int con_depth;

#define CON_COUNTED 1ULL    /* in the returned flags: DAIF bit 0 is RES0 */

uint64_t console_lock(void)
{
    uint64_t flags = irq_save();
    if (!smp_running)
        return flags;
    int me = smp_cpu_id();
    if (con_owner != me) {
        spin_lock(&con_lock);
        con_owner = me;
    }
    con_depth++;
    return flags | CON_COUNTED;
}

void console_unlock(uint64_t flags)
{
    if ((flags & CON_COUNTED) && --con_depth == 0) {
        con_owner = -1;
        spin_unlock(&con_lock);
    }
    irq_restore(flags & ~CON_COUNTED);
}

void console_host_hold(int on)
{
    uint64_t flags = console_lock();
    host_held = on;
    console_unlock(flags);
}

void (*console_capture(void (*fn)(char c)))(char c)
{
    uint64_t flags = console_lock();
    void (*prev)(char c) = capture_fn;
    capture_fn = fn;
    capture_task = fn ? (void *)current_task : (void *)0;
    console_unlock(flags);
    return prev;
}

/* Replay the log; muted so the replay doesn't log itself over the ring */
void klog_dump(void)
{
    uint64_t flags = console_lock();
    uint32_t end = klog_total;
    uint32_t start = end > KLOG_SIZE ? end - KLOG_SIZE : 0;
    klog_muted = 1;
    int captured = capture_fn && current_task == capture_task;
    for (uint32_t i = start; i < end; i++) {
        char c = klog[i % KLOG_SIZE];
        if (!captured)
            out_raw(c);
        else if (c != '\r')
            capture_fn(c);              /* `dmesg | grep ...` */
    }
    klog_muted = 0;
    console_unlock(flags);
}

int console_scroll(int lines)
{
    uint64_t flags = console_lock();
    int back = fb_history(klog, KLOG_SIZE, klog_total, lines);
    console_unlock(flags);
    return back;
}

void console_redraw(void)
{
    uint64_t flags = console_lock();
    fb_redraw_log(klog, KLOG_SIZE, klog_total);
    console_unlock(flags);
}

void serial_init(void)
{
    uart_init();
}

void serial_putchar(char c)
{
    uint64_t flags = console_lock();
    if (c == '\n')
        out('\r');
    out(c);
    console_unlock(flags);
}

void serial_putc(char c)
{
    serial_putchar(c);
}

void serial_puts(const char *s)
{
    uint64_t flags = console_lock();
    while (*s) {
        if (*s == '\n')
            out('\r');
        out(*s);
        s++;
    }
    console_unlock(flags);
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789abcdef";
    uint64_t flags = console_lock();

    out('0');
    out('x');

    if (digits <= 0) digits = 16;

    for (int i = digits - 1; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xF;
        out(hex[nibble]);
    }
    console_unlock(flags);
}

void serial_putdec(uint64_t val)
{
    char buf[21];   /* max uint64 = 20 digits + NUL */
    int i = 0;

    do {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);

    /* Print in reverse */
    uint64_t flags = console_lock();
    while (i > 0)
        out(buf[--i]);
    console_unlock(flags);
}

char serial_getc(void)
{
    int c = uart_trygetc();
    return (c < 0) ? 0 : (char)c;
}
