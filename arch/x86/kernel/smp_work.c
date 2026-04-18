/*
 * OsitoK x86-64 — SMP Work Distribution
 *
 * Converts idle AP cores into general-purpose workers. Each AP runs
 * a work loop: HLT until IPI wakes it, execute the submitted function,
 * signal completion, repeat.
 *
 * The BSP submits work via smp_submit/smp_submit_any and can block
 * on smp_wait/smp_barrier for completion.
 */

#include "smp_work.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern void serial_puthex(uint64_t val, int digits);

/* APIC access */
extern volatile uint32_t *idt_get_apic_base(void);

#define APIC_ICR_LOW   0x300
#define APIC_ICR_HIGH  0x310
#define APIC_EOI       0x0B0

/* ── Global state ───────────────────────────────────────────── */

ap_control_t ap_controls[SMP_MAX_APS];
int          ap_worker_count;
volatile int ap_work_system_ready;  /* Set to 1 after smp_work_init */

/* ── Send IPI to a specific LAPIC ID ────────────────────────── */

static void send_ipi(uint32_t lapic_id, uint8_t vector)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return;

    /* ICR high: destination LAPIC ID in bits 24-31 */
    apic[APIC_ICR_HIGH / 4] = lapic_id << 24;

    /* ICR low: fixed delivery mode, specified vector */
    apic[APIC_ICR_LOW / 4] = (uint32_t)vector;

    /* Wait for delivery (bit 12 = delivery status) */
    int timeout = 100000;
    while ((apic[APIC_ICR_LOW / 4] & (1 << 12)) && --timeout > 0)
        __asm__ volatile ("pause");
}

/* ── BSP-side API ───────────────────────────────────────────── */

void smp_work_init(void)
{
    /* Discover APs from the SMP cpu table */
    extern uint32_t smp_get_cpu_count(void);
    extern uint32_t smp_get_cpu_apic_id(uint32_t index);
    extern uint32_t smp_get_bsp_apic_id(void);

    uint32_t total = smp_get_cpu_count();
    uint32_t bsp_id = smp_get_bsp_apic_id();

    memset(ap_controls, 0, sizeof(ap_controls));
    ap_worker_count = 0;

    for (uint32_t i = 0; i < total && ap_worker_count < SMP_MAX_APS; i++) {
        uint32_t apic_id = smp_get_cpu_apic_id(i);
        if (apic_id == bsp_id) continue;  /* skip BSP */

        ap_controls[ap_worker_count].lapic_id = apic_id;
        ap_controls[ap_worker_count].state = AP_OFFLINE;
        ap_worker_count++;
    }

    /* Signal APs that control blocks are ready */
    __asm__ volatile ("mfence" ::: "memory");
    ap_work_system_ready = 1;

    /* Don't IPI APs yet — they'll wake on the next timer tick
     * or when first work is submitted. The wait loop uses sti;hlt
     * so any interrupt (including spurious) will wake them. */

    serial_puts("[SMP-WORK] Initialized, ");
    serial_putdec((uint64_t)ap_worker_count);
    serial_puts(" APs as workers\n");
}

int smp_submit(int ap_idx, smp_work_func_t func, void *arg, void *result)
{
    if (ap_idx < 0 || ap_idx >= ap_worker_count) return -1;
    ap_control_t *ap = &ap_controls[ap_idx];

    if (ap->state != AP_IDLE) return -1;

    ap->func       = func;
    ap->arg        = arg;
    ap->result_buf = result;
    ap->done       = 0;

    __asm__ volatile ("mfence" ::: "memory");

    ap->work_pending = 1;
    ap->state = AP_BUSY;

    __asm__ volatile ("mfence" ::: "memory");

    /* IPI to wake the AP from HLT */
    send_ipi(ap->lapic_id, SMP_IPI_VECTOR);

    return 0;
}

int smp_submit_any(smp_work_func_t func, void *arg, void *result)
{
    for (int i = 0; i < ap_worker_count; i++) {
        if (ap_controls[i].state == AP_IDLE) {
            if (smp_submit(i, func, arg, result) == 0)
                return i;
        }
    }
    return -1;
}

void smp_wait(int ap_idx)
{
    if (ap_idx < 0 || ap_idx >= ap_worker_count) return;
    while (!ap_controls[ap_idx].done)
        __asm__ volatile ("pause" ::: "memory");
}

void smp_barrier(void)
{
    for (int i = 0; i < ap_worker_count; i++) {
        if (ap_controls[i].state == AP_BUSY)
            smp_wait(i);
    }
}

/* ── AP-side worker loop ────────────────────────────────────── */

void ap_worker_loop(uint32_t cpu_idx)
{
    /* Wait for BSP to call smp_work_init() and populate control blocks.
     * Use sti;hlt so QEMU doesn't waste host CPU on busy-wait.
     * ISR fxsave/fxrstor is now per-CPU safe (skipped on APs). */
    while (!ap_work_system_ready)
        __asm__ volatile ("sti; hlt" ::: "memory");

    /* Find our control block — match by LAPIC ID using the cpu_idx
     * we received from smp_ap_entry. */
    extern uint32_t smp_get_cpu_apic_id(uint32_t index);
    uint32_t my_id = smp_get_cpu_apic_id(cpu_idx);
    int my_idx = -1;

    for (int i = 0; i < ap_worker_count; i++) {
        if (ap_controls[i].lapic_id == my_id) {
            my_idx = i;
            break;
        }
    }

    if (my_idx < 0) {
        /* Orphan AP — not in our control table, just idle */
        for (;;) __asm__ volatile ("pause");
    }

    ap_control_t *me = &ap_controls[my_idx];
    me->state = AP_IDLE;

    for (;;) {
        /* Wait for work — HLT until IPI or timer wakes us */
        while (!me->work_pending)
            __asm__ volatile ("sti; hlt" ::: "memory");

        __asm__ volatile ("mfence" ::: "memory");

        /* Execute */
        if (me->func)
            me->func(me->arg, me->result_buf);

        /* Signal completion */
        me->done = 1;
        me->work_pending = 0;
        me->tasks_completed++;
        me->state = AP_IDLE;

        __asm__ volatile ("mfence" ::: "memory");
    }
}
