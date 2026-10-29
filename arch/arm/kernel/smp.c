/*
 * smp.c -- Multi-core SMP boot for AArch64 via PSCI
 *
 * Starts the secondary cores (APs) with PSCI CPU_ON. Each AP turns on its
 * MMU and caches with the BSP's page tables, reports its MIDR/MPIDR and parks
 * in wfe with interrupts masked: nothing schedules on it yet. Before a kload
 * handoff they are turned off again with CPU_OFF (smp_stop_aps).
 *
 * QEMU virt: PSCI over HVC, -smp N cores numbered in MPIDR Aff0.
 * SM8350: PSCI over SMC, 8 cores (4x A55, 3x A78, 1x X1) with MPIDR.MT set,
 * so the core number is Aff1 (0x81000000, 0x81000100, ... 0x81000700).
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "../include/types.h"

#define MAX_CPUS        8
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
    int started;                    /* CPU_ON accepted (BSP-owned) */
};

static struct ap_boot  ap_boot[MAX_CPUS];
static struct cpu_info cpus[MAX_CPUS];
static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));
static int cpu_count = 1;           /* BSP + online APs */
static int psci_smc;                /* conduit: SMC on SM8350, HVC on virt */
static volatile int stop_req;

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

/* ── AP side ────────────────────────────────────────────── */

/* PSCI CPU_ON entry: EL1, MMU and caches off, interrupts masked,
 * x0 = context_id = this AP's struct ap_boot. Loads the BSP's translation
 * regime and vectors, turns the MMU on (the map is identity, so the next
 * instruction is still valid) and calls ap_main(boot) on the AP's stack. */
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
    uint64_t cpu = boot->cpu;
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(cpu));

    /* No printing here: the console (USB) isn't safe to share between
     * CPUs. The BSP reports what each AP left in cpus[]. */
    cpus[cpu].mpidr = read_mpidr_el1();
    cpus[cpu].midr = read_midr_el1();
    __asm__ volatile("dmb ish" : : : "memory");
    cpus[cpu].online = 1;

    while (!stop_req)
        __asm__ volatile("wfe");

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

/* Turn the APs off (before jumping to another kernel, which reuses their
 * memory). Waits up to 100 ms for PSCI to report each one off. */
void smp_stop_aps(void)
{
    uint64_t bsp = read_mpidr_el1();

    stop_req = 1;
    __asm__ volatile("dsb ish\nsev" : : : "memory");
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!cpus[i].started) continue;
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
        serial_puts("\n");
    }
    serial_puts("CPUs online: ");
    serial_putdec(cpu_count);
    serial_puts("\n");
}
