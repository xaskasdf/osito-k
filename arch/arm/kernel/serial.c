/*
 * serial.c -- Common serial wrappers over platform UART
 *
 * Provides \n -> \r\n conversion and hex/decimal formatting.
 */

#include "../include/hal.h"

/* Kernel log: the last KLOG_SIZE bytes of console output, for `dmesg`. On
 * SM8350 the boot messages are printed before the host can open the USB
 * console, and the framebuffer console wraps over them. */
#define KLOG_SIZE 16384
static char klog[KLOG_SIZE];
static uint32_t klog_total;         /* bytes ever logged; wraps the ring */
static int klog_muted;

static void out_raw(char c)
{
    fb_putc(c);
    uart_putc(c);
}

/* Every byte also goes to the framebuffer console (a no-op until
 * fb_init_platform, so always on virt). Framebuffer first: if the UART
 * stalls, the screen still shows the text. */
static void out(char c)
{
    if (!klog_muted)
        klog[klog_total++ % KLOG_SIZE] = c;
    out_raw(c);
}

/* Replay the log; muted so the replay doesn't log itself over the ring */
void klog_dump(void)
{
    uint32_t end = klog_total;
    uint32_t start = end > KLOG_SIZE ? end - KLOG_SIZE : 0;
    klog_muted = 1;
    for (uint32_t i = start; i < end; i++)
        out_raw(klog[i % KLOG_SIZE]);
    klog_muted = 0;
}

void serial_init(void)
{
    uart_init();
}

void serial_putchar(char c)
{
    if (c == '\n')
        out('\r');
    out(c);
}

void serial_putc(char c)
{
    serial_putchar(c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            out('\r');
        out(*s);
        s++;
    }
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789abcdef";

    out('0');
    out('x');

    if (digits <= 0) digits = 16;

    for (int i = digits - 1; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xF;
        out(hex[nibble]);
    }
}

void serial_putdec(uint64_t val)
{
    char buf[21];   /* max uint64 = 20 digits + NUL */
    int i = 0;

    if (val == 0) {
        out('0');
        return;
    }

    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }

    /* Print in reverse */
    while (i > 0)
        out(buf[--i]);
}

char serial_getc(void)
{
    int c = uart_trygetc();
    return (c < 0) ? 0 : (char)c;
}
