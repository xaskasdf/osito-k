/*
 * OsitoK x86-64 — COM1 Serial Driver (0x3F8)
 *
 * 115200 baud, 8N1. Used for debug console output.
 * Works with: minicom -D /dev/ttyS0 -b 115200
 */

#include "serial.h"

#define COM1_PORT   0x3F8

#define REG_DATA    0   /* Data (TX/RX) */
#define REG_IER     1   /* Interrupt Enable */
#define REG_FCR     2   /* FIFO Control (write) */
#define REG_LCR     3   /* Line Control */
#define REG_MCR     4   /* Modem Control */
#define REG_LSR     5   /* Line Status */

#define LSR_TX_EMPTY  0x20
#define LSR_RX_READY  0x01

#define SERIAL_TX_CAPACITY 16384u
#define SERIAL_FLUSH_NS 20000000ULL
#define SERIAL_RECORD_BYTES 64u

static serial_tx_slot_t tx_slots[SERIAL_TX_CAPACITY];
static serial_tx_t tx;

static uint8_t uart_status(void *opaque)
{
    (void)opaque;
    return inb(COM1_PORT + REG_LSR);
}

static void uart_write(void *opaque, uint8_t value)
{
    (void)opaque;
    outb(COM1_PORT + REG_DATA, value);
}

static uint64_t uart_now_ns(void *opaque)
{
    extern uint64_t idt_get_tsc_freq(void);
    extern uint64_t idt_get_monotonic_ns(void);
    (void)opaque;
    return idt_get_tsc_freq() ? idt_get_monotonic_ns() : 0;
}

static const serial_tx_ops_t uart_ops = {uart_status, uart_write, uart_now_ns};

void serial_init(void)
{
    bool present = inb(COM1_PORT + REG_LSR) != 0xff;
    serial_tx_init(&tx, tx_slots, SERIAL_TX_CAPACITY, 1, present, &uart_ops, NULL);
    if (!present) return;
    outb(COM1_PORT + REG_IER, 0x00);   /* Disable interrupts */
    outb(COM1_PORT + REG_LCR, 0x80);   /* DLAB=1 (set baud divisor) */
    outb(COM1_PORT + 0, 0x01);         /* Divisor lo: 1 = 115200 baud */
    outb(COM1_PORT + 1, 0x00);         /* Divisor hi */
    outb(COM1_PORT + REG_LCR, 0x03);   /* 8 bits, no parity, 1 stop */
    outb(COM1_PORT + REG_FCR, 0xC7);   /* Enable FIFO, 14-byte threshold */
    outb(COM1_PORT + REG_MCR, 0x0B);   /* DTR + RTS + OUT2 */
    if ((inb(COM1_PORT + REG_FCR) & 0xc0) == 0xc0)
        tx.fifo_depth = 16;
    else
        outb(COM1_PORT + REG_FCR, 0);  /* Do not use a broken 16550 FIFO. */
}

static void serial_record(const char *data, uint32_t length)
{
    extern bool shell_remote_capture(const char *, size_t) __attribute__((weak));
    if (shell_remote_capture && shell_remote_capture(data, length)) return;
    /* Keep the memory log independent of UART backpressure and availability. */
    extern void klog_putc(char c) __attribute__((weak));
    if (klog_putc)
        for (uint32_t i = 0; i < length; i++) klog_putc(data[i]);
    serial_tx_enqueue(&tx, data, length);
    extern bool sched_is_enabled(void) __attribute__((weak));
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    /* Runtime diagnostics must not hold network/device dispatch waiting for
     * the UART. The timer drains the queue; early boot and fatal paths still
     * use the bounded synchronous flush when that consumer cannot run. */
    if (sched_is_enabled && sched_is_enabled() && (flags & (1u << 9)))
        serial_poll();
    else
        serial_flush();
}

void serial_putc(char c)
{
    serial_record(&c, 1);
}

void serial_putchar(char c)
{
    if (c == '\n') serial_record("\r\n", 2);
    else serial_putc(c);
}

void serial_puts(const char *s)
{
    if (s) serial_write(s, strlen(s));
}

void serial_write(const char *data, uint64_t length)
{
    if (!data || !length) return;

    char record[SERIAL_RECORD_BYTES];
    uint32_t used = 0;
    for (uint64_t i = 0; i < length; i++) {
        char c = data[i];
        if (!c) continue;
        uint32_t needed = c == '\n' ? 2 : 1;
        if (used + needed > sizeof(record)) {
            serial_record(record, used);
            used = 0;
        }
        if (c == '\n') record[used++] = '\r';
        record[used++] = c;
    }
    if (used) serial_record(record, used);
}

void serial_puthex(uint64_t val, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char record[18] = {'0', 'x'};
    if (digits < 0) digits = 0;
    if (digits > 16) digits = 16;
    for (int i = 0; i < digits; i++)
        record[2 + i] = hex[(val >> ((digits - i - 1) * 4)) & 0xf];
    serial_record(record, 2 + digits);
}

void serial_putdec(uint64_t val)
{
    char buf[20];
    uint32_t i = sizeof(buf);
    do {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    } while (val);
    serial_record(buf + i, sizeof(buf) - i);
}

int serial_getc(void)
{
    if (!tx.present || !(inb(COM1_PORT + REG_LSR) & LSR_RX_READY))
        return -1;
    return inb(COM1_PORT + REG_DATA);
}

void serial_poll(void)
{
    serial_tx_poll(&tx);
}

void serial_poll_irq(void)
{
    /* Port I/O may cause expensive VM exits. Keep the timer's UART work to
     * one byte; ordinary producers can drain larger batches preemptibly. */
    serial_tx_poll_limit(&tx, 1);
}

bool serial_flush(void)
{
    return serial_tx_flush(&tx, SERIAL_FLUSH_NS);
}

void serial_get_stats(serial_stats_t *stats)
{
    if (stats) serial_tx_stats(&tx, stats);
}
