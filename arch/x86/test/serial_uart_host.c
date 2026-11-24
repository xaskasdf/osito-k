#include "serial_uart_host.h"

extern int printf(const char *format, ...);

static struct {
    char output[32768], log[32768];
    uint32_t length, logged, credit, depth, busy_reads, violations, writes;
    uint64_t clock;
    uint8_t lcr;
    bool absent, no_clock, stuck, scheduled;
} uart;

uint8_t serial_test_inb(uint16_t port)
{
    if (uart.absent) return 0xff;
    if (port == 0x3fa) return uart.depth == 16 ? 0xc0 : 0;
    if (port != 0x3fd) return 0;
    if (uart.stuck) return 0;
    if (uart.busy_reads) { uart.busy_reads--; return 0; }
    uart.credit = uart.depth;
    return SERIAL_TX_THRE | SERIAL_TX_TEMT;
}

void serial_test_outb(uint16_t port, uint8_t value)
{
    uart.writes++;
    if (port == 0x3fb) uart.lcr = value;
    if (port != 0x3f8 || (uart.lcr & 0x80)) return;
    if (!uart.credit || uart.length == sizeof(uart.output)) {
        uart.violations++;
        return;
    }
    uart.credit--;
    uart.output[uart.length++] = value;
}

void klog_putc(char c)
{
    if (uart.logged < sizeof(uart.log)) uart.log[uart.logged++] = c;
}

uint64_t idt_get_tsc_freq(void) { return uart.no_clock ? 0 : 1000000000ULL; }
uint64_t idt_get_monotonic_ns(void) { return uart.clock += 1000; }
bool sched_is_enabled(void) { return uart.scheduled; }

static void reset_uart(uint32_t depth)
{
    memset(&uart, 0, sizeof(uart));
    uart.depth = depth;
    serial_init();
}

int main(void)
{
    uint32_t checks = 0, failures = 0;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; printf("[SERIAL-UART] FAIL line=%u\n", __LINE__); } } while (0)
    reset_uart(16);
    serial_puts("checks=");
    uart.busy_reads = 5000;
    serial_putdec(371);
    serial_puts(" failures="); serial_putdec(0); serial_putchar('\n');
    static const char expected[] = "checks=371 failures=0\r\n";
    CHECK(uart.length == sizeof(expected) - 1 &&
          !memcmp(uart.output, expected, sizeof(expected) - 1));
    CHECK(uart.logged == sizeof(expected) - 1 &&
          !memcmp(uart.log, expected, sizeof(expected) - 1));
#ifndef SERIAL_LEGACY_CONTROL
    for (uint32_t depth = 1; depth <= 16; depth *= 16) {
        reset_uart(depth);
        serial_puthex(UINT64_MAX, 16); serial_putc(' ');
        serial_puthex(0xabcdef, 4); serial_putc(' ');
        serial_puthex(0, 0); serial_putc(' ');
        serial_puthex(1, 99); serial_putc(' ');
        serial_puthex(2, -1); serial_putc(' ');
        serial_putdec(UINT64_MAX); serial_putc(' '); serial_putdec(0);
        serial_putchar('\n');
        serial_write("a\0b\n", 4);
        serial_putc(0);
        serial_puts(NULL); serial_write(NULL, 10);
        static const char formatted[] =
            "0xFFFFFFFFFFFFFFFF 0xCDEF 0x 0x0000000000000001 0x "
            "18446744073709551615 0\r\nab\r\n\0";
        CHECK(uart.length == sizeof(formatted) - 1 && !uart.violations);
        CHECK(!memcmp(uart.output, formatted, sizeof(formatted) - 1));
        CHECK(uart.logged == uart.length && !memcmp(uart.log, uart.output, uart.length));
        serial_stats_t stats;
        serial_get_stats(&stats);
        CHECK(stats.accepted == uart.length && stats.sent == uart.length &&
              !stats.pending && !stats.dropped && stats.fifo_depth == depth);
    }

    reset_uart(16);
    uart.stuck = true;
    serial_puts("delayed\n");
    serial_stats_t stats;
    serial_get_stats(&stats);
    CHECK(stats.pending == 9 && stats.sent == 0 && stats.timeouts == 1 &&
          !stats.dropped && uart.logged == 9 && !uart.length);
    uart.stuck = false;
    CHECK(serial_flush());
    CHECK(uart.length == 9 && !memcmp(uart.output, "delayed\r\n", 9));

    reset_uart(16);
    uart.no_clock = uart.stuck = true;
    serial_puts("early\n");
    CHECK(uart.length == 0 && uart.logged == 7 && uart.clock == 0);
    uart.stuck = false;
    serial_poll();
    CHECK(uart.length == 7 && !memcmp(uart.output, "early\r\n", 7));

    /* A slow debug UART must not consume a timeout for each runtime record. */
    reset_uart(16);
    uart.scheduled = true;
    uart.busy_reads = 5000;
    for (unsigned i = 0; i < 100; i++) serial_puts("runtime\n");
    serial_get_stats(&stats);
    CHECK(uart.busy_reads == 4900 && uart.clock == 0);
    CHECK(stats.accepted == 900 && stats.pending == 900 && !stats.sent &&
          !stats.timeouts && !stats.dropped && uart.logged == 900);
    uart.busy_reads = 0;
    serial_poll_irq();
    CHECK(uart.length == 1 && !uart.violations && uart.clock == 0);
    serial_poll();
    CHECK(uart.length == 65 && !uart.violations && uart.clock == 0);
    CHECK(serial_flush());
    serial_get_stats(&stats);
    CHECK(stats.sent == 900 && !stats.pending && !stats.timeouts &&
          !memcmp(uart.output, uart.log, 900));

    reset_uart(16);
    uart.scheduled = uart.stuck = true;
    serial_puts("runtime-stuck\n");
    serial_get_stats(&stats);
    CHECK(stats.pending == 15 && !stats.timeouts && uart.clock == 0 &&
          uart.logged == 15 && !uart.length);
    CHECK(!serial_flush());
    serial_get_stats(&stats);
    CHECK(stats.pending == 15 && stats.timeouts == 1);
    uart.stuck = false;
    CHECK(serial_flush() && uart.length == 15);

    memset(&uart, 0, sizeof(uart));
    uart.absent = true;
    serial_init();
    serial_puts("offline\n");
    serial_get_stats(&stats);
    CHECK(!stats.present && stats.dropped == 9 && !stats.pending);
    CHECK(!uart.writes && !uart.length && uart.logged == 9 && serial_getc() == -1);
#endif
#undef CHECK
    printf("[SERIAL-UART] checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
