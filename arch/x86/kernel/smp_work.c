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

    /* Wait for APs to enter their worker loops and report AP_IDLE.
     * They wake from HLT via timer ticks (EOI-only fast path). */
    {
        int timeout = 100000;
        while (timeout-- > 0) {
            int ready = 0;
            for (int i = 0; i < ap_worker_count; i++)
                if (ap_controls[i].state == AP_IDLE) ready++;
            if (ready == ap_worker_count) break;
            __asm__ volatile ("pause");
        }
    }

    serial_puts("[SMP-WORK] Initialized, ");
    serial_putdec((uint64_t)ap_worker_count);
    serial_puts(" APs as workers\n");

    /* Smoke test: submit a no-op to AP 0 to verify IPI delivery works */
    if (ap_worker_count > 0 && ap_controls[0].state == AP_IDLE) {
        static volatile int test_done;
        test_done = 0;
        smp_submit(0, (smp_work_func_t)(void (*)(void*,void*))0, NULL, NULL);

        /* Wait with timeout */
        int tout = 10000000;
        while (!ap_controls[0].done && --tout > 0)
            __asm__ volatile ("pause");

        if (ap_controls[0].done) {
            serial_puts("[SMP-WORK] AP 0 smoke test: OK\n");
        } else {
            serial_puts("[SMP-WORK] AP 0 smoke test: TIMEOUT (state=");
            serial_putdec((uint64_t)ap_controls[0].state);
            serial_puts(" pending=");
            serial_putdec((uint64_t)ap_controls[0].work_pending);
            serial_puts(")\n");
        }
    }
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
    int timeout = 100000000;  /* ~seconds at pause speed */
    while (!ap_controls[ap_idx].done && --timeout > 0)
        __asm__ volatile ("pause" ::: "memory");
    if (timeout <= 0) {
        serial_puts("[SMP-WORK] TIMEOUT waiting for AP ");
        serial_putdec((uint64_t)ap_idx);
        serial_puts(" (state="); serial_putdec((uint64_t)ap_controls[ap_idx].state);
        serial_puts(" pending="); serial_putdec((uint64_t)ap_controls[ap_idx].work_pending);
        serial_puts(" done="); serial_putdec((uint64_t)ap_controls[ap_idx].done);
        serial_puts(")\n");
    }
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
    /* Initialize FPU/SSE on this AP — required before any float work.
     * Without this, the AP's FPU state is undefined and float ops
     * may generate #MF/#XM exceptions that loop forever in the
     * AP fast-path ISR (EOI + iretq → retry → exception → ...). */
    __asm__ volatile ("fninit");
    /* Enable SSE: set CR0.MP, clear CR0.EM, set CR4.OSFXSR+OSXMMEXCPT */
    {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 = (cr0 | (1 << 1)) & ~(1ULL << 2);  /* MP=1, EM=0 */
        __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9) | (1 << 10);  /* OSFXSR + OSXMMEXCPT */
        __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
    }

    /* Wait for BSP to call smp_work_init() and populate control blocks. */
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
