/*
 * OsitoK x86-64 — COM1 Serial Driver (0x3F8)
 *
 * 115200 baud, 8N1. Used for debug console output.
 * Works with: minicom -D /dev/ttyS0 -b 115200
 */

#include "../include/types.h"

#define COM1_PORT   0x3F8

#define REG_DATA    0   /* Data (TX/RX) */
#define REG_IER     1   /* Interrupt Enable */
#define REG_FCR     2   /* FIFO Control (write) */
#define REG_LCR     3   /* Line Control */
#define REG_MCR     4   /* Modem Control */
#define REG_LSR     5   /* Line Status */

#define LSR_TX_EMPTY  0x20
#define LSR_RX_READY  0x01

void serial_init(void)
{
    outb(COM1_PORT + REG_IER, 0x00);   /* Disable interrupts */
    outb(COM1_PORT + REG_LCR, 0x80);   /* DLAB=1 (set baud divisor) */
    outb(COM1_PORT + 0, 0x01);         /* Divisor lo: 1 = 115200 baud */
    outb(COM1_PORT + 1, 0x00);         /* Divisor hi */
    outb(COM1_PORT + REG_LCR, 0x03);   /* 8 bits, no parity, 1 stop */
    outb(COM1_PORT + REG_FCR, 0xC7);   /* Enable FIFO, 14-byte threshold */
    outb(COM1_PORT + REG_MCR, 0x0B);   /* DTR + RTS + OUT2 */
}

void serial_putc(char c)
{
    while (!(inb(COM1_PORT + REG_LSR) & LSR_TX_EMPTY))
        ;
    outb(COM1_PORT + REG_DATA, (uint8_t)c);
}

void serial_putchar(char c)
{
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        serial_putc(hex[(val >> i) & 0xF]);
}

void serial_putdec(uint64_t val)
{
    char buf[20];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0) serial_putc(buf[i]);
}

int serial_getc(void)
{
    if (!(inb(COM1_PORT + REG_LSR) & LSR_RX_READY))
        return -1;
    return inb(COM1_PORT + REG_DATA);
}
