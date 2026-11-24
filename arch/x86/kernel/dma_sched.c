/*
 * OsitoK x86-64 — Cooperative DMA Scheduling
 *
 * Coordinates DMA traffic across NVMe, I211 NIC, xHCI USB, and
 * display. Priority: NIC rx (latency) > NVMe tensor (throughput)
 * > USB HID (normal) > display flip (deferrable).
 *
 * Called from sched_tick to make DMA priority decisions.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* ── DMA priority levels ────────────────────────────────────── */

typedef enum {
    DMA_PRIO_REALTIME,   /* NIC rx — latency critical */
    DMA_PRIO_HIGH,       /* NVMe tensor streaming — throughput */
    DMA_PRIO_NORMAL,     /* USB HID polling */
    DMA_PRIO_LOW,        /* Display flip — can wait for vblank */
    DMA_PRIO_COUNT
} dma_priority_t;

/* ── Per-device DMA statistics ──────────────────────────────── */

typedef struct {
    uint64_t bytes_total;        /* lifetime bytes transferred */
    uint64_t bytes_this_tick;    /* bytes since last sched_tick */
    uint64_t stalls;             /* times this device was throttled */
    dma_priority_t priority;
    bool     active;
} dma_device_t;

#define DMA_DEV_NVME  0
#define DMA_DEV_NIC   1
#define DMA_DEV_XHCI  2
#define DMA_DEV_DISP  3
#define DMA_DEV_COUNT 4

static dma_device_t dma_devices[DMA_DEV_COUNT];

/* ── Bandwidth budget per tick (10ms at 100Hz) ──────────────── */

/* PCIe Gen3 x4 ≈ 3.9 GB/s. Per 10ms tick: ~39 MB available.
 * Allocate proportionally based on priority. */
#define DMA_BUDGET_PER_TICK  (39 * 1024 * 1024)  /* 39 MB */

static const uint32_t dma_budget_pct[DMA_PRIO_COUNT] = {
    10,    /* REALTIME: 10% (~3.9 MB) — small packets, low volume */
    60,    /* HIGH: 60% (~23.4 MB) — NVMe tensor streaming */
    20,    /* NORMAL: 20% (~7.8 MB) — USB, misc */
    10,    /* LOW: 10% (~3.9 MB) — display */
};

void dma_sched_init(void)
{
    memset(dma_devices, 0, sizeof(dma_devices));
    dma_devices[DMA_DEV_NIC].priority  = DMA_PRIO_REALTIME;
    dma_devices[DMA_DEV_NVME].priority = DMA_PRIO_HIGH;
    dma_devices[DMA_DEV_XHCI].priority = DMA_PRIO_NORMAL;
    dma_devices[DMA_DEV_DISP].priority = DMA_PRIO_LOW;
}

/* Report DMA bytes from a device (called from driver code) */
void dma_sched_report(int device, uint64_t bytes)
{
    if (device < 0 || device >= DMA_DEV_COUNT) return;
    dma_devices[device].bytes_total += bytes;
    dma_devices[device].bytes_this_tick += bytes;
    dma_devices[device].active = true;
}

/* Check if a device should defer its DMA (called before submission).
 * Returns true if the device should wait until next tick. */
bool dma_sched_should_defer(int device)
{
    if (device < 0 || device >= DMA_DEV_COUNT) return false;
    dma_device_t *d = &dma_devices[device];

    /* Never defer REALTIME */
    if (d->priority == DMA_PRIO_REALTIME) return false;

    /* Check if this device exceeded its budget this tick */
    uint64_t budget = (uint64_t)DMA_BUDGET_PER_TICK *
                      dma_budget_pct[d->priority] / 100;
    if (d->bytes_this_tick > budget) {
        d->stalls++;
        return true;
    }

    return false;
}

/* Reset per-tick counters (called from sched_tick) */
void dma_sched_tick(void)
{
    for (int i = 0; i < DMA_DEV_COUNT; i++)
        dma_devices[i].bytes_this_tick = 0;
}

/* Print DMA stats */
void dma_sched_stats(void)
{
    static const char *names[] = { "NVMe", "NIC", "xHCI", "Display" };
    serial_puts("[DMA] Statistics:\n");
    for (int i = 0; i < DMA_DEV_COUNT; i++) {
        serial_puts("  ");
        serial_puts(names[i]);
        serial_puts(": ");
        serial_putdec(dma_devices[i].bytes_total / (1024 * 1024));
        serial_puts(" MB total, ");
        serial_putdec(dma_devices[i].stalls);
        serial_puts(" stalls\n");
    }
}
