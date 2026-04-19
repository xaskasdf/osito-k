/*
 * OsitoK x86-64 — SMP Work-Stealing Scheduler
 *
 * Chase-Lev work-stealing deques per CPU. Each core (BSP + APs) owns
 * a deque: owner pushes/pops from bottom (LIFO, cache-warm sub-tasks),
 * thieves steal from top (FIFO, distributes oldest work first).
 *
 * Key insight: smp_wait() does useful work while waiting — it pops
 * from its own deque and steals from others. This enables nested
 * parallelism: an AP running matvec can push row sub-tasks that
 * BSP (in smp_wait) or other idle APs steal and execute.
 */

#include "smp_work.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);

/* APIC access */
extern volatile uint32_t *idt_get_apic_base(void);

#define APIC_ICR_LOW   0x300
#define APIC_ICR_HIGH  0x310

/* ── Global state ───────────────────────────────────────────── */

ws_cpu_t         ws_cpus[SMP_TOTAL_CPUS] __attribute__((aligned(128)));
int              ws_cpu_count;
int              ap_worker_count;       /* ws_cpu_count - 1 (compat) */
static volatile int ws_ready;           /* set to 1 after init */

/* ── Task pool ──────────────────────────────────────────────── */

static smp_task_t task_pool[TASK_POOL_SIZE] __attribute__((aligned(64)));
static volatile uint32_t task_bitmap[2]; /* 64 bits for 64 slots */

static int task_alloc(void)
{
    for (int w = 0; w < 2; w++) {
        uint32_t bits = __atomic_load_n(&task_bitmap[w], __ATOMIC_RELAXED);
        while (bits != 0xFFFFFFFFU) {
            uint32_t free_bit = __builtin_ctz(~bits);
            uint32_t mask = 1u << free_bit;
            uint32_t old = __sync_fetch_and_or(&task_bitmap[w], mask);
            if (!(old & mask)) {
                uint32_t id = (uint32_t)w * 32 + free_bit;
                task_pool[id].completed = 0;
                task_pool[id].auto_free = 0;
                task_pool[id].task_id = id;
                return (int)id;
            }
            bits = __atomic_load_n(&task_bitmap[w], __ATOMIC_RELAXED);
        }
    }
    return -1;
}

static void task_free(uint32_t tid)
{
    uint32_t w = tid / 32;
    uint32_t bit = tid % 32;
    __sync_fetch_and_and(&task_bitmap[w], ~(1u << bit));
}

/* ── Chase-Lev deque operations ─────────────────────────────── */

static int deque_push(ws_deque_t *dq, uint32_t task_id)
{
    int64_t b = __atomic_load_n(&dq->bottom, __ATOMIC_RELAXED);
    int64_t t = __atomic_load_n(&dq->top, __ATOMIC_ACQUIRE);

    if (b - t >= DEQUE_CAPACITY)
        return -1;  /* full */

    dq->buf[b & (DEQUE_CAPACITY - 1)] = task_id;
    __atomic_store_n(&dq->bottom, b + 1, __ATOMIC_RELEASE);
    return 0;
}

static int deque_pop(ws_deque_t *dq)
{
    int64_t b = __atomic_load_n(&dq->bottom, __ATOMIC_RELAXED) - 1;
    __atomic_store_n(&dq->bottom, b, __ATOMIC_RELAXED);

    /* MFENCE: ensure bottom decrement visible before reading top.
     * Without this, CPU could reorder load-top before store-bottom,
     * allowing both owner and thief to take the same item. */
    __asm__ volatile ("mfence" ::: "memory");

    int64_t t = __atomic_load_n(&dq->top, __ATOMIC_RELAXED);

    if (t > b) {
        /* Empty — restore bottom */
        __atomic_store_n(&dq->bottom, t, __ATOMIC_RELAXED);
        return -1;
    }

    uint32_t task_id = dq->buf[b & (DEQUE_CAPACITY - 1)];

    if (t == b) {
        /* Last element — race with steal */
        if (!__sync_bool_compare_and_swap(&dq->top, t, t + 1)) {
            __atomic_store_n(&dq->bottom, t + 1, __ATOMIC_RELAXED);
            return -1;  /* thief got it */
        }
        __atomic_store_n(&dq->bottom, t + 1, __ATOMIC_RELAXED);
    }

    return (int)task_id;
}

static int deque_steal(ws_deque_t *dq)
{
    int64_t t = __atomic_load_n(&dq->top, __ATOMIC_ACQUIRE);
    int64_t b = __atomic_load_n(&dq->bottom, __ATOMIC_ACQUIRE);

    if (t >= b)
        return -1;  /* empty */

    uint32_t task_id = dq->buf[t & (DEQUE_CAPACITY - 1)];

    if (!__sync_bool_compare_and_swap(&dq->top, t, t + 1))
        return -1;  /* another thief won */

    return (int)task_id;
}

/* ── Send IPI ───────────────────────────────────────────────── */

static void send_ipi(uint32_t lapic_id, uint8_t vector)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return;

    apic[APIC_ICR_HIGH / 4] = lapic_id << 24;
    apic[APIC_ICR_LOW / 4] = (uint32_t)vector;

    int timeout = 100000;
    while ((apic[APIC_ICR_LOW / 4] & (1 << 12)) && --timeout > 0)
        __asm__ volatile ("pause");
}

/* Wake one sleeping AP (if any). Only sends ONE IPI — the woken AP
 * will find work and potentially wake others if deques stay busy. */
static void wake_one_ap(void)
{
    for (int i = 1; i < ws_cpu_count; i++) {
        if (__atomic_load_n(&ws_cpus[i].sleeping, __ATOMIC_ACQUIRE)) {
            send_ipi(ws_cpus[i].lapic_id, SMP_IPI_VECTOR);
            return;
        }
    }
}

/* ── Fast CPU identification ────────────────────────────────── */

extern uint8_t apic_to_cpu_lut[256];

static inline uint32_t current_cpu(void)
{
    volatile uint32_t *apic = idt_get_apic_base();
    if (!apic) return 0;
    uint32_t id = (apic[0x20 / 4] >> 24) & 0xFF;
    return apic_to_cpu_lut[id];
}

/* ── Execute one task ───────────────────────────────────────── */

static void execute_task(uint32_t tid, ws_cpu_t *me)
{
    smp_task_t *t = &task_pool[tid];
    if (t->func)
        t->func(t->arg, t->result_buf);
    __atomic_store_n(&t->completed, 1, __ATOMIC_RELEASE);
    me->tasks_completed++;
    if (t->auto_free)
        task_free(tid);
}

/* Try to find and execute one task. Returns 1 if executed, 0 if nothing. */
static int try_execute_one(ws_cpu_t *me)
{
    /* 1. Pop from own deque (LIFO — cache-warm sub-tasks) */
    int tid = deque_pop(&me->deque);

    /* 2. Steal from others (FIFO — oldest first) */
    if (tid < 0) {
        uint32_t start = (uint32_t)(me->tasks_completed) % (uint32_t)ws_cpu_count;
        for (int attempt = 0; attempt < ws_cpu_count; attempt++) {
            int victim = ((int)start + attempt) % ws_cpu_count;
            if (victim == (int)me->cpu_index) continue;
            tid = deque_steal(&ws_cpus[victim].deque);
            if (tid >= 0) {
                me->tasks_stolen++;
                break;
            }
        }
    }

    if (tid >= 0) {
        execute_task((uint32_t)tid, me);
        return 1;
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────── */

static int submit_internal(int target_cpu, smp_work_func_t func,
                           void *arg, void *result, int auto_free)
{
    int tid = task_alloc();
    if (tid < 0) return -1;

    smp_task_t *task = &task_pool[tid];
    task->func       = func;
    task->arg        = arg;
    task->result_buf = result;
    task->auto_free  = (uint8_t)auto_free;

    __asm__ volatile ("mfence" ::: "memory");

    if (deque_push(&ws_cpus[target_cpu].deque, (uint32_t)tid) < 0) {
        task_free((uint32_t)tid);
        return -1;
    }

    wake_one_ap();
    return tid;
}

int smp_submit(int ap_idx, smp_work_func_t func, void *arg, void *result)
{
    (void)ap_idx;  /* ap_idx hint ignored — work-stealing distributes automatically */
    uint32_t my_cpu = current_cpu();
    return submit_internal((int)my_cpu, func, arg, result, 0);
}

int smp_submit_any(smp_work_func_t func, void *arg, void *result)
{
    uint32_t my_cpu = current_cpu();
    return submit_internal((int)my_cpu, func, arg, result, 0);
}

int smp_submit_ff(smp_work_func_t func, void *arg, void *result)
{
    uint32_t my_cpu = current_cpu();
    return submit_internal((int)my_cpu, func, arg, result, 1);
}

void smp_wait(int task_id)
{
    if (task_id < 0 || task_id >= TASK_POOL_SIZE) return;

    smp_task_t *task = &task_pool[task_id];
    uint32_t cpu = current_cpu();
    ws_cpu_t *me = &ws_cpus[cpu];

    /* Pop from our OWN deque while waiting. All smp_submit calls push
     * to the caller's deque, so our sub-tasks are here. APs in the
     * worker loop steal from us — we don't need to steal from them.
     * This avoids circular waits from nested work-stealing. */
    while (!__atomic_load_n(&task->completed, __ATOMIC_ACQUIRE)) {
        int tid = deque_pop(&me->deque);
        if (tid >= 0) {
            execute_task((uint32_t)tid, me);
        } else {
            __asm__ volatile ("pause" ::: "memory");
        }
    }

    task_free((uint32_t)task_id);
}

/* Non-blocking check: has this task completed?
 * Used by io_uring to poll for async completion without blocking. */
bool smp_task_done(int task_id)
{
    if (task_id < 0 || task_id >= TASK_POOL_SIZE) return true;
    return __atomic_load_n(&task_pool[task_id].completed, __ATOMIC_ACQUIRE);
}

void smp_barrier(void)
{
    ws_cpu_t *me = &ws_cpus[current_cpu()];
    int idle = 0;
    while (idle < 200) {
        if (try_execute_one(me))
            idle = 0;
        else {
            __asm__ volatile ("pause" ::: "memory");
            idle++;
        }
    }
}

void smp_stats(void)
{
    serial_puts("[SMP-WS] Work-stealing stats:\n");
    for (int i = 0; i < ws_cpu_count; i++) {
        serial_puts("  CPU ");
        serial_putdec((uint64_t)i);
        serial_puts(i == 0 ? " (BSP)" : " (AP) ");
        serial_puts(": completed=");
        serial_putdec(ws_cpus[i].tasks_completed);
        serial_puts(" stolen=");
        serial_putdec(ws_cpus[i].tasks_stolen);
        serial_puts("\n");
    }
}

/* ── Initialization ─────────────────────────────────────────── */

void smp_work_init(void)
{
    extern uint32_t smp_get_cpu_count(void);
    extern uint32_t smp_get_cpu_apic_id(uint32_t index);
    extern uint32_t smp_get_bsp_apic_id(void);

    uint32_t total = smp_get_cpu_count();
    uint32_t bsp_id = smp_get_bsp_apic_id();

    memset(ws_cpus, 0, sizeof(ws_cpus));
    memset(task_pool, 0, sizeof(task_pool));
    memset((void *)task_bitmap, 0, sizeof(task_bitmap));
    ws_cpu_count = 0;

    /* CPU 0 = BSP */
    ws_cpus[0].cpu_index = 0;
    ws_cpus[0].lapic_id = bsp_id;
    ws_cpu_count = 1;

    /* CPU 1..N = APs */
    for (uint32_t i = 0; i < total && ws_cpu_count < SMP_TOTAL_CPUS; i++) {
        uint32_t apic_id = smp_get_cpu_apic_id(i);
        if (apic_id == bsp_id) continue;

        ws_cpus[ws_cpu_count].cpu_index = (uint32_t)ws_cpu_count;
        ws_cpus[ws_cpu_count].lapic_id = apic_id;
        ws_cpu_count++;
    }

    ap_worker_count = ws_cpu_count - 1;

    __asm__ volatile ("mfence" ::: "memory");
    ws_ready = 1;

    /* Wait for APs to reach their work-stealing loop */
    {
        int timeout = 100000;
        while (timeout-- > 0) {
            int ready = 0;
            for (int i = 1; i < ws_cpu_count; i++)
                if (__atomic_load_n(&ws_cpus[i].sleeping, __ATOMIC_RELAXED) ||
                    ws_cpus[i].tasks_completed > 0)
                    ready++;
            if (ready >= ap_worker_count) break;
            __asm__ volatile ("pause");
        }
    }

    serial_puts("[SMP-WS] Work-stealing initialized, ");
    serial_putdec((uint64_t)ap_worker_count);
    serial_puts(" APs, ");
    serial_putdec(TASK_POOL_SIZE);
    serial_puts(" task slots, ");
    serial_putdec(DEQUE_CAPACITY);
    serial_puts("-deep deques\n");

    /* Smoke test: submit a no-op to AP 0 */
    if (ap_worker_count > 0) {
        int tid = smp_submit(0, NULL, NULL, NULL);
        if (tid >= 0) {
            smp_wait(tid);
            serial_puts("[SMP-WS] AP 0 smoke test: OK\n");
        } else {
            serial_puts("[SMP-WS] AP 0 smoke test: submit failed\n");
        }
    }
}

/* ── AP worker loop ─────────────────────────────────────────── */

void ap_worker_loop(uint32_t cpu_idx)
{
    /* Initialize FPU/SSE on this AP */
    __asm__ volatile ("fninit");
    {
        uint64_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 = (cr0 | (1 << 1)) & ~(1ULL << 2);
        __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9) | (1 << 10);
        __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
    }

    /* Wait for BSP to call smp_work_init() */
    while (!ws_ready)
        __asm__ volatile ("sti; hlt" ::: "memory");

    /* Find our slot */
    extern uint32_t smp_get_cpu_apic_id(uint32_t index);
    uint32_t my_id = smp_get_cpu_apic_id(cpu_idx);
    int my_slot = -1;
    for (int i = 1; i < ws_cpu_count; i++) {
        if (ws_cpus[i].lapic_id == my_id) {
            my_slot = i;
            break;
        }
    }
    if (my_slot < 0) {
        for (;;) __asm__ volatile ("pause");
    }

    ws_cpu_t *me = &ws_cpus[my_slot];

    for (;;) {
        /* Try to find and execute work */
        if (try_execute_one(me))
            continue;

        /* No work — prepare to sleep */
        __atomic_store_n(&me->sleeping, 1, __ATOMIC_RELEASE);
        __asm__ volatile ("mfence" ::: "memory");

        /* Double-check: a task may have appeared between our last
         * steal attempt and setting the sleeping flag. */
        {
            int found = 0;
            for (int i = 0; i < ws_cpu_count && !found; i++) {
                int64_t t = __atomic_load_n(&ws_cpus[i].deque.top, __ATOMIC_RELAXED);
                int64_t b = __atomic_load_n(&ws_cpus[i].deque.bottom, __ATOMIC_RELAXED);
                if (b > t) found = 1;
            }
            if (found) {
                __atomic_store_n(&me->sleeping, 0, __ATOMIC_RELEASE);
                continue;
            }
        }

        __asm__ volatile ("sti; hlt" ::: "memory");
        __atomic_store_n(&me->sleeping, 0, __ATOMIC_RELEASE);
    }
}
