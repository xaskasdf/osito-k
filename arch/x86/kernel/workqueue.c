/*
 * OsitoK x86-64 — Workqueue Subsystem
 *
 * Deferred work execution for interrupt bottom halves,
 * async cleanup, and background processing.
 * Work items queued from IRQ context, executed by worker thread.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern uint64_t idt_get_ticks(void);

/* ── Work Item ───────────────────────────────────────────────── */

#define WQ_MAX_ITEMS 64

typedef struct {
    bool     pending;
    void   (*func)(void *data);
    void    *data;
    uint64_t submit_tick;
    char     name[16];
} work_item_t;

static work_item_t work_items[WQ_MAX_ITEMS];
static uint32_t wq_head, wq_tail, wq_count;
static bool     wq_initialized;

/* ── Init ────────────────────────────────────────────────────── */

void workqueue_init(void)
{
    memset(work_items, 0, sizeof(work_items));
    wq_head = wq_tail = wq_count = 0;
    wq_initialized = true;
    serial_puts("[WQ] Workqueue initialized\n");
}

/* ── Submit Work (safe to call from IRQ context) ─────────────── */

int workqueue_submit(void (*func)(void *), void *data, const char *name)
{
    if (!wq_initialized || wq_count >= WQ_MAX_ITEMS) return -1;

    work_item_t *w = &work_items[wq_head];
    w->pending = true;
    w->func = func;
    w->data = data;
    w->submit_tick = idt_get_ticks();
    int i = 0;
    if (name) while (name[i] && i < 15) { w->name[i] = name[i]; i++; }
    w->name[i] = '\0';

    wq_head = (wq_head + 1) % WQ_MAX_ITEMS;
    wq_count++;
    return 0;
}

/* ── Process Work (called from main loop or worker thread) ──── */

int workqueue_process(void)
{
    if (!wq_initialized) return 0;
    int processed = 0;

    while (wq_count > 0) {
        work_item_t *w = &work_items[wq_tail];
        if (w->pending && w->func) {
            w->func(w->data);
            processed++;
        }
        w->pending = false;
        wq_tail = (wq_tail + 1) % WQ_MAX_ITEMS;
        wq_count--;
    }
    return processed;
}

/* Submit delayed work (execute after delay_ticks) */
typedef struct {
    void   (*func)(void *);
    void    *data;
    uint64_t fire_tick;
    bool     active;
} delayed_work_t;

#define DW_MAX 16
static delayed_work_t delayed_work[DW_MAX];

int workqueue_submit_delayed(void (*func)(void *), void *data,
                             uint32_t delay_ticks)
{
    for (int i = 0; i < DW_MAX; i++) {
        if (!delayed_work[i].active) {
            delayed_work[i].func = func;
            delayed_work[i].data = data;
            delayed_work[i].fire_tick = idt_get_ticks() + delay_ticks;
            delayed_work[i].active = true;
            return i;
        }
    }
    return -1;
}

/* Check and fire delayed work items */
void workqueue_process_delayed(void)
{
    uint64_t now = idt_get_ticks();
    for (int i = 0; i < DW_MAX; i++) {
        if (delayed_work[i].active && now >= delayed_work[i].fire_tick) {
            workqueue_submit(delayed_work[i].func, delayed_work[i].data, "delayed");
            delayed_work[i].active = false;
        }
    }
}

uint32_t workqueue_pending(void) { return wq_count; }

void workqueue_stats(void)
{
    serial_puts("[WQ] Pending: ");
    serial_putdec(wq_count);
    int dw_count = 0;
    for (int i = 0; i < DW_MAX; i++) if (delayed_work[i].active) dw_count++;
    serial_puts(", delayed: ");
    serial_putdec((uint64_t)dw_count);
    serial_puts("\n");
}
