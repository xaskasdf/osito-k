/* Opt-in, bounded AP observer for BSP timer/scheduler stalls. */
#include "lockup.h"
#include "smp.h"
#include "smp_work.h"
#include "screen_capture.h"

extern void sh_puts(const char *);
extern uint64_t idt_get_ticks(void);
extern uint64_t idt_get_tsc_freq(void);
extern uint64_t sched_get_switches(void);
extern int sched_sleep_ticks(uint64_t);
extern uint32_t smp_get_bsp_apic_id(void);
extern volatile uint32_t *idt_get_apic_base(void);
extern int smp_submit_ff(smp_work_func_t, void *, void *) __attribute__((weak));
extern const uint8_t gui_font8x16[95][16];

#define APIC_ICR_LOW  0x300
#define APIC_ICR_HIGH 0x310
#define APIC_SEND_PENDING (1U << 12)
#define APIC_LEVEL_ASSERT (1U << 14)
#define APIC_DELIVERY_NMI (4U << 8)

volatile uint32_t lockup_nmi_pending, lockup_nmi_captured;
volatile x86_interrupt_frame_t lockup_nmi_frame __attribute__((aligned(64)));
volatile uint64_t lockup_nmi_cr2, lockup_nmi_cr3;
volatile uint64_t lockup_nmi_cr0, lockup_nmi_cr4;

enum { WD_OFF, WD_STARTING, WD_ARMED, WD_CAPTURED, WD_EXPIRED, WD_FAILED };
enum { WHY_NONE, WHY_SAMPLE, WHY_TIMER, WHY_SCHED, WHY_NO_NMI };
static volatile uint32_t running, stop_requested, sample_requested, state, reason;
static uint64_t observer_hz, stall_cycles;
static volatile uint64_t observed_ticks, observed_switches;
static screen_view_t screen;

#ifdef LOCKUP_HOST_TEST
extern uint64_t lockup_tsc(void);
extern bool lockup_send_nmi(void);
#else
static uint64_t lockup_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static bool lockup_send_nmi(void)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return false;
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    unsigned left = 100000;
    while ((apic[APIC_ICR_LOW / 4] & APIC_SEND_PENDING) && --left)
        __asm__ volatile ("pause");
    if (left) {
        apic[APIC_ICR_HIGH / 4] = smp_get_bsp_apic_id() << 24;
        apic[APIC_ICR_LOW / 4] = APIC_DELIVERY_NMI | APIC_LEVEL_ASSERT;
    }
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return left != 0;
}
#endif

static const char *reason_name(uint32_t value)
{
    switch (value) {
    case WHY_SAMPLE: return "requested sample";
    case WHY_TIMER: return "BSP timer stopped";
    case WHY_SCHED: return "BSP scheduling stopped";
    case WHY_NO_NMI: return "BSP NMI did not respond";
    default: return "none";
    }
}

static void hex_line(char *line, const char *name, uint64_t value)
{
    unsigned n = 0;
    while (*name && n < 40) line[n++] = *name++;
    line[n++] = '=';
    for (int shift = 60; shift >= 0; shift -= 4)
        line[n++] = "0123456789ABCDEF"[(value >> shift) & 15];
    line[n++] = '\n';
    line[n] = 0;
}

/* Bypass terminal/compositor/log locks. The GOP mapping is permanent; bounds
 * and scanout availability were checked before arming, on the normal shell. */
static void draw_line(unsigned row, const char *text)
{
    volatile uint32_t *pixels = (volatile uint32_t *)screen.pixels;
    unsigned scale = screen.width >= 1280 && screen.height >= 1024 ? 2 : 1;
    unsigned top = 8 + row * 16 * scale;
    if (!pixels || top + 16 * scale > screen.height) return;
    for (unsigned col = 0; text[col] && text[col] != '\n'; col++) {
        unsigned left = 8 + col * 8 * scale;
        if (left + 8 * scale > screen.width) break;
        unsigned c = (unsigned char)text[col];
        if (c < 32 || c > 126) c = '?';
        for (unsigned y = 0; y < 16 * scale; y++) {
            uint8_t bits = gui_font8x16[c - 32][y / scale];
            for (unsigned x = 0; x < 8 * scale; x++)
                pixels[(uint64_t)(top + y) * screen.pitch + left + x] =
                    bits & (0x80 >> (x / scale)) ? 0x00ffffff : 0;
        }
    }
    __asm__ volatile ("sfence" ::: "memory");
}

static void report(bool visible)
{
    char line[80];
    unsigned row = 0;
    if (visible && screen.pixels) {
        unsigned scale = screen.width >= 1280 && screen.height >= 1024 ? 2 : 1;
        unsigned width = 50 * 8 * scale + 16, height = 30 * 16 * scale + 16;
        if (width > screen.width) width = screen.width;
        if (height > screen.height) height = screen.height;
        volatile uint32_t *pixels = (volatile uint32_t *)screen.pixels;
        for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++)
                pixels[(uint64_t)y * screen.pitch + x] = 0;
    }
    x86_interrupt_frame_t frame;
    uint64_t cr2, cr3, cr0, cr4;
    uint32_t generation = __atomic_load_n(&lockup_nmi_captured, __ATOMIC_ACQUIRE);
    bool ready = generation && !__atomic_load_n(&lockup_nmi_pending, __ATOMIC_ACQUIRE);
    frame = lockup_nmi_frame;
    cr2 = lockup_nmi_cr2;
    cr3 = lockup_nmi_cr3;
    cr0 = lockup_nmi_cr0;
    cr4 = lockup_nmi_cr4;
    ready = ready && !__atomic_load_n(&lockup_nmi_pending, __ATOMIC_ACQUIRE) &&
            generation == __atomic_load_n(&lockup_nmi_captured, __ATOMIC_ACQUIRE);
#define LINE(s) do { if (visible) draw_line(row++, (s)); else sh_puts(s); } while (0)
#define VALUE(n, v) do { hex_line(line, (n), (v)); LINE(line); } while (0)
    LINE("LOCKUP DIAGNOSTIC (registers in hex)\n");
    LINE(reason_name(__atomic_load_n(&reason, __ATOMIC_ACQUIRE)));
    if (!visible) sh_puts("\n");
    VALUE("state", __atomic_load_n(&state, __ATOMIC_ACQUIRE));
    VALUE("observer_running", __atomic_load_n(&running, __ATOMIC_ACQUIRE));
    VALUE("ticks", observed_ticks);
    VALUE("switches", observed_switches);
    if (!ready || __atomic_load_n(&reason, __ATOMIC_ACQUIRE) == WHY_NO_NMI) {
        LINE("No completed NMI snapshot.\n");
        return;
    }
    VALUE("RIP", frame.rip);
    VALUE("CS", frame.cs);
    VALUE("RSP", frame.rsp);
    VALUE("FLAGS", frame.rflags);
    VALUE("CR3", cr3);
    VALUE("CR2", cr2);
    VALUE("CR0", cr0);
    VALUE("CR4", cr4);
    VALUE("RAX", frame.rax);
    VALUE("RBX", frame.rbx);
    VALUE("RCX", frame.rcx);
    VALUE("RDX", frame.rdx);
    VALUE("RSI", frame.rsi);
    VALUE("RDI", frame.rdi);
    VALUE("RBP", frame.rbp);
    VALUE("R8", frame.r8);
    VALUE("R9", frame.r9);
    VALUE("R10", frame.r10);
    VALUE("R11", frame.r11);
    VALUE("R12", frame.r12);
    VALUE("R13", frame.r13);
    VALUE("R14", frame.r14);
    VALUE("R15", frame.r15);
#undef VALUE
#undef LINE
}

static bool capture(void)
{
    uint32_t generation = __atomic_load_n(&lockup_nmi_captured, __ATOMIC_ACQUIRE);
    __atomic_store_n(&lockup_nmi_pending, 1, __ATOMIC_RELEASE);
    if (!lockup_send_nmi()) {
        __atomic_store_n(&lockup_nmi_pending, 0, __ATOMIC_RELEASE);
        return false;
    }
    uint64_t start = lockup_tsc();
    while (__atomic_load_n(&lockup_nmi_pending, __ATOMIC_ACQUIRE)) {
        if (lockup_tsc() - start >= observer_hz * 2) return false;
        __asm__ volatile ("pause");
    }
    return __atomic_load_n(&lockup_nmi_captured, __ATOMIC_ACQUIRE) != generation;
}

static void observe(void *arg, void *result)
{
    (void)arg;
    (void)result;
    if (!smp_current_cpu()) {
        __atomic_store_n(&state, WD_FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        return;
    }
    uint64_t start = lockup_tsc(), last_tick = start, last_switch = start;
    observed_ticks = idt_get_ticks();
    observed_switches = sched_get_switches();
    __atomic_store_n(&state, WD_ARMED, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
        uint64_t now = lockup_tsc();
        uint64_t ticks = idt_get_ticks(), switches = sched_get_switches();
        if (ticks != observed_ticks) { observed_ticks = ticks; last_tick = now; }
        if (switches != observed_switches) {
            observed_switches = switches;
            last_switch = now;
        }
        if (now - start >= observer_hz * 300) {
            __atomic_store_n(&state, WD_EXPIRED, __ATOMIC_RELEASE);
            break;
        }
        uint32_t why = now - last_tick >= stall_cycles ? WHY_TIMER :
                       now - last_switch >= stall_cycles ? WHY_SCHED : WHY_NONE;
        if (!why && __atomic_exchange_n(&sample_requested, 0, __ATOMIC_ACQ_REL))
            why = WHY_SAMPLE;
        if (why) {
            __atomic_store_n(&reason, why, __ATOMIC_RELEASE);
            bool ok = capture();
            if (!ok) __atomic_store_n(&reason, WHY_NO_NMI, __ATOMIC_RELEASE);
            if (why != WHY_SAMPLE || !ok) {
                __atomic_store_n(&state, ok ? WD_CAPTURED : WD_FAILED, __ATOMIC_RELEASE);
                report(true);
                break;
            }
            last_tick = last_switch = lockup_tsc();
        }
        __asm__ volatile ("pause");
    }
    if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE))
        __atomic_store_n(&state, WD_OFF, __ATOMIC_RELEASE);
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
}

static bool arm(unsigned seconds)
{
    uint64_t hz = idt_get_tsc_freq();
    if (seconds < 2 || seconds > 60 || hz < 1000000 || hz > 10000000000ULL ||
        smp_cpu_count() < 2 || !smp_submit_ff || !idt_get_apic_base() ||
        __atomic_load_n(&running, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&lockup_nmi_pending, __ATOMIC_ACQUIRE)) return false;
    screen_view_t view;
    if (screen_capture_gop.view(&view) != SCREEN_OK || !view.pixels ||
        !view.width || !view.height || view.width > view.pitch ||
        view.width > SCREEN_MAX_DIMENSION || view.height > SCREEN_MAX_DIMENSION ||
        (uint64_t)view.pitch * view.height * 4 > view.bytes) return false;
    screen = view;
    observer_hz = hz;
    stall_cycles = hz * seconds;
    stop_requested = sample_requested = lockup_nmi_captured = 0;
    reason = WHY_NONE;
    __atomic_store_n(&state, WD_STARTING, __ATOMIC_RELEASE);
    __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
    if (smp_submit_ff(observe, NULL, NULL) < 0) {
        __atomic_store_n(&state, WD_FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        return false;
    }
    uint64_t start = lockup_tsc();
    while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == WD_STARTING) {
        if (lockup_tsc() - start >= hz * 2) {
            __atomic_store_n(&stop_requested, 1, __ATOMIC_RELEASE);
            return false;
        }
        sched_sleep_ticks(1);
    }
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == WD_ARMED;
}

void lockup_command(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "status"))) {
        report(false);
    } else if ((argc == 2 || argc == 3) && !strcmp(argv[1], "arm")) {
        unsigned seconds = 10;
        if (argc == 3) {
            seconds = 0;
            for (const char *p = argv[2]; *p; p++) {
                if (*p < '0' || *p > '9' || seconds > 60) { seconds = 0; break; }
                seconds = seconds * 10 + (unsigned)(*p - '0');
            }
        }
        sh_puts(arm(seconds) ? "lockup: armed; one AP reserved, expires in 300s\n" :
                "lockup: cannot arm (busy, AP/GOP/TSC unavailable, or invalid 2..60 seconds)\n");
    } else if (argc == 2 && !strcmp(argv[1], "off")) {
        __atomic_store_n(&stop_requested, 1, __ATOMIC_RELEASE);
        sh_puts("lockup: stop requested\n");
    } else if (argc == 2 && !strcmp(argv[1], "sample")) {
        if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == WD_ARMED) {
            __atomic_store_n(&sample_requested, 1, __ATOMIC_RELEASE);
            sh_puts("lockup: NMI sample requested\n");
        } else sh_puts("lockup: not armed\n");
    } else sh_puts("Usage: lockup status | lockup arm [seconds] | lockup sample | lockup off\n");
}
