#include "serial.h"

#ifdef SERIAL_HOST_TEST
extern int printf(const char *format, ...);
#else
#include "smp.h"
#include "smp_work.h"
#endif

typedef struct {
    serial_tx_t *tx;
    uint8_t output[1024];
    uint32_t length, reads, credit, depth, busy_reads, violations;
    uint64_t now, step;
    bool stuck, shift_busy, no_clock, absent, reenter, nested_ok;
} fake_uart_t;

static uint8_t fake_status(void *opaque)
{
    fake_uart_t *uart = opaque;
    uart->reads++;
    if (uart->absent) return 0xff;
    if (uart->stuck) return 0;
    if (uart->busy_reads) {
        uart->busy_reads--;
        return 0;
    }
    uart->credit = uart->depth;
    return SERIAL_TX_THRE | (uart->shift_busy ? 0 : SERIAL_TX_TEMT);
}

static void fake_write(void *opaque, uint8_t value)
{
    fake_uart_t *uart = opaque;
    if (!uart->credit || uart->length == sizeof(uart->output)) {
        uart->violations++;
        return;
    }
    uart->credit--;
    uart->output[uart->length++] = value;
    if (uart->reenter) {
        uart->reenter = false;
        uart->nested_ok = serial_tx_enqueue(uart->tx, "B", 1) &&
                          !serial_tx_flush(uart->tx, 100000);
        serial_tx_poll(uart->tx);
    }
}

static uint64_t fake_now(void *opaque)
{
    fake_uart_t *uart = opaque;
    if (uart->no_clock) return 0;
    uart->now += uart->step;
    return uart->now;
}

static const serial_tx_ops_t fake_ops = {fake_status, fake_write, fake_now};

static void fake_init(serial_tx_t *tx, serial_tx_slot_t *slots,
                      fake_uart_t *uart, uint32_t capacity, uint32_t depth)
{
    memset(uart, 0, sizeof(*uart));
    uart->tx = tx;
    uart->now = 1;
    uart->step = 1;
    uart->depth = depth;
    serial_tx_init(tx, slots, capacity, depth, true, &fake_ops, uart);
}

static void failed_check(unsigned line)
{
#ifdef SERIAL_HOST_TEST
    printf("[SERIAL-CORE] FAIL line=%u\n", line);
#else
    serial_puts("[SERIAL-CORE] FAIL line=");
    serial_putdec(line);
    serial_puts("\n");
#endif
}

static int serial_core_selftest(void)
{
    serial_tx_t tx;
    serial_tx_slot_t slots[256];
    fake_uart_t uart;
    serial_stats_t stats;
    uint32_t checks = 0, failures = 0;
    char bytes[256];
    for (uint32_t i = 0; i < sizeof(bytes); i++) bytes[i] = (char)i;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; failed_check(__LINE__); } } while (0)

    CHECK(!serial_tx_init(&tx, slots, 0, 1, true, &fake_ops, &uart));
    CHECK(!serial_tx_init(&tx, slots, 3, 1, true, &fake_ops, &uart));
    CHECK(!serial_tx_init(&tx, slots, 32, 2, true, &fake_ops, &uart));
    CHECK(!serial_tx_init(&tx, slots, 32, 1, true, NULL, &uart));
    for (uint32_t depth = 1; depth <= 16; depth *= 16) {
        fake_init(&tx, slots, &uart, 256, depth);
        uart.busy_reads = 5000;
        CHECK(serial_tx_enqueue(&tx, bytes, 96));
        CHECK(serial_tx_flush(&tx, 100000));
        CHECK(uart.reads > 4096 && uart.length == 96 && !uart.violations);
        CHECK(!memcmp(uart.output, bytes, 96));
        serial_tx_stats(&tx, &stats);
        CHECK(stats.accepted == 96 && stats.sent == 96 && !stats.pending &&
              !stats.dropped && !stats.timeouts);
    }

    fake_init(&tx, slots, &uart, 256, 16);
    uart.stuck = true;
    CHECK(serial_tx_enqueue(&tx, bytes, 80));
    CHECK(!serial_tx_flush(&tx, 50));
    serial_tx_stats(&tx, &stats);
    CHECK(stats.pending == 80 && stats.accepted == 80 && stats.sent == 0 &&
          stats.timeouts == 1 && stats.dropped == 0 && uart.length == 0);
    uart.stuck = false;
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 80 && !memcmp(uart.output, bytes, 80));

    fake_init(&tx, slots, &uart, 256, 16);
    uart.no_clock = uart.stuck = true;
    CHECK(serial_tx_enqueue(&tx, bytes, 100));
    CHECK(!serial_tx_flush(&tx, 1000));
    CHECK(uart.reads == 1 && tx.head - tx.tail == 100 && !tx.timeouts);
    uart.stuck = false;
    CHECK(!serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 64 && tx.head - tx.tail == 36);
    uart.no_clock = false;
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 100 && !memcmp(uart.output, bytes, 100));

    fake_init(&tx, slots, &uart, 256, 16);
    CHECK(serial_tx_enqueue(&tx, bytes, 256));
    for (uint32_t i = 1; i <= 4; i++) {
        serial_tx_poll(&tx);
        CHECK(uart.length == 64 * i && !uart.violations);
    }
    CHECK(!memcmp(uart.output, bytes, 256));

    for (uint32_t depth = 1; depth <= 16; depth *= 16) {
        fake_init(&tx, slots, &uart, 256, depth);
        CHECK(serial_tx_enqueue(&tx, bytes, 100));
        serial_tx_poll_limit(&tx, 0);
        CHECK(!uart.reads && !uart.length);
        serial_tx_poll_limit(&tx, 1);
        CHECK(uart.reads == 1 && uart.length == 1 && !uart.violations);
        serial_tx_poll_limit(&tx, 3);
        CHECK(uart.length == 4 && !uart.violations);
        serial_tx_poll_limit(&tx, UINT32_MAX);
        CHECK(uart.length == 68 && !uart.violations);
        serial_tx_poll(&tx);
        CHECK(uart.length == 100 && !memcmp(uart.output, bytes, 100));
    }

    fake_init(&tx, slots, &uart, 32, 1);
    CHECK(serial_tx_enqueue(&tx, bytes, 32));
    CHECK(!serial_tx_enqueue(&tx, "xyz", 3));
    CHECK(!serial_tx_enqueue(&tx, NULL, 2));
    CHECK(!serial_tx_enqueue(&tx, bytes, 33));
    CHECK(serial_tx_enqueue(&tx, NULL, 0));
    CHECK(tx.head - tx.tail == 32 && tx.dropped == 38);
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 32 && !memcmp(uart.output, bytes, 32));
    for (uint32_t i = 0; i < 12; i++) {
        CHECK(serial_tx_enqueue(&tx, bytes, 23));
        CHECK(serial_tx_flush(&tx, 1000));
        CHECK(!memcmp(uart.output + 32 + 23 * i, bytes, 23));
    }

    fake_init(&tx, slots, &uart, 64, 16);
    tx.head = tx.tail = UINT64_MAX - 12;
    CHECK(serial_tx_enqueue(&tx, bytes, 40));
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(tx.head == 27 && tx.tail == 27 && !memcmp(uart.output, bytes, 40));

    /* Suspend the first producer between reservation and publication. */
    fake_init(&tx, slots, &uart, 32, 16);
    tx.head = tx.accepted = 3;
    CHECK(serial_tx_enqueue(&tx, "def", 3));
    CHECK(!serial_tx_flush(&tx, 1000));
    serial_tx_poll(&tx);
    CHECK(tx.tail == 0 && uart.reads == 0 && uart.length == 0);
    slots[1].value = 'b'; slots[2].value = 'c';
    __atomic_store_n(&slots[1].ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&slots[2].ready, 1, __ATOMIC_RELEASE);
    serial_tx_poll(&tx);
    CHECK(tx.tail == 0 && uart.reads == 0);
    slots[0].value = 'a';
    __atomic_store_n(&slots[0].ready, 1, __ATOMIC_RELEASE);
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 6 && !memcmp(uart.output, "abcdef", 6));

    fake_init(&tx, slots, &uart, 32, 16);
    uart.reenter = true;
    CHECK(serial_tx_enqueue(&tx, "AC", 2));
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.nested_ok && tx.busy == 2 && tx.head - tx.tail == 1);
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 3 && !memcmp(uart.output, "ACB", 3) && !uart.violations);

    fake_init(&tx, slots, &uart, 32, 16);
    uart.shift_busy = true;
    CHECK(serial_tx_enqueue(&tx, "wire", 4));
    CHECK(!serial_tx_flush(&tx, 50));
    CHECK(tx.head == tx.tail && tx.sent == 4 && tx.timeouts == 1);
    uart.shift_busy = false;
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 4 && !memcmp(uart.output, "wire", 4));

    fake_init(&tx, slots, &uart, 32, 16);
    uart.absent = true;
    CHECK(serial_tx_enqueue(&tx, "late", 4));
    CHECK(!serial_tx_flush(&tx, 50) && uart.length == 0 && tx.head - tx.tail == 4);
    uart.absent = false;
    CHECK(serial_tx_flush(&tx, 1000) && uart.length == 4);
    fake_init(&tx, slots, &uart, 32, 16);
    tx.present = false;
    CHECK(!serial_tx_enqueue(&tx, "offline", 7));
    CHECK(!serial_tx_flush(&tx, 1000));
    serial_tx_poll(&tx);
    CHECK(tx.dropped == 7 && tx.head == tx.tail && !uart.length && !uart.reads);

    fake_init(&tx, slots, &uart, 256, 16);
    CHECK(serial_tx_enqueue(&tx, bytes, 128));
    CHECK(!serial_tx_flush(&tx, 2));
    CHECK(tx.sent == 16 && tx.head - tx.tail == 112);
    CHECK(serial_tx_flush(&tx, 1000));
    CHECK(uart.length == 128 && !memcmp(uart.output, bytes, 128));
#undef CHECK
#ifdef SERIAL_HOST_TEST
    printf("[SERIAL-CORE] checks=%u failures=%u\n", checks, failures);
#else
    serial_puts("[SERIAL-CORE] checks="); serial_putdec(checks);
    serial_puts(" failures="); serial_putdec(failures); serial_puts("\n");
#endif
    return failures;
}

#ifdef SERIAL_HOST_TEST
int main(void) { return serial_core_selftest() != 0; }
#else
typedef struct {
    uint32_t mask;
} serial_test_group_t;

typedef struct {
    serial_test_group_t *group;
    uint32_t id;
} serial_test_worker_t;

static char *frame_hex(char *out, uint32_t value, uint32_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    while (digits) *out++ = hex[(value >> (--digits * 4)) & 15];
    return out;
}

static void serial_test_worker(void *arg, void *result)
{
    serial_test_worker_t *worker = arg;
    (void)result;
    uint32_t cpu = smp_current_cpu();
    __atomic_fetch_or(&worker->group->mask, 1u << cpu, __ATOMIC_RELAXED);
    for (uint32_t seq = 0; seq < 64; seq++) {
        char frame[64];
        strcpy(frame, "[SERIAL-FRAME] ");
        char *out = frame + strlen(frame);
        out = frame_hex(out, worker->id, 2); *out++ = ' ';
        out = frame_hex(out, seq, 4); *out++ = ' ';
        out = frame_hex(out, cpu, 2); *out++ = ' ';
        uint32_t sum = 0x9e3779b9u ^ ((worker->id + 1) * 0x10001u) ^
                       (seq * 0x45d9f3bu) ^ cpu;
        out = frame_hex(out, sum, 8); *out++ = '\n';
        serial_write(frame, out - frame);
    }
}

void serial_stats_dump(void)
{
    serial_stats_t stats;
    serial_flush();
    serial_get_stats(&stats);
    serial_puts("[SERIAL] accepted="); serial_putdec(stats.accepted);
    serial_puts(" sent="); serial_putdec(stats.sent);
    serial_puts(" pending="); serial_putdec(stats.pending);
    serial_puts(" dropped="); serial_putdec(stats.dropped);
    serial_puts(" timeouts="); serial_putdec(stats.timeouts);
    serial_puts(" busy="); serial_putdec(stats.busy);
    serial_puts(" capacity="); serial_putdec(stats.capacity);
    serial_puts(" fifo="); serial_putdec(stats.fifo_depth);
    serial_puts(" present="); serial_putdec(stats.present);
    serial_puts("\n");
}

int serial_selftest(void)
{
    extern uint64_t idt_get_monotonic_ns(void);
    int failures = serial_core_selftest();
    serial_puts("[SERIAL-FORMAT] ");
    serial_puthex(UINT64_MAX, 16); serial_putc(' ');
    serial_puthex(0xabcdef, 4); serial_putc(' ');
    serial_puthex(0, 0); serial_putc(' ');
    serial_puthex(1, 99); serial_putc(' ');
    serial_puthex(2, -1); serial_putc(' ');
    serial_putdec(UINT64_MAX); serial_putc(' '); serial_putdec(0);
    serial_putchar('\n');
    serial_write("[SERIAL-NUL] a\0b\n", 17);

    serial_flush();
    serial_stats_t before, after;
    serial_get_stats(&before);
    serial_test_group_t group = {0};
    serial_test_worker_t workers[4];
    int tasks[4] = {-1, -1, -1, -1};
    for (uint32_t i = 0; i < 4; i++) {
        workers[i].group = &group;
        workers[i].id = i;
        if (i) {
            tasks[i] = smp_submit_any(serial_test_worker, &workers[i], NULL);
            if (tasks[i] < 0) failures++;
        }
    }
    serial_test_worker(&workers[0], NULL);
    for (uint32_t i = 1; i < 4; i++)
        if (tasks[i] >= 0) smp_wait(tasks[i]);
    uint64_t start = idt_get_monotonic_ns();
    bool complete;
    do {
        complete = serial_flush();
    } while (!complete && idt_get_monotonic_ns() - start < 2000000000ULL);
    serial_get_stats(&after);
    uint32_t mask = __atomic_load_n(&group.mask, __ATOMIC_RELAXED);
    if (!complete || after.pending || after.dropped != before.dropped ||
        (smp_cpu_count() > 1 && !(mask & (mask - 1)))) failures++;
    serial_puts("[SERIAL-SMP] records=256 cpumask="); serial_puthex(mask, 8);
    serial_puts(" dropped="); serial_putdec(after.dropped - before.dropped);
    serial_puts(" pending="); serial_putdec(after.pending); serial_puts("\n");
    serial_stats_dump();
    return failures;
}
#endif
