/*
 * OsitoK x86-64 — Kernel Profiler
 *
 * Sampling profiler: periodically records RIP from timer interrupt.
 * Builds a histogram of instruction pointer values for hotspot analysis.
 * Output via serial (text-mode flame graph data).
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);
extern uint64_t idt_get_ticks(void);

/* ── Profile Data ────────────────────────────────────────────── */

#define KPROF_MAX_SAMPLES 4096
#define KPROF_BUCKETS     256   /* Histogram buckets (16KB granularity) */
#define KPROF_BUCKET_SHIFT 14   /* 2^14 = 16KB per bucket */

typedef struct {
    uint64_t rip;
    uint64_t tick;
} kprof_sample_t;

static kprof_sample_t *samples;  /* Lazy alloc (saves ~64KB BSS) */
static uint32_t sample_head;
static uint32_t sample_count;
static uint32_t histogram[KPROF_BUCKETS];
static uint64_t histogram_samples;  /* always-on sample counter */
static bool     profiling;          /* ring buffer recording (opt-in) */
static uint64_t prof_start_tick;

/* ── Control ─────────────────────────────────────────────────── */

void kprof_start(void)
{
    if (!samples) {
        extern void *kmalloc(uint64_t);
        samples = (kprof_sample_t *)kmalloc(KPROF_MAX_SAMPLES * sizeof(kprof_sample_t));
        if (!samples) return;
    }
    memset(samples, 0, KPROF_MAX_SAMPLES * sizeof(kprof_sample_t));
    memset(histogram, 0, sizeof(histogram));
    sample_head = 0;
    sample_count = 0;
    prof_start_tick = idt_get_ticks();
    profiling = true;
    serial_puts("[KPROF] Profiling started\n");
}

void kprof_stop(void)
{
    profiling = false;
    uint64_t elapsed = idt_get_ticks() - prof_start_tick;
    serial_puts("[KPROF] Profiling stopped (");
    serial_putdec(sample_count);
    serial_puts(" samples in ");
    serial_putdec(elapsed / 100);
    serial_puts("s)\n");
}

bool kprof_is_active(void) { return profiling; }

/* ── Sample Recording (called from timer ISR) ────────────────── */

void kprof_record(uint64_t rip)
{
    /* Always-on: histogram records every tick (~3 cycles, one array write) */
    uint32_t bucket = (uint32_t)(rip >> KPROF_BUCKET_SHIFT);
    if (bucket < KPROF_BUCKETS)
        histogram[bucket]++;
    histogram_samples++;

    /* Detailed ring buffer: only when explicitly profiling */
    if (!profiling) return;
    if (sample_count < KPROF_MAX_SAMPLES) {
        samples[sample_head].rip = rip;
        samples[sample_head].tick = idt_get_ticks();
        sample_head = (sample_head + 1) % KPROF_MAX_SAMPLES;
        sample_count++;
    }
}

/* ── Report ──────────────────────────────────────────────────── */

void kprof_report(void)
{
    uint64_t total = histogram_samples ? histogram_samples : sample_count;
    serial_puts("[KPROF] Hotspot report (");
    serial_putdec(total);
    serial_puts(" samples):\n");

    /* Find top 10 buckets */
    for (int top = 0; top < 10; top++) {
        uint32_t max_count = 0;
        int max_idx = -1;
        for (int i = 0; i < KPROF_BUCKETS; i++) {
            if (histogram[i] > max_count) {
                max_count = histogram[i];
                max_idx = i;
            }
        }
        if (max_idx < 0 || max_count == 0) break;

        uint64_t addr = (uint64_t)max_idx << KPROF_BUCKET_SHIFT;
        uint32_t pct = (total > 0) ? (uint32_t)(max_count * 100 / total) : 0;

        serial_puts("  0x");
        serial_puthex(addr, 12);
        serial_puts("-0x");
        serial_puthex(addr + (1ULL << KPROF_BUCKET_SHIFT) - 1, 12);
        serial_puts(": ");
        serial_putdec(max_count);
        serial_puts(" (");
        serial_putdec(pct);
        serial_puts("%)\n");

        histogram[max_idx] = 0;  /* Remove from consideration */
    }
}

/* Dump raw samples (for external analysis) */
void kprof_dump_raw(void)
{
    serial_puts("[KPROF] Raw samples (RIP, tick):\n");
    uint32_t count = sample_count < KPROF_MAX_SAMPLES ? sample_count : KPROF_MAX_SAMPLES;
    for (uint32_t i = 0; i < count && i < 100; i++) {
        serial_puts("  0x");
        serial_puthex(samples[i].rip, 16);
        serial_puts(" t=");
        serial_putdec(samples[i].tick);
        serial_puts("\n");
    }
    if (count > 100) {
        serial_puts("  ... (");
        serial_putdec(count - 100);
        serial_puts(" more)\n");
    }
}
