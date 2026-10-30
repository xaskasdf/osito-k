#define KPROF_HOST_TEST
#include "../kernel/kprof.c"

extern int printf(const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void free(void *);

static unsigned checks, failures, allocations;
static bool irq_enabled = true, fail_alloc, inject_sample, apic_present = true;
static uint64_t ticks, current_cr3 = 0x12345000;
static int32_t current_pid = 42;
static uint32_t fake_apic[0x400 / 4];
static char output[1024 * 1024], previous[1024 * 1024];
static size_t output_size;
#define CHECK(test) do { checks++; if (!(test)) { \
    printf("FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

uint64_t kprof_irq_save(void)
{
    uint64_t flags = irq_enabled ? 0x200 : 0;
    irq_enabled = false;
    return flags;
}
void kprof_irq_restore(uint64_t flags) { irq_enabled = (flags & 0x200) != 0; }
uint64_t kprof_read_cr3(void) { return current_cr3; }
uint64_t idt_get_ticks(void) { return ticks; }
int32_t proc_current_pid(void) { return current_pid; }
volatile uint32_t *idt_get_apic_base(void) { return apic_present ? fake_apic : NULL; }
void *kmalloc(uint64_t size)
{
    CHECK(irq_enabled);
    if (fail_alloc) return NULL;
    void *ptr = malloc(size);
    if (ptr) allocations++;
    return ptr;
}
void kfree(void *ptr)
{
    CHECK(irq_enabled);
    if (ptr) { allocations--; free(ptr); }
}

static void record(uint64_t rip)
{
    ticks++;
    uint64_t flags = kprof_irq_save();
    kprof_record(rip);
    kprof_irq_restore(flags);
}

void serial_puts(const char *text)
{
    CHECK(irq_enabled);
    if (inject_sample) {
        inject_sample = false;
        record(0x140001234ULL);
    }
    size_t size = strlen(text);
    CHECK(output_size + size < sizeof(output));
    if (output_size + size < sizeof(output)) {
        memcpy(output + output_size, text, size + 1);
        output_size += size;
    }
}
void serial_putdec(uint64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%llu", value);
    serial_puts(text);
}
void serial_puthex(uint64_t value, int digits)
{
    char text[32];
    snprintf(text, sizeof(text), "0x%0*llX", digits, value);
    serial_puts(text);
}
static void clear_output(void) { output_size = 0; output[0] = 0; }
static bool contains(const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; i + length <= output_size; i++)
        if (!memcmp(output + i, text, length)) return true;
    return false;
}
static uint64_t bucket_count(uint64_t address, uint64_t cr3)
{
    for (unsigned i = 0; i < KPROF_BUCKETS; i++)
        if (histogram[i].address == address && histogram[i].cr3 == cr3)
            return histogram[i].count;
    return 0;
}

int main(void)
{
    fake_apic[KPROF_APIC_ISR32 / 4] = 1;
    record(KPROF_KERNEL_BASE + 0x2101234);
    CHECK(!histogram_samples && !allocations && !kprof_is_active());
    fail_alloc = true;
    kprof_start();
    CHECK(!kprof_is_active() && !allocations);
    fail_alloc = false;
    kprof_start();
    CHECK(kprof_is_active() && allocations == 1);
    record(KPROF_KERNEL_BASE + 0x2101234);
    record(0x401234);
    record(0x140001234ULL);
    current_cr3 = 0x6789a000; current_pid = 43;
    record(0x401234);
    record(KPROF_KERNEL_BASE + 0x2101235);
    CHECK(histogram_samples == 5 && kernel_samples == 2 && !histogram_dropped);
    CHECK(bucket_count(KPROF_KERNEL_BASE + 0x2101200, 0) == 2);
    CHECK(bucket_count(0x401200, 0x12345000) == 1);
    CHECK(bucket_count(0x401200, 0x6789a000) == 1);
    CHECK(bucket_count(0x140001200ULL, 0x12345000) == 1);
    CHECK(samples[3].pid == 43 && samples[3].cr3 == current_cr3);
    uint64_t flags = kprof_irq_save();
    kprof_record(0x401234);
    kprof_irq_restore(flags);
    CHECK(same_tick_events == 1 && histogram_samples == 5);
    fake_apic[KPROF_APIC_ISR32 / 4] = 0;
    for (unsigned i = 0; i < 1000; i++) record(0x401234);
    fake_apic[KPROF_APIC_ISR32 / 4] = 2;
    record(0x401234);
    apic_present = false;
    record(0x401234);
    apic_present = true;
    fake_apic[KPROF_APIC_ISR32 / 4] = 1;
    CHECK(software_events == 1002 && histogram_samples == 5);

    clear_output();
    inject_sample = true;
    kprof_report();
    CHECK(contains("BSP samples=5 kernel=2 user=3 dropped=0"));
    CHECK(contains("address=0xFFFF800002101200"));
    CHECK(histogram_samples == 6 && sample_count == 6);
    kprof_stop();
    uint64_t stopped_tick = prof_stop_tick;
    record(0x1234);
    kprof_stop();
    CHECK(histogram_samples == 6 && prof_stop_tick == stopped_tick);
    clear_output(); kprof_report(); strcpy(previous, output);
    clear_output(); kprof_report();
    CHECK(!strcmp(previous, output));
    CHECK(bucket_count(KPROF_KERNEL_BASE + 0x2101200, 0) == 2);
    clear_output(); kprof_dump_raw();
    CHECK(contains("Raw retained=6 overwritten=0"));
    CHECK(contains("rip=0x0000000000401234"));
    CHECK(contains("pid=43 cr3=0x000000006789A000"));
    fail_alloc = true;
    clear_output(); kprof_report(); kprof_dump_raw();
    CHECK(contains("Out of memory") && histogram_samples == 6 && sample_count == 6);
    fail_alloc = false;

    kprof_start();
    CHECK(!histogram_samples && !kernel_samples && !histogram_dropped && !sample_count);
    CHECK(!overwritten && !software_events && !same_tick_events);
    uint64_t first_tick = ticks + 1;
    for (unsigned i = 0; i < KPROF_MAX_SAMPLES + 7; i++) record(0x401234);
    CHECK(sample_count == KPROF_MAX_SAMPLES && overwritten == 7 && sample_head == 7);
    CHECK(histogram_samples == KPROF_MAX_SAMPLES + 7 && !histogram_dropped);
    CHECK(samples[7].tick == first_tick + 7);
    clear_output(); inject_sample = true; kprof_dump_raw();
    CHECK(contains("Raw retained=4096 overwritten=7"));
    char expected[128];
    snprintf(expected, sizeof(expected), "\n  rip=0x0000000000401234 tick=%llu pid=43", first_tick + 7);
    CHECK(contains(expected));
    CHECK(overwritten == 8 && sample_head == 8);

    kprof_start();
    uint64_t candidates[KPROF_PROBES + 1];
    unsigned found = 0;
    for (uint64_t address = 0x400000; found < KPROF_PROBES + 1; address += 64) {
        if (kprof_hash(address, current_cr3) == 0) candidates[found++] = address;
    }
    for (unsigned i = 0; i < found; i++) record(candidates[i]);
    CHECK(histogram_samples == KPROF_PROBES + 1 && histogram_dropped == 1);
    CHECK(sample_count == KPROF_PROBES + 1);
    record(candidates[0]);
    CHECK(bucket_count(candidates[0], current_cr3) == 2 && histogram_dropped == 1);
    kprof_stop();
    kfree(samples); samples = NULL;
    CHECK(!allocations && irq_enabled);
    printf("Kernel profiler: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
