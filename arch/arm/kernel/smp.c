/*
 * smp.c -- Multi-core SMP boot for AArch64 via PSCI
 *
 * Starts the secondary cores (APs) with PSCI CPU_ON. Each AP turns on its
 * MMU and caches with the BSP's page tables, reports its MIDR/MPIDR and waits
 * in wfe, interrupts masked, until sched_start releases it: then it sets up
 * its GIC redistributor and timer and joins the scheduler (sched.c). Before
 * a kload handoff they are turned off again with CPU_OFF (smp_stop_aps).
 *
 * QEMU virt: PSCI over HVC, -smp N cores numbered in MPIDR Aff0.
 * SM8350: PSCI over SMC, 8 cores (4x A55, 3x A78, 1x X1) with MPIDR.MT set,
 * so the core number is Aff1 (0x81000000, 0x81000100, ... 0x81000700).
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "../include/types.h"
#include "task.h"

#define AP_STACK_SIZE   8192

#define PSCI_CPU_OFF        0x84000002ULL
#define PSCI_CPU_ON         0xC4000003ULL   /* SMC64 */
#define PSCI_AFFINITY_INFO  0xC4000004ULL   /* SMC64 */
#define AFFINITY_OFF        1

/* What an AP needs before its MMU is on, read with the caches off: the
 * BSP cleans it to the point of coherency before CPU_ON. Field offsets are
 * used by ap_entry below. */
struct ap_boot {
    uint64_t mair, tcr;         /* 0, 8 */
    uint64_t ttbr0, ttbr1;      /* 16, 24 */
    uint64_t cpacr, sctlr;      /* 32, 40 */
    uint64_t stack, cpu;        /* 48, 56 */
} __attribute__((aligned(64)));

_Static_assert(sizeof(struct ap_boot) == 64, "ap_entry offsets");

/* Per-CPU state, written by each AP once its caches are on */
struct cpu_info {
    uint64_t mpidr, midr;
    volatile int online;
    volatile int scheduling;        /* in the scheduler: stop it with SGI_STOP */
    int started;                    /* CPU_ON accepted (BSP-owned) */
};

static struct ap_boot  ap_boot[MAX_CPUS];
static struct cpu_info cpus[MAX_CPUS];
static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));
static int cpu_count = 1;           /* BSP + online APs */
static int psci_smc;                /* conduit: SMC on SM8350, HVC on virt */
static volatile int stop_req;       /* parked APs: turn off */
static volatile int sched_go;       /* parked APs: join the scheduler */
volatile int smp_running;           /* APs schedule: spinlocks are live */

/* ── PSCI ───────────────────────────────────────────────── */

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    /* SMCCC 1.0 may clobber x4-x17 */
    if (psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                         "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                         "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
                         "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                         "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return (int64_t)x0;
}

/* MPIDR of core n, in the BSP's cluster. With MT set (SM8350) Aff0 is the
 * thread and the core is Aff1. */
static uint64_t core_mpidr(uint64_t bsp, int n)
{
    uint64_t aff = bsp & 0xFF00FF0000ULL;           /* Aff3, Aff2 */
    if (bsp & (1ULL << 24))
        return aff | (bsp & 0xFF) | ((uint64_t)n << 8);
    return aff | ((bsp & 0xFF00) | (uint64_t)n);
}

static int core_number(uint64_t mpidr)
{
    return (mpidr & (1ULL << 24)) ? (mpidr >> 8) & 0xFF : mpidr & 0xFF;
}

int smp_this_core(void)
{
    return core_number(read_mpidr_el1());
}

/* ── AP side ────────────────────────────────────────────── */

/* PSCI CPU_ON entry: EL1, MMU and caches off, interrupts masked,
 * x0 = context_id = this AP's struct ap_boot. Loads the BSP's translation
 * regime and vectors, turns the MMU on (the map is identity, so the next
 * instruction is still valid) and calls ap_main(boot) on the AP's stack,
 * as SP_EL1 (the entry may select SP_EL0). */
__asm__(
    ".pushsection .text\n"
    ".balign 4\n"
    ".globl ap_entry\n"
    ".type ap_entry, %function\n"
    "ap_entry:\n"
    "    msr   daifset, #0xf\n"
    "    ldp   x1, x2, [x0, #0]\n"
    "    msr   mair_el1, x1\n"
    "    msr   tcr_el1, x2\n"
    "    ldp   x1, x2, [x0, #16]\n"
    "    msr   ttbr0_el1, x1\n"
    "    msr   ttbr1_el1, x2\n"
    "    ldr   x1, [x0, #32]\n"
    "    msr   cpacr_el1, x1\n"
    "    adrp  x1, _vectors\n"
    "    add   x1, x1, :lo12:_vectors\n"
    "    msr   vbar_el1, x1\n"
    "    isb\n"
    "    tlbi  vmalle1\n"
    "    ic    iallu\n"
    "    dsb   nsh\n"
    "    isb\n"
    "    ldp   x1, x2, [x0, #40]\n"
    "    msr   sctlr_el1, x1\n"
    "    isb\n"
    /* PSCI doesn't promise SP_EL1: exceptions always use it, so run on it */
    "    msr   spsel, #1\n"
    "    mov   sp, x2\n"
    "    bl    ap_main\n"
    "1:  wfe\n"
    "    b     1b\n"
    ".size ap_entry, . - ap_entry\n"
    ".popsection\n");

extern void ap_entry(void);
void ap_main(struct ap_boot *boot);

void ap_main(struct ap_boot *boot)
{
    int cpu = (int)boot->cpu;
    sched_cpu_init(cpu, 0);

    /* No printing until smp_running: the console takes no lock before.
     * The BSP reports what each AP left in cpus[]. */
    cpus[cpu].mpidr = read_mpidr_el1();
    cpus[cpu].midr = read_midr_el1();
    __asm__ volatile("dmb ish" : : : "memory");
    cpus[cpu].online = 1;

    /* Stage colors in this CPU's heartbeat square, which then blinks:
     * blue parked, yellow GIC up (red: failed), cyan timer, green scheduler */
    FB_CPU_MARK(cpu, 0x000000FF);
    while (!sched_go && !stop_req)
        __asm__ volatile("wfe");

    if (!stop_req) {
        if (gic_init_cpu() == 0) {
            FB_CPU_MARK(cpu, 0x00FFFF00);
            timer_init_cpu();
            FB_CPU_MARK(cpu, 0x0000FFFF);
            cpus[cpu].scheduling = 1;
            FB_CPU_MARK(cpu, 0x0000FF00);
            sched_ap_start();           /* returns only if the pool is full */
            cpus[cpu].scheduling = 0;
        } else {
            FB_CPU_MARK(cpu, 0x00FF0000);
        }
    }
    while (!stop_req)
        __asm__ volatile("wfe");
    smp_cpu_off();
}

/* Turn this CPU off for good: the scheduler's SGI_STOP or a parked AP */
void smp_cpu_off(void)
{
    int cpu = smp_cpu_id();
    irq_disable();
    write_cntv_ctl_el0(0);
    write_icc_igrpen1_el1(0);
    __asm__ volatile("isb");
    cpus[cpu].scheduling = 0;
    cpus[cpu].online = 0;
    __asm__ volatile("dsb ish" : : : "memory");
    psci_call(PSCI_CPU_OFF, 0, 0, 0);   /* returns only on failure */
    for (;;)
        __asm__ volatile("wfe");
}

/* ── BSP side ───────────────────────────────────────────── */

static void clean_to_poc(const void *p, uint64_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xF);
    uint64_t a = (uint64_t)(uintptr_t)p & ~(line - 1);
    for (; a < (uint64_t)(uintptr_t)p + size; a += line)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

static void print_psci_error(int cpu, const char *what, int64_t err)
{
    serial_puts("[SMP ] CPU ");
    serial_putdec(cpu);
    serial_puts(": ");
    serial_puts(what);
    serial_puts(" -");
    serial_putdec((uint64_t)-err);
    serial_puts("\n");
}

/* Start cores 0..num_cpus-1 other than the BSP, one at a time, waiting up
 * to 100 ms for each to report. psci_use_smc picks the PSCI conduit. */
void smp_boot_aps(int num_cpus, int psci_use_smc)
{
    if (num_cpus <= 1 || num_cpus > MAX_CPUS) return;
    psci_smc = psci_use_smc;
    stop_req = 0;

    uint64_t bsp = read_mpidr_el1();
    int self = core_number(bsp);
    uint64_t mair, tcr, ttbr0, ttbr1, cpacr, sctlr;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    cpus[self].mpidr = bsp;
    cpus[self].midr = read_midr_el1();
    cpus[self].online = 1;

    for (int i = 0; i < num_cpus; i++) {
        if (i == self) continue;
        struct ap_boot *b = &ap_boot[i];
        b->mair = mair;
        b->tcr = tcr;
        b->ttbr0 = ttbr0;
        b->ttbr1 = ttbr1;
        b->cpacr = cpacr;
        b->sctlr = sctlr;
        b->stack = (uint64_t)(uintptr_t)&ap_stacks[i][AP_STACK_SIZE];
        b->cpu = (uint64_t)i;
        cpus[i].online = 0;
        cpus[i].started = 0;
        clean_to_poc(b, sizeof(*b));

        /* Identity map: the entry and context addresses are physical */
        int64_t ret = psci_call(PSCI_CPU_ON, core_mpidr(bsp, i),
                                (uint64_t)(uintptr_t)ap_entry,
                                (uint64_t)(uintptr_t)b);
        if (ret != 0) {
            print_psci_error(i, "CPU_ON error", ret);
            continue;
        }
        cpus[i].started = 1;
        uint64_t t0 = timer_ms();
        while (!cpus[i].online && timer_ms() - t0 < 100)
            ;
        if (!cpus[i].online) {
            serial_puts("[SMP ] CPU ");
            serial_putdec(i);
            serial_puts(": no response\n");
            continue;
        }
        __asm__ volatile("dmb ish" : : : "memory");
        cpu_count++;
    }

    serial_puts("[SMP ] ");
    serial_putdec(cpu_count);
    serial_puts(" of ");
    serial_putdec(num_cpus);
    serial_puts(" CPUs online\n");
}

/* sched_start, on the BSP: the online APs join the scheduler */
void smp_release_aps(void)
{
    if (cpu_count <= 1)
        return;                 /* single CPU: no locking needed */
    smp_running = 1;
    __asm__ volatile("dsb ish" : : : "memory");
    sched_go = 1;
    __asm__ volatile("dsb ish\nsev" : : : "memory");
}

/* Turn the APs off (before jumping to another kernel, which reuses their
 * memory). Parked ones see stop_req; scheduling ones get SGI_STOP, which
 * they take as soon as they unmask IRQs, so never while holding a lock.
 * Waits up to 100 ms for PSCI to report each one off. */
void smp_stop_aps(void)
{
    uint64_t bsp = read_mpidr_el1();
    int self = core_number(bsp);

    stop_req = 1;
    __asm__ volatile("dsb ish\nsev" : : : "memory");
    for (int i = 0; i < MAX_CPUS; i++)
        if (i != self && cpus[i].scheduling)
            gic_send_sgi(core_mpidr(bsp, i), SGI_STOP);
    for (int i = 0; i < MAX_CPUS; i++) {
        if (i == self || !cpus[i].started) continue;
        uint64_t t0 = timer_ms();
        int64_t r;
        while ((r = psci_call(PSCI_AFFINITY_INFO, core_mpidr(bsp, i), 0, 0))
                   != AFFINITY_OFF && timer_ms() - t0 < 100)
            ;
        if (r != AFFINITY_OFF) {
            serial_puts("[SMP ] CPU ");
            serial_putdec(i);
            serial_puts(": still on after CPU_OFF\n");
        }
        cpus[i].started = 0;
    }
    cpu_count = 1;
    smp_running = 0;
}

int smp_get_cpu_count(void)
{
    return cpu_count;
}

/* Shell `cpus` */
void smp_print_cpus(void)
{
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].started && !cpus[i].online) {
            serial_puts("  cpu");
            serial_putdec(i);
            serial_puts("  started, never reported\n");
            continue;
        }
        if (!cpus[i].online) continue;
        const char *name = cpu_core_name(cpus[i].midr);
        serial_puts("  cpu");
        serial_putdec(i);
        serial_puts("  mpidr ");
        serial_puthex(cpus[i].mpidr, 8);
        serial_puts("  ");
        serial_puts(name ? name : "unknown core");
        serial_puts(" r");
        serial_putdec((cpus[i].midr >> 20) & 0xF);
        serial_puts("p");
        serial_putdec(cpus[i].midr & 0xF);
        task_tcb_t *t = cpu_sched[i].current;
        if (t) {
            serial_puts("  running ");
            serial_puts(t->name);
        } else {
            serial_puts("  parked");    /* not (yet) in the scheduler */
        }
        serial_puts("\n");
    }
    serial_puts("CPUs online: ");
    serial_putdec(cpu_count);
    serial_puts("\n");
}

/* ── cpubench: timed tasks on every CPU at once ──────────── */

#define BENCH_ITERS     20000000ULL     /* x 16 dependent adds */
#define BENCH_SLOTS     (2 * MAX_CPUS)

struct bench_slot {
    uint64_t ticks;                 /* CNTPCT ticks the loop took */
    int started_on, ran_on;
    int used;
    volatile int done;
};

static struct bench_slot bench[BENCH_SLOTS];

/* A chain of dependent adds retires one per cycle on these cores, so
 * cycles ~ 16 per iteration and the rate gives the clock */
static void bench_task(void *arg)
{
    struct bench_slot *s = arg;
    uint64_t x = 0, n = BENCH_ITERS;
    s->started_on = smp_cpu_id();
    uint64_t t0 = read_cntpct_el0();
    __asm__ volatile(
        "1:\n"
        ".rept 16\n"
        "add   %0, %0, %2\n"
        ".endr\n"
        "subs  %1, %1, #1\n"
        "b.ne  1b\n"
        : "+r"(x), "+r"(n) : "r"(1ULL) : "cc");
    s->ticks = read_cntpct_el0() - t0;
    s->ran_on = smp_cpu_id();
    __asm__ volatile("dmb ish" : : : "memory");
    s->done = 1;
}

static void print_ms(uint64_t ticks, uint64_t freq)
{
    serial_putdec(ticks * 1000 / freq);
    serial_puts(" ms");
}

/* Pinned: one task per online CPU, task i on CPU i. `any`: two per online
 * CPU, not pinned, so CPUs share them and pick up each other's. */
void smp_bench(int any)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    int n = 0;
    uint64_t t0 = read_cntpct_el0();
    for (int i = 0; i < BENCH_SLOTS; i++) {
        int cpu = i % MAX_CPUS;
        bench[i].done = 0;
        bench[i].ticks = 0;
        bench[i].used = cpus[cpu].online && (any || i < MAX_CPUS);
        if (!bench[i].used)
            continue;
        if (task_create_on("bench", bench_task, &bench[i], 1,
                           any ? TASK_ANY_CPU : cpu) < 0) {
            bench[i].done = -1;
            continue;
        }
        n++;
    }

    /* Wait for all of them, sleeping so the bench on this CPU can run */
    for (int waited = 0; waited < 30000; waited += 10) {
        int pending = 0;
        for (int i = 0; i < BENCH_SLOTS; i++)
            if (bench[i].used && !bench[i].done)
                pending++;
        if (!pending)
            break;
        task_delay_ms(10);
    }
    uint64_t wall = read_cntpct_el0() - t0;
    __asm__ volatile("dmb ish" : : : "memory");

    uint64_t sum = 0;
    int moved = 0;
    serial_puts("task  cpu   core          time    ~MHz\n");
    for (int i = 0; i < BENCH_SLOTS; i++) {
        if (!bench[i].used)
            continue;
        serial_puts(i < 10 ? "   " : "  ");
        serial_putdec(i);
        serial_puts("   ");
        if (bench[i].done != 1) {
            serial_puts(bench[i].done ? "no task slot\n" : "did not finish\n");
            continue;
        }
        /* Where it started, and where it finished if it moved */
        int c0 = bench[i].started_on, c1 = bench[i].ran_on;
        serial_putdec(c0);
        if (c1 != c0) {
            serial_puts(">");
            serial_putdec(c1);
            moved++;
        } else {
            serial_puts("  ");
        }
        serial_puts("  ");
        const char *name = cpu_core_name(cpus[c1].midr);
        serial_puts(name ? name : "?");
        serial_puts("  ");
        print_ms(bench[i].ticks, freq);
        serial_puts("  ");
        /* MHz = cycles / us = 16 * ITERS / (ticks / freq * 1e6); only
         * meaningful when the task had its CPU to itself */
        serial_putdec(16 * BENCH_ITERS * freq / 1000000 / bench[i].ticks);
        serial_puts("\n");
        sum += bench[i].ticks;
    }
    serial_puts("wall ");
    print_ms(wall, freq);
    serial_puts(", sum of the tasks ");
    print_ms(sum, freq);
    serial_puts(", ");
    serial_putdec(n);
    serial_puts(" tasks, ");
    serial_putdec(moved);
    serial_puts(" changed CPU\n");
}
