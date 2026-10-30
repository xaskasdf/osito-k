/* Opt-in BSP timer sampling. No allocation or output in the interrupt path. */
#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t value);
extern void serial_puthex(uint64_t value, int digits);
extern uint64_t idt_get_ticks(void);
extern volatile uint32_t *idt_get_apic_base(void);
extern int32_t proc_current_pid(void);
extern void *kmalloc(uint64_t size);
extern void kfree(void *ptr);

#define KPROF_MAX_SAMPLES 4096
#define KPROF_BUCKETS 1024
#define KPROF_PROBES 8
#define KPROF_BUCKET_SHIFT 6
#define KPROF_KERNEL_BASE 0xffff800000000000ULL
#define KPROF_APIC_ISR32 0x110

#ifdef KPROF_HOST_TEST
extern uint64_t kprof_irq_save(void);
extern void kprof_irq_restore(uint64_t flags);
extern uint64_t kprof_read_cr3(void);
#else
static uint64_t kprof_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void kprof_irq_restore(uint64_t flags)
{
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

static uint64_t kprof_read_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
#endif

typedef struct {
    uint64_t rip, tick, cr3;
    uint32_t pid;
} kprof_sample_t;

typedef struct {
    uint64_t address, cr3, count;
} kprof_bucket_t;

/* The timer hook and shell commands run on the BSP. Brief IRQ masks protect
 * snapshots; output and heap operations always run outside those masks. */
static kprof_sample_t *samples;
static kprof_bucket_t histogram[KPROF_BUCKETS];
static uint32_t sample_head, sample_count;
static uint64_t histogram_samples, kernel_samples, histogram_dropped;
static uint64_t overwritten, software_events, same_tick_events;
static uint64_t prof_start_tick, prof_stop_tick, last_sample_tick;
static bool profiling;

static uint32_t kprof_hash(uint64_t address, uint64_t cr3)
{
    uint64_t hash = (address >> KPROF_BUCKET_SHIFT) ^ (cr3 >> 12);
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    return (uint32_t)hash & (KPROF_BUCKETS - 1);
}

void kprof_start(void)
{
    kprof_sample_t *allocated = NULL;
    if (!samples) {
        allocated = kmalloc(KPROF_MAX_SAMPLES * sizeof(*samples));
        if (!allocated) {
            serial_puts("[KPROF] Out of memory; previous capture unchanged\n");
            return;
        }
    }
    uint64_t flags = kprof_irq_save();
    if (!samples) {
        samples = allocated;
        allocated = NULL;
    }
    memset(histogram, 0, sizeof(histogram));
    sample_head = sample_count = 0;
    histogram_samples = kernel_samples = histogram_dropped = 0;
    overwritten = software_events = same_tick_events = 0;
    prof_start_tick = prof_stop_tick = idt_get_ticks();
    last_sample_tick = UINT64_MAX;
    profiling = true;
    kprof_irq_restore(flags);
    if (allocated) kfree(allocated);
    serial_puts("[KPROF] Started: BSP hardware timer, at most 100 samples/s\n");
}

void kprof_stop(void)
{
    uint64_t flags = kprof_irq_save();
    if (profiling) prof_stop_tick = idt_get_ticks();
    profiling = false;
    uint64_t total = histogram_samples;
    uint64_t elapsed = prof_stop_tick - prof_start_tick;
    kprof_irq_restore(flags);
    serial_puts("[KPROF] Stopped: samples=");
    serial_putdec(total);
    serial_puts(" elapsed_ticks=");
    serial_putdec(elapsed);
    serial_puts("\n");
}

bool kprof_is_active(void) { return profiling; }

void kprof_record(uint64_t rip)
{
    if (!profiling) return;
    /* INT 0x20 is also sched_yield. Only a real LAPIC timer interrupt sets
     * ISR bit 32. Check before the handler sends EOI; never count yields. */
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic || !(apic[KPROF_APIC_ISR32 / 4] & 1U)) {
        software_events++;
        return;
    }
    uint64_t tick = idt_get_ticks();
    if (last_sample_tick == tick) {
        same_tick_events++;
        return;
    }
    last_sample_tick = tick;
    uint64_t cr3 = kprof_read_cr3();
    uint64_t address = rip & ~((1ULL << KPROF_BUCKET_SHIFT) - 1);
    bool kernel = rip >= KPROF_KERNEL_BASE;
    uint64_t bucket_cr3 = kernel ? 0 : cr3;
    uint32_t slot = kprof_hash(address, bucket_cr3);
    bool stored = false;
    for (uint32_t probe = 0; probe < KPROF_PROBES; probe++) {
        kprof_bucket_t *bucket = &histogram[(slot + probe) & (KPROF_BUCKETS - 1)];
        if (!bucket->count || (bucket->address == address && bucket->cr3 == bucket_cr3)) {
            bucket->address = address;
            bucket->cr3 = bucket_cr3;
            bucket->count++;
            stored = true;
            break;
        }
    }
    histogram_samples++;
    if (kernel) kernel_samples++;
    if (!stored) histogram_dropped++;

    kprof_sample_t *sample = &samples[sample_head];
    sample->rip = rip;
    sample->tick = tick;
    sample->cr3 = cr3;
    sample->pid = (uint32_t)proc_current_pid();
    sample_head = (sample_head + 1) % KPROF_MAX_SAMPLES;
    if (sample_count < KPROF_MAX_SAMPLES) sample_count++;
    else overwritten++;
}

void kprof_report(void)
{
    kprof_bucket_t *copy = kmalloc(sizeof(histogram));
    if (!copy) {
        serial_puts("[KPROF] Out of memory for snapshot\n");
        return;
    }
    uint64_t flags = kprof_irq_save();
    memcpy(copy, histogram, sizeof(histogram));
    uint64_t total = histogram_samples, kernel = kernel_samples;
    uint64_t dropped = histogram_dropped, soft = software_events, repeated = same_tick_events;
    bool active = profiling;
    uint64_t elapsed = (active ? idt_get_ticks() : prof_stop_tick) - prof_start_tick;
    kprof_irq_restore(flags);

    serial_puts("[KPROF] BSP samples="); serial_putdec(total);
    serial_puts(" kernel="); serial_putdec(kernel);
    serial_puts(" user="); serial_putdec(total - kernel);
    serial_puts(" dropped="); serial_putdec(dropped);
    serial_puts(" elapsed_ticks="); serial_putdec(elapsed);
    serial_puts(active ? " state=active\n" : " state=stopped\n");
    serial_puts("  skipped_software="); serial_putdec(soft);
    serial_puts(" skipped_same_tick="); serial_putdec(repeated);
    serial_puts("\n  Scope: BSP timer samples, not wall time or all-CPU utilization.\n");
    for (unsigned top = 0; top < 20; top++) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < KPROF_BUCKETS; i++)
            if (copy[i].count > copy[best].count) best = i;
        if (!copy[best].count) break;
        serial_puts("  address="); serial_puthex(copy[best].address, 16);
        serial_puts(" cr3="); serial_puthex(copy[best].cr3, 16);
        serial_puts(" samples="); serial_putdec(copy[best].count);
        serial_puts("\n");
        copy[best].count = 0;
    }
    kfree(copy);
}

void kprof_dump_raw(void)
{
    kprof_sample_t *copy = kmalloc(KPROF_MAX_SAMPLES * sizeof(*copy));
    if (!copy) {
        serial_puts("[KPROF] Out of memory for snapshot\n");
        return;
    }
    uint64_t flags = kprof_irq_save();
    uint32_t count = sample_count;
    uint32_t first = (sample_head + KPROF_MAX_SAMPLES - count) % KPROF_MAX_SAMPLES;
    uint32_t tail = KPROF_MAX_SAMPLES - first;
    if (tail > count) tail = count;
    if (count) {
        memcpy(copy, samples + first, tail * sizeof(*copy));
        memcpy(copy + tail, samples, (count - tail) * sizeof(*copy));
    }
    uint64_t lost = overwritten;
    kprof_irq_restore(flags);
    serial_puts("[KPROF] Raw retained="); serial_putdec(count);
    serial_puts(" overwritten="); serial_putdec(lost);
    serial_puts("\n");
    for (uint32_t i = 0; i < count; i++) {
        serial_puts("  rip="); serial_puthex(copy[i].rip, 16);
        serial_puts(" tick="); serial_putdec(copy[i].tick);
        serial_puts(" pid="); serial_putdec(copy[i].pid);
        serial_puts(" cr3="); serial_puthex(copy[i].cr3, 16);
        serial_puts("\n");
    }
    kfree(copy);
}
