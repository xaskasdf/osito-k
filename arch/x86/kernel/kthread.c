/*
 * OsitoK x86-64 — Kernel Thread API
 *
 * kthread_create/kthread_stop for kernel-space worker threads.
 * Used by: flush daemons, network workers, async I/O processors.
 * Wrapper around sched_spawn with kernel-specific lifecycle.
 */

#include "../include/types.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t val);
extern int sched_spawn(const char *name, void (*entry)(void));
extern int sched_spawn_qos(const char *name, void (*entry)(void), uint8_t qos);

#define KTHREAD_MAX 16

typedef struct {
    bool     active;
    char     name[32];
    int      pid;
    bool     should_stop;
    void   (*func)(void *data);
    void    *data;
} kthread_t;

static kthread_t kthreads[KTHREAD_MAX];

/* Wrapper that calls the actual function with its data argument */
static void kthread_run_slot(int idx)
{
    if (idx < 0 || idx >= KTHREAD_MAX || !kthreads[idx].active) return;
    kthreads[idx].func(kthreads[idx].data);
    kthreads[idx].active = false;
}

#define KTHREAD_WRAPPER(n) \
    static void kthread_wrapper_##n(void) { kthread_run_slot(n); }

KTHREAD_WRAPPER(0)
KTHREAD_WRAPPER(1)
KTHREAD_WRAPPER(2)
KTHREAD_WRAPPER(3)
KTHREAD_WRAPPER(4)
KTHREAD_WRAPPER(5)
KTHREAD_WRAPPER(6)
KTHREAD_WRAPPER(7)
KTHREAD_WRAPPER(8)
KTHREAD_WRAPPER(9)
KTHREAD_WRAPPER(10)
KTHREAD_WRAPPER(11)
KTHREAD_WRAPPER(12)
KTHREAD_WRAPPER(13)
KTHREAD_WRAPPER(14)
KTHREAD_WRAPPER(15)

static void (*const kthread_wrappers[KTHREAD_MAX])(void) = {
    kthread_wrapper_0,  kthread_wrapper_1,
    kthread_wrapper_2,  kthread_wrapper_3,
    kthread_wrapper_4,  kthread_wrapper_5,
    kthread_wrapper_6,  kthread_wrapper_7,
    kthread_wrapper_8,  kthread_wrapper_9,
    kthread_wrapper_10, kthread_wrapper_11,
    kthread_wrapper_12, kthread_wrapper_13,
    kthread_wrapper_14, kthread_wrapper_15,
};

/* Create and start a kernel thread */
int kthread_create(const char *name, void (*func)(void *), void *data)
{
    for (int i = 0; i < KTHREAD_MAX; i++) {
        if (!kthreads[i].active) {
            kthread_t *kt = &kthreads[i];
            kt->active = true;
            kt->should_stop = false;
            kt->func = func;
            kt->data = data;
            int j = 0;
            while (name[j] && j < 31) { kt->name[j] = name[j]; j++; }
            kt->name[j] = '\0';

            kt->pid = sched_spawn(name, kthread_wrappers[i]);

            serial_puts("[KTHREAD] Created: ");
            serial_puts(name);
            serial_puts(" (pid=");
            serial_putdec((uint64_t)kt->pid);
            serial_puts(")\n");
            return i;
        }
    }
    return -1;
}

/* Request a kernel thread to stop */
void kthread_stop(int idx)
{
    if (idx < 0 || idx >= KTHREAD_MAX) return;
    kthreads[idx].should_stop = true;
    serial_puts("[KTHREAD] Stop requested: ");
    serial_puts(kthreads[idx].name);
    serial_puts("\n");
}

/* Check if stop was requested (called by the thread itself) */
bool kthread_should_stop(int idx)
{
    if (idx < 0 || idx >= KTHREAD_MAX) return true;
    return kthreads[idx].should_stop;
}

/* List active kernel threads */
void kthread_list(void)
{
    serial_puts("[KTHREAD] Active threads:\n");
    int n = 0;
    for (int i = 0; i < KTHREAD_MAX; i++) {
        if (!kthreads[i].active) continue;
        serial_puts("  ["); serial_putdec((uint64_t)i);
        serial_puts("] "); serial_puts(kthreads[i].name);
        serial_puts(" pid="); serial_putdec((uint64_t)kthreads[i].pid);
        if (kthreads[i].should_stop) serial_puts(" (stopping)");
        serial_puts("\n");
        n++;
    }
    if (n == 0) serial_puts("  (none)\n");
}
