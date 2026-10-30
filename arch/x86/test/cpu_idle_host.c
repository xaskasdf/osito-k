#define CPU_IDLE_HOST_TEST
#include "../kernel/cpu_idle.c"

extern int printf(const char *, ...);
extern char *strstr(const char *, const char *);
static unsigned checks, failures, sti_calls, hlt_calls, monitor_calls, wake_calls, msr_calls;
static uint32_t leaf_max, vendor, signature, feature1, feature5, feature6, lines_min, lines_max;
static uint32_t state_counts, ext_max, ext_flags, running_cpu;
static bool monitor_fault, msr_fault, corrupt_msr, skip_ap, change_during_monitor;
static uint64_t ticks, hz;
static cpu_info_t cpus[2];
static char output[8192];
static unsigned out_len;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

void idle_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    switch (leaf) {
    case 0: *a = leaf_max; *b = vendor; *c = 0x6c65746e; *d = 0x49656e69; break;
    case 1: *a = signature; *b = (running_cpu + 4) << 24; *c = feature1; *d = 1U << 5; break;
    case 5: *a = lines_min; *b = lines_max; *c = feature5; *d = state_counts; break;
    case 6: *a = feature6; break;
    case 0x80000000: *a = ext_max; break;
    case 0x80000007: *d = ext_flags; break;
    default: CHECK(false); break;
    }
}
uint64_t idle_tsc(void) { ticks += 10000; return ticks; }
uint64_t idt_get_tsc_freq(void) { return hz; }
void idle_sti(void) { sti_calls++; }
void idle_hlt(void) { hlt_calls++; }
cpu_info_t *smp_cpu_info(uint32_t i) { return i < 2 ? &cpus[i] : NULL; }
uint32_t smp_current_cpu(void) { return running_cpu; }
bool x86_read_msr_safe(uint32_t msr, uint64_t *value)
{
    CHECK(msr == 0xffffffffU);
    msr_calls++;
    if (corrupt_msr) *value = 0;
    return !msr_fault;
}
int x86_mwait_safe(const volatile uint32_t *word, uint32_t expected, uint32_t hint)
{
    CHECK(hint <= 0x20 && !(hint & 15));
    monitor_calls++;
    if (change_during_monitor) return 2;
    if (monitor_fault) return 0;
    return *word == expected ? 1 : 2;
}
void smp_wake_all(void)
{
    wake_calls++;
    cpu_idle_notify(1);
    if (!skip_ap) {
        uint32_t save = running_cpu;
        running_cpu = 1;
        cpu_idle_poll(1);
        running_cpu = save;
    }
}
int sched_sleep_ticks(uint64_t n) { (void)n; ticks += 10000; return -1; }
void serial_puts(const char *s)
{
    while (*s && out_len < sizeof(output) - 1) output[out_len++] = *s++;
    output[out_len] = 0;
}
void serial_putdec(uint64_t n) { (void)n; }
void serial_puthex(uint64_t n, int d) { (void)n; (void)d; }

static void reset(void)
{
    memset(idle_cpus, 0, sizeof(idle_cpus));
    memset(idle_events, 0, sizeof(idle_events));
    memset(cpus, 0, sizeof(cpus));
    use_mwait = probe_epoch = 0;
    sti_calls = hlt_calls = monitor_calls = wake_calls = msr_calls = 0;
    out_len = 0; output[0] = 0;
    leaf_max = 6; vendor = 0x756e6547; signature = 0x906ed;
    feature1 = 8; feature5 = 3; feature6 = 4;
    lines_min = lines_max = 64; state_counts = 0x11142120;
    ext_max = 0x80000007; ext_flags = 1U << 8;
    monitor_fault = corrupt_msr = skip_ap = change_during_monitor = false;
    msr_fault = true;
    ticks = 0; hz = 1000000; running_cpu = 0;
    cpus[0].online = cpus[1].online = true;
    cpus[0].bsp = true; cpus[0].apic_id = 4; cpus[1].apic_id = 5;
}

int main(void)
{
    idle_caps_t s;
    reset(); idle_detect(&s, true);
    CHECK(s.capable && s.bsp && s.hint == 0x20 && s.apic_id == 4);
    CHECK(msr_calls == 0 && monitor_calls == 0);
    leaf_max = 0; idle_detect(&s, true); CHECK(!s.capable);
    leaf_max = 5; idle_detect(&s, true); CHECK(!s.capable);
    leaf_max = 6; vendor = 0; idle_detect(&s, true); CHECK(!s.capable);
    vendor = 0x756e6547; signature = 0xa0671; idle_detect(&s, true); CHECK(!s.capable);
    signature = 0x906ed; feature1 = 0; idle_detect(&s, true); CHECK(!s.capable);
    feature1 = 8;
    for (feature5 = 0; feature5 < 3; feature5++) { idle_detect(&s, true); CHECK(!s.capable); }
    feature5 = 3; lines_min = 0; idle_detect(&s, true); CHECK(!s.capable);
    lines_min = 128; idle_detect(&s, true); CHECK(!s.capable);
    lines_min = 2; idle_detect(&s, true); CHECK(!s.capable);
    lines_min = 63; idle_detect(&s, true); CHECK(!s.capable);
    lines_min = 64; lines_max = 8192; idle_detect(&s, true); CHECK(!s.capable);
    lines_max = 256; idle_detect(&s, true); CHECK(!s.capable);
    lines_max = 65; idle_detect(&s, true); CHECK(!s.capable);
    lines_max = 64; ext_max = 0x80000006; idle_detect(&s, true); CHECK(!s.capable);
    ext_max = 0x80000007; ext_flags = 0; idle_detect(&s, true); CHECK(!s.capable);
    ext_flags = 1U << 8; feature6 = 0; idle_detect(&s, true); CHECK(!s.capable);
    idle_detect(&s, false); CHECK(s.capable);
    feature6 = 4; state_counts = 0xf0000000; idle_detect(&s, true); CHECK(!s.capable);
    state_counts = 0x100; idle_detect(&s, true); CHECK(s.capable && s.hint == 0x10);
    state_counts = 0x10; idle_detect(&s, true); CHECK(s.capable && s.hint == 0);
    state_counts = 1; idle_detect(&s, true); CHECK(!s.capable); /* C0 isn't an idle hint. */

    reset(); cpu_idle_init_cpu(0);
    running_cpu = 1; cpu_idle_init_cpu(1); running_cpu = 0;
    CHECK(idle_cpus[0].ready && idle_cpus[1].ready && !use_mwait);
    CHECK(idle_cpus[0].caps.bsp && !idle_cpus[1].caps.bsp);
    CHECK(sizeof(idle_event_t) == 128 && idle_cpus[0].caps.monitor_bytes == 64);
    CHECK(((uintptr_t)&idle_events[0] & 127) == 0);
    cpu_idle_notify(0); cpu_idle_notify(1); cpu_idle_notify(SMP_MAX_CPUS);
    CHECK(idle_events[0].sequence == 1 && idle_events[1].sequence == 1);
    idle_events[1].sequence = 0xffffffffU;
    cpu_idle_notify(1); CHECK(idle_events[1].sequence == 0);
    volatile uint32_t word = 0;
    cpu_idle_wait(0, &word, 0); CHECK(hlt_calls == 1 && monitor_calls == 0);
    cpu_idle_set(true); CHECK(use_mwait && wake_calls == 1);
    cpu_idle_wait(0, &word, 0); CHECK(sti_calls == 1 && monitor_calls == 1 && hlt_calls == 1);
    word = 1;
    cpu_idle_wait(0, &word, 0); CHECK(sti_calls == 2 && monitor_calls == 1 && hlt_calls == 1);
    word = 0; change_during_monitor = true;
    cpu_idle_wait(0, &word, 0); CHECK(sti_calls == 3 && idle_cpus[0].changed == 1 && hlt_calls == 1);
    change_during_monitor = false; monitor_fault = true;
    cpu_idle_wait(0, &word, 0); CHECK(idle_cpus[0].broken && idle_cpus[0].faults == 1 && hlt_calls == 2);
    unsigned calls = monitor_calls;
    cpu_idle_wait(0, &word, 0); CHECK(hlt_calls == 3 && monitor_calls == calls);
    monitor_fault = false;
    cpu_idle_wait(1, &word, 0); CHECK(idle_cpus[1].mwait_calls == 1 && !idle_cpus[1].broken);
    cpu_idle_set(false); CHECK(!use_mwait && wake_calls == 2);
    cpu_idle_wait(1, &word, 0); CHECK(hlt_calls == 4);
    cpu_idle_wait(SMP_MAX_CPUS, &word, 0); CHECK(hlt_calls == 5);
    cpu_idle_init_cpu(SMP_MAX_CPUS); cpu_idle_poll(SMP_MAX_CPUS);

    reset(); feature1 = 0; cpu_idle_init_cpu(0); cpu_idle_init_cpu(1);
    cpu_idle_set(true); CHECK(!use_mwait && wake_calls == 0);
    CHECK(strstr(output, "unavailable") != NULL);
    cpu_idle_dump(); CHECK(strstr(output, "MONITOR unavailable") != NULL);
    monitor_fault = true;
    cpu_idle_selftest(); CHECK(strstr(output, " PASS") != NULL);
    CHECK(idle_cpus[0].probe_ok && idle_cpus[1].probe_ok && msr_calls == 2);

    reset(); cpu_idle_init_cpu(0); cpu_idle_init_cpu(1);
    cpu_idle_selftest(); CHECK(strstr(output, " PASS") != NULL);
    CHECK(monitor_calls == 2 && msr_calls == 2);
    corrupt_msr = true; out_len = 0;
    cpu_idle_selftest(); CHECK(strstr(output, " FAIL") != NULL);
    corrupt_msr = false; msr_fault = false; out_len = 0;
    cpu_idle_selftest(); CHECK(strstr(output, " FAIL") != NULL);
    msr_fault = true; skip_ap = true; out_len = 0;
    cpu_idle_selftest(); CHECK(strstr(output, " FAIL") != NULL);
    CHECK(ticks >= hz / 2 && ticks < hz);
    hz = 0; out_len = 0; unsigned probes = msr_calls;
    cpu_idle_selftest(); CHECK(strstr(output, "calibrated TSC required") != NULL && probes == msr_calls);
    reset(); cpu_idle_init_cpu(0); /* An online but uninitialized AP cannot be counted as a pass. */
    cpu_idle_selftest(); CHECK(strstr(output, " FAIL") != NULL);
    printf("cpu_idle_host: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
