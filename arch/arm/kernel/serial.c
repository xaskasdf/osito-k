/*
 * serial.c -- Common serial wrappers over platform UART
 *
 * Provides \n -> \r\n conversion and hex/decimal formatting.
 */

#include "../include/hal.h"

void serial_init(void)
{
    uart_init();
}

void serial_putchar(char c)
{
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
}

void serial_putc(char c)
{
    serial_putchar(c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
        s++;
    }
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789abcdef";

    uart_putc('0');
    uart_putc('x');

    if (digits <= 0) digits = 16;

    for (int i = digits - 1; i >= 0; i--) {
        int nibble = (val >> (i * 4)) & 0xF;
        uart_putc(hex[nibble]);
    }
}

void serial_putdec(uint64_t val)
{
    char buf[21];   /* max uint64 = 20 digits + NUL */
    int i = 0;

    if (val == 0) {
        uart_putc('0');
        return;
    }

    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }

    /* Print in reverse */
    while (i > 0)
        uart_putc(buf[--i]);
}

char serial_getc(void)
{
    int c = uart_trygetc();
    return (c < 0) ? 0 : (char)c;
}
