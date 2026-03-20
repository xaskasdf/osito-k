/*
 * smp.c -- Multi-core SMP boot for AArch64 via PSCI
 *
 * Uses PSCI CPU_ON (function ID 0xC4000003) to start secondary cores.
 * Each AP initializes its own stack, GIC redistributor, and VBAR_EL1,
 * then enters the idle loop waiting for scheduler work.
 *
 * QEMU virt: -smp N provides N cores, MPIDR_EL1 identifies each.
 */

#include "../include/hal.h"
#include "../include/aarch64.h"
#include "../include/types.h"

#define MAX_CPUS        8
#define AP_STACK_SIZE   8192

/* Per-CPU state */
static volatile int     cpu_count = 1;   /* BSP = 1, incremented by APs */
static volatile int     ap_ready[MAX_CPUS];
uint8_t                 ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));

/* External: GIC per-CPU init */
extern void gic_init(void);

/* ── PSCI CPU_ON via SMC ─────────────────────────────────── */

static int psci_cpu_on(uint64_t target_cpu, uint64_t entry_point)
{
    register uint64_t x0 __asm__("x0") = 0xC4000003ULL;  /* CPU_ON 64-bit */
    register uint64_t x1 __asm__("x1") = target_cpu;
    register uint64_t x2 __asm__("x2") = entry_point;
    register uint64_t x3 __asm__("x3") = 0;              /* context_id */
    __asm__ volatile("hvc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (int)(int64_t)x0;  /* 0 = success, negative = error */
}

/* ── AP entry point (called from asm trampoline) ─────────── */

void ap_main(uint64_t cpu_id)
{
    /* Set exception vectors (same as BSP) */
    extern char _vectors[];
    __asm__ volatile("msr VBAR_EL1, %0" :: "r"((uint64_t)_vectors));
    __asm__ volatile("isb");

    /* Store CPU ID in TPIDR_EL1 for per-CPU identification */
    __asm__ volatile("msr TPIDR_EL1, %0" :: "r"(cpu_id));

    /* Signal ready + wake BSP */
    __asm__ volatile("dmb ish" ::: "memory");
    ap_ready[cpu_id] = 1;
    cpu_count++;
    __asm__ volatile("dmb ish" ::: "memory");
    __asm__ volatile("sev");  /* wake BSP from wfe */

    serial_puts("[SMP ] CPU ");
    serial_putdec(cpu_id);
    serial_puts(" online\n");

    /* Idle loop — wait for scheduler to assign work */
    for (;;)
        __asm__ volatile("wfe");
}

/* ── AP assembly entry (sets stack, calls ap_main) ────────── */

/* Defined as a naked function to control the entry exactly */
__attribute__((naked, noreturn))
void ap_entry(void)
{
    /* x0 = context_id (cpu_id passed from PSCI CPU_ON) */
    __asm__ volatile(
        /* Calculate stack: ap_stacks + (cpu_id + 1) * AP_STACK_SIZE */
        "adrp    x1, ap_stacks\n"
        "add     x1, x1, :lo12:ap_stacks\n"
        "add     x2, x0, #1\n"
        "mov     x3, %0\n"
        "mul     x2, x2, x3\n"
        "add     x1, x1, x2\n"
        "mov     sp, x1\n"
        /* Call ap_main(cpu_id) */
        "bl      ap_main\n"
        /* Should never return */
        "1: wfe\n"
        "b       1b\n"
        :: "i"(AP_STACK_SIZE)
        : "memory"
    );
}

/* ── Boot secondary CPUs ─────────────────────────────────── */

void smp_boot_aps(int num_cpus)
{
    if (num_cpus <= 1 || num_cpus > MAX_CPUS) return;

    serial_puts("[SMP ] Booting ");
    serial_putdec(num_cpus - 1);
    serial_puts(" secondary CPUs...\n");

    for (int i = 1; i < num_cpus; i++) {
        ap_ready[i] = 0;

        /* QEMU virt uses MPIDR_EL1 Aff0 = cpu_id for CPU identification */
        int ret = psci_cpu_on((uint64_t)i, (uint64_t)ap_entry);
        if (ret != 0) {
            serial_puts("[SMP ] CPU ");
            serial_putdec(i);
            serial_puts(" failed (PSCI error ");
            serial_putdec((uint64_t)(-(int64_t)ret));
            serial_puts(")\n");
            continue;
        }

        /* Don't wait — QEMU TCG can't schedule APs while BSP polls.
         * APs will come up asynchronously and increment cpu_count. */
    }

    serial_puts("[SMP ] PSCI CPU_ON sent to ");
    serial_putdec(num_cpus - 1);
    serial_puts(" APs (async startup)\n");
}

int smp_get_cpu_count(void)
{
    return cpu_count;
}
