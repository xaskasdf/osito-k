/*
 * OsitoK x86-64 — Kernel Tracepoints (ftrace-like)
 *
 * Static instrumentation points in the kernel.
 * Tracepoints record events to a ring buffer for analysis.
 * Categories: sched, syscall, irq, net, fs, mm.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);

#define TRACE_BUF_SIZE  1024
#define TRACE_MAX_LEN   64

typedef struct {
    uint64_t timestamp;
    uint32_t cpu;
    uint32_t pid;
    uint8_t  category;
    uint8_t  event;
    uint16_t data_len;
    uint8_t  data[TRACE_MAX_LEN];
} trace_entry_t;

/* Categories */
#define TRACE_SCHED   0
#define TRACE_SYSCALL 1
#define TRACE_IRQ     2
#define TRACE_NET     3
#define TRACE_FS      4
#define TRACE_MM      5

/* Events */
#define TRACE_SCHED_SWITCH   0
#define TRACE_SCHED_WAKEUP   1
#define TRACE_SYSCALL_ENTER  0
#define TRACE_SYSCALL_EXIT   1
#define TRACE_IRQ_ENTER      0
#define TRACE_IRQ_EXIT       1

static trace_entry_t trace_buf[TRACE_BUF_SIZE];
static uint32_t trace_head;
static uint32_t trace_count;
static bool     trace_enabled;
static uint8_t  trace_mask = 0xFF;  /* All categories enabled */

void trace_init(void)
{
    trace_head = trace_count = 0;
    trace_enabled = false;
    serial_puts("[TRACE] Tracepoint system ready\n");
}

void trace_start(void) { trace_enabled = true; trace_head = trace_count = 0; }
void trace_stop(void)  { trace_enabled = false; }
void trace_set_mask(uint8_t mask) { trace_mask = mask; }

void trace_record(uint8_t category, uint8_t event, uint32_t pid,
                  const void *data, uint16_t len)
{
    if (!trace_enabled || !(trace_mask & (1 << category))) return;
    if (len > TRACE_MAX_LEN) len = TRACE_MAX_LEN;

    trace_entry_t *e = &trace_buf[trace_head % TRACE_BUF_SIZE];
    e->timestamp = idt_get_ticks();
    e->cpu = 0;
    e->pid = pid;
    e->category = category;
    e->event = event;
    e->data_len = len;
    if (len > 0 && data) memcpy(e->data, data, len);
    trace_head++;
    if (trace_count < TRACE_BUF_SIZE) trace_count++;
}

void trace_dump(uint32_t max_entries)
{
    if (max_entries == 0 || max_entries > trace_count) max_entries = trace_count;
    uint32_t start = (trace_head >= max_entries) ? trace_head - max_entries : 0;

    serial_puts("[TRACE] Dump (");
    serial_putdec(max_entries);
    serial_puts(" entries):\n");

    static const char *cat_names[] = {"sched","syscall","irq","net","fs","mm"};

    for (uint32_t i = 0; i < max_entries; i++) {
        trace_entry_t *e = &trace_buf[(start + i) % TRACE_BUF_SIZE];
        serial_puts("  t="); serial_putdec(e->timestamp);
        serial_puts(" pid="); serial_putdec(e->pid);
        serial_puts(" ");
        if (e->category < 6) serial_puts(cat_names[e->category]);
        serial_puts(":"); serial_putdec(e->event);
        serial_puts("\n");
    }
}

uint32_t trace_entry_count(void) { return trace_count; }
