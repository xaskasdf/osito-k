#include "cpu_idle.h"
#include "smp.h"
#include "smp_work.h"
#include "../include/msr_safe.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t n);
extern void serial_puthex(uint64_t n, int digits);
extern uint64_t idt_get_tsc_freq(void);
extern int sched_sleep_ticks(uint64_t ticks);

#ifdef CPU_IDLE_HOST_TEST
extern void idle_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                       uint32_t *c, uint32_t *d);
extern uint64_t idle_tsc(void);
extern void idle_sti(void);
extern void idle_hlt(void);
#else
static void idle_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                       uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}
static uint64_t idle_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static void idle_sti(void) { __asm__ volatile ("sti" ::: "memory"); }
static void idle_hlt(void) { __asm__ volatile ("sti; hlt" ::: "memory"); }
#endif

typedef struct {
    uint32_t signature, apic_id, hint, substates, monitor_bytes;
    bool monitor, msr, capable, bsp;
    const char *reason;
} idle_caps_t;

typedef struct __attribute__((aligned(128))) {
    idle_caps_t caps;  /* Immutable after ready is published. */
    uint32_t ready, broken, probe_epoch, probe_ok;
    uint64_t hlt_calls, mwait_calls, changed, faults;
} idle_cpu_t;

static idle_cpu_t idle_cpus[SMP_MAX_CPUS];
/* Nothing else may share the hardware MONITOR range, including idle stats. */
typedef struct __attribute__((aligned(128))) { uint32_t sequence; } idle_event_t;
static idle_event_t idle_events[SMP_MAX_CPUS];
static uint32_t use_mwait;  /* Opt-in for now; HLT remains the boot default. */
static uint32_t probe_epoch;

static void idle_detect(idle_caps_t *s, bool bsp)
{
    memset(s, 0, sizeof(*s));
    s->bsp = bsp;
    s->reason = "unsupported CPU profile";
    uint32_t a, b, c, d;
    idle_cpuid(0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    bool intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
    if (max_leaf < 1) return;
    idle_cpuid(1, &a, &b, &c, &d);
    s->signature = a;
    s->apic_id = b >> 24;
    s->monitor = (c & (1U << 3)) != 0;
    s->msr = (d & (1U << 5)) != 0;
    if (!intel || max_leaf < 6) return;
    uint32_t model = ((a >> 4) & 15) | ((a >> 12) & 0xf0);
    if (((a >> 8) & 15) != 6 || (model != 0x4e && model != 0x5e &&
                                model != 0x8e && model != 0x9e)) return;
    s->reason = "MONITOR unavailable";
    if (!s->monitor) return;
    idle_cpuid(5, &a, &b, &c, &d);
    s->substates = d;
    s->reason = "MWAIT extensions/IRQ break unavailable";
    if ((c & 3) != 3) return;
    s->reason = "invalid MONITOR line sizes";
    a &= 0xffff;
    b &= 0xffff;
    if (a < sizeof(uint32_t) || a > b || b > sizeof(idle_event_t) ||
        (a & (a - 1)) || (b & (b - 1))) return;
    s->monitor_bytes = b;
    s->reason = "invariant TSC unavailable";
    idle_cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000007) return;
    idle_cpuid(0x80000007, &a, &b, &c, &d);
    if (!(d & (1U << 8))) return;
    /* No broadcast timer backend exists. Never stop the BSP's local timer. */
    idle_cpuid(6, &a, &b, &c, &d);
    s->reason = "BSP needs ARAT for its local timer";
    if (bsp && !(a & (1U << 2))) return;
    s->reason = "no advertised idle substate";
    /* Native Intel client profile, capped at hint 0x20. These are processor
     * hints, not ACPI _CST indices. Do not change firmware package limits. */
    for (int cls = 2; cls >= 0; cls--) {
        if ((s->substates >> (4 * (cls + 1))) & 15) {
            s->hint = (uint32_t)cls << 4;
            s->capable = true;
            s->reason = "ready";
            return;
        }
    }
}

void cpu_idle_init_cpu(uint32_t cpu)
{
    if (cpu >= SMP_MAX_CPUS) return;
    cpu_info_t *info = smp_cpu_info(cpu);
    idle_detect(&idle_cpus[cpu].caps, info && info->bsp);
    if (info) idle_cpus[cpu].caps.apic_id = info->apic_id;
    __atomic_store_n(&idle_cpus[cpu].ready, 1, __ATOMIC_RELEASE);
}

void cpu_idle_notify(uint32_t cpu)
{
    if (cpu < SMP_MAX_CPUS)
        __atomic_add_fetch(&idle_events[cpu].sequence, 1, __ATOMIC_RELEASE);
}

void cpu_idle_poll(uint32_t cpu)
{
    if (cpu >= SMP_MAX_CPUS) return;
    idle_cpu_t *s = &idle_cpus[cpu];
    uint32_t epoch = __atomic_load_n(&probe_epoch, __ATOMIC_ACQUIRE);
    if (epoch == __atomic_load_n(&s->probe_epoch, __ATOMIC_RELAXED)) return;
    uint64_t value = 0x123456789abcdef0ULL;
    bool ok = smp_current_cpu() == cpu;
    if (s->caps.msr)
        ok = ok && !x86_read_msr_safe(0xffffffffU, &value) && value == 0x123456789abcdef0ULL;
    /* A deliberately mismatched word tests MONITOR/fault recovery without
     * entering MWAIT. This runs on each owning CPU, never via work stealing. */
    volatile uint32_t word = 1;
    int result = x86_mwait_safe(&word, 0, 0);
    ok = ok && (result == 2 || (!s->caps.monitor && result == 0));
    __atomic_store_n(&s->probe_ok, ok, __ATOMIC_RELAXED);
    __atomic_store_n(&s->probe_epoch, epoch, __ATOMIC_RELEASE);
}

void cpu_idle_wait(uint32_t cpu, const volatile uint32_t *word, uint32_t expected)
{
    idle_cpu_t *s = cpu < SMP_MAX_CPUS ? &idle_cpus[cpu] : NULL;
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) {
        idle_sti();
        return;
    }
    if (s && __atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) && s->caps.capable &&
        __atomic_load_n(&use_mwait, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s->broken, __ATOMIC_RELAXED)) {
        uint32_t sequence = __atomic_load_n(&idle_events[cpu].sequence, __ATOMIC_ACQUIRE);
        __atomic_store_n(&s->mwait_calls, s->mwait_calls + 1, __ATOMIC_RELAXED);
        /* The existing CLI/work recheck + IPI contract covers an event that
         * preceded this snapshot. Arm/recheck covers stores after it. Do not
         * monitor queue/statistics fields that other CPUs may share. */
        int result = x86_mwait_safe(&idle_events[cpu].sequence, sequence, s->caps.hint);
        if (result) {
            if (result == 2) __atomic_store_n(&s->changed, s->changed + 1, __ATOMIC_RELAXED);
            idle_sti();
            return;
        }
        __atomic_store_n(&s->faults, s->faults + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s->broken, 1, __ATOMIC_RELEASE);
    }
    /* A failed optional instruction must not become a retry/fault loop. */
    if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) {
        idle_sti();
        return;
    }
    if (s) __atomic_store_n(&s->hlt_calls, s->hlt_calls + 1, __ATOMIC_RELAXED);
    idle_hlt();
}

void cpu_idle_set(bool mwait)
{
    unsigned capable = 0;
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++) {
        idle_cpu_t *s = &idle_cpus[i];
        if (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) && s->caps.capable &&
            !__atomic_load_n(&s->broken, __ATOMIC_ACQUIRE)) capable++;
    }
    if (mwait && !capable) {
        serial_puts("CPU idle: MWAIT unavailable; policy unchanged\n");
        return;
    }
    __atomic_store_n(&use_mwait, mwait, __ATOMIC_RELEASE);
    smp_wake_all();
    serial_puts(mwait ? "CPU idle: MWAIT selected; unsupported CPUs use HLT\n" :
                       "CPU idle: HLT selected\n");
}

void cpu_idle_dump(void)
{
    serial_puts(__atomic_load_n(&use_mwait, __ATOMIC_ACQUIRE) ?
                "CPU idle policy: MWAIT (HLT fallback)\n" : "CPU idle policy: HLT\n");
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++) {
        idle_cpu_t *s = &idle_cpus[i];
        if (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)) continue;
        serial_puts("  CPU "); serial_putdec(i);
        serial_puts(" APIC "); serial_putdec(s->caps.apic_id);
        serial_puts(s->caps.bsp ? " BSP: " : " AP: ");
        serial_puts(__atomic_load_n(&s->broken, __ATOMIC_ACQUIRE) ? "fault-disabled" : s->caps.reason);
        serial_puts(" hint="); serial_puthex(s->caps.hint, 2);
        serial_puts(" substates="); serial_puthex(s->caps.substates, 8);
        serial_puts(" monitor-bytes="); serial_putdec(s->caps.monitor_bytes);
        serial_puts(" HLT/MWAIT/changed/faults=");
        serial_putdec(__atomic_load_n(&s->hlt_calls, __ATOMIC_RELAXED)); serial_puts("/");
        serial_putdec(__atomic_load_n(&s->mwait_calls, __ATOMIC_RELAXED)); serial_puts("/");
        serial_putdec(__atomic_load_n(&s->changed, __ATOMIC_RELAXED)); serial_puts("/");
        serial_putdec(__atomic_load_n(&s->faults, __ATOMIC_RELAXED)); serial_puts("\n");
    }
    serial_puts("Counts are wait attempts, not residency. Firmware limits and HWP unchanged.\n");
}

void cpu_idle_selftest(void)
{
    uint64_t hz = idt_get_tsc_freq();
    if (hz < 1000000 || hz > 10000000000ULL) {
        serial_puts("CPU idle selftest: calibrated TSC required\n");
        return;
    }
    uint32_t epoch = __atomic_add_fetch(&probe_epoch, 1, __ATOMIC_ACQ_REL);
    cpu_idle_poll(smp_current_cpu());
    smp_wake_all();
    uint64_t start = idle_tsc();
    unsigned total = 0, done = 0, passed = 0;
    do {
        total = done = passed = 0;
        for (unsigned i = 0; i < SMP_MAX_CPUS; i++) {
            idle_cpu_t *s = &idle_cpus[i];
            cpu_info_t *info = smp_cpu_info(i);
            if (!info || !info->online) continue;
            total++;
            if (__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&s->probe_epoch, __ATOMIC_ACQUIRE) == epoch) {
                done++;
                if (__atomic_load_n(&s->probe_ok, __ATOMIC_RELAXED)) passed++;
            }
        }
        if (done == total) break;
        if (sched_sleep_ticks(1) < 0) __asm__ volatile ("pause");
    } while (idle_tsc() - start < hz / 2);
    serial_puts("CPU idle selftest: CPUs/completed/passed=");
    serial_putdec(total); serial_puts("/"); serial_putdec(done); serial_puts("/");
    serial_putdec(passed);
    serial_puts(total && done == total && passed == total ? " PASS\n" : " FAIL\n");
}
