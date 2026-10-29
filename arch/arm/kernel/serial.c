/*
 * serial.c -- Common serial wrappers over platform UART
 *
 * Provides \n -> \r\n conversion and hex/decimal formatting.
 */

#include "../include/hal.h"

/* Every byte also goes to the framebuffer console (a no-op until
 * fb_init_platform, so always on virt). Framebuffer first: if the UART
 * stalls, the screen still shows the text. */
static void out(char c)
{
    fb_putc(c);
    uart_putc(c);
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
