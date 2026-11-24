/*
 * OsitoK x86-64 — PSI (Pressure Stall Information)
 *
 * Tracks resource pressure: CPU, memory, I/O.
 * Reports time spent stalled waiting for resources.
 * Used for autoscaling, OOM prevention, load monitoring.
 * Readable via /proc/pressure/{cpu,memory,io}.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);
extern uint64_t mem_get_free(void);
extern uint64_t mem_get_total(void);

/* ── PSI Metrics ─────────────────────────────────────────────── */

typedef struct {
    uint64_t total_stall_ticks;   /* Total time stalled (ticks) */
    uint64_t window_stall;        /* Stall in current 10s window */
    uint64_t window_start;        /* Window start tick */
    uint32_t avg10;               /* 10-second average (% × 100) */
    uint32_t avg60;               /* 60-second average */
    uint32_t avg300;              /* 5-minute average */
    uint64_t samples;
} psi_resource_t;

static psi_resource_t psi_cpu;
static psi_resource_t psi_memory;
static psi_resource_t psi_io;
static bool psi_initialized;

void psi_init(void)
{
    memset(&psi_cpu, 0, sizeof(psi_resource_t));
    memset(&psi_memory, 0, sizeof(psi_resource_t));
    memset(&psi_io, 0, sizeof(psi_resource_t));
    uint64_t now = idt_get_ticks();
    psi_cpu.window_start = now;
    psi_memory.window_start = now;
    psi_io.window_start = now;
    psi_initialized = true;
    serial_puts("[PSI] Pressure tracking initialized\n");
}

/* ── Record Stall Events ─────────────────────────────────────── */

void psi_record_cpu_stall(uint32_t ticks)
{
    if (!psi_initialized) return;
    psi_cpu.total_stall_ticks += ticks;
    psi_cpu.window_stall += ticks;
    psi_cpu.samples++;
}

void psi_record_memory_stall(uint32_t ticks)
{
    if (!psi_initialized) return;
    psi_memory.total_stall_ticks += ticks;
    psi_memory.window_stall += ticks;
    psi_memory.samples++;
}

void psi_record_io_stall(uint32_t ticks)
{
    if (!psi_initialized) return;
    psi_io.total_stall_ticks += ticks;
    psi_io.window_stall += ticks;
    psi_io.samples++;
}

/* ── Window Update (call periodically) ───────────────────────── */

static void psi_update_window(psi_resource_t *r)
{
    uint64_t now = idt_get_ticks();
    uint64_t elapsed = now - r->window_start;

    if (elapsed >= 1000) {  /* 10 seconds at 100Hz */
        /* Calculate percentage: stall_ticks / elapsed * 10000 */
        uint32_t pct = (elapsed > 0) ? (uint32_t)(r->window_stall * 10000 / elapsed) : 0;

        /* Exponential moving average */
        r->avg10 = pct;
        r->avg60 = (r->avg60 * 5 + pct) / 6;
        r->avg300 = (r->avg300 * 29 + pct) / 30;

        r->window_stall = 0;
        r->window_start = now;
    }
}

void psi_update(void)
{
    if (!psi_initialized) return;
    psi_update_window(&psi_cpu);
    psi_update_window(&psi_memory);
    psi_update_window(&psi_io);
}

/* ── Read Interface (for /proc/pressure/*) ───────────────────── */

static int psi_format(psi_resource_t *r, char *buf, int max)
{
    int p = 0;
    /* Format: "avg10=X.XX avg60=X.XX avg300=X.XX total=N" */
    const char *s = "some avg10=";
    while (*s && p < max - 1) buf[p++] = *s++;

    /* Format percentage (e.g., 1234 → "12.34") */
    uint32_t v = r->avg10;
    char tmp[8]; int n = 0;
    if (v == 0) { buf[p++] = '0'; buf[p++] = '.'; buf[p++] = '0'; buf[p++] = '0'; }
    else {
        uint32_t whole = v / 100, frac = v % 100;
        /* whole part */
        if (whole == 0) buf[p++] = '0';
        else { while (whole) { tmp[n++] = '0' + whole % 10; whole /= 10; }
               for (int i = n - 1; i >= 0; i--) buf[p++] = tmp[i]; }
        buf[p++] = '.';
        buf[p++] = '0' + (frac / 10);
        buf[p++] = '0' + (frac % 10);
    }

    s = " avg60=0.00 avg300=0.00 total=";
    while (*s && p < max - 1) buf[p++] = *s++;

    /* total stall microseconds */
    uint64_t total_us = r->total_stall_ticks * 10000;
    n = 0;
    if (total_us == 0) buf[p++] = '0';
    else {
        while (total_us) { tmp[n++] = '0' + total_us % 10; total_us /= 10; }
        for (int i = n - 1; i >= 0 && p < max - 1; i--) buf[p++] = tmp[i];
    }
    if (p < max - 1) buf[p++] = '\n';
    buf[p] = '\0';
    return p;
}

int psi_read_cpu(char *buf, int max) { return psi_format(&psi_cpu, buf, max); }
int psi_read_memory(char *buf, int max) { return psi_format(&psi_memory, buf, max); }
int psi_read_io(char *buf, int max) { return psi_format(&psi_io, buf, max); }
