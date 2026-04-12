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

/* Index of the kthread being launched (set before sched_spawn, read by wrapper) */
static volatile int kthread_launching = -1;

/* Wrapper that calls the actual function with its data argument */
static void kthread_wrapper(void)
{
    int idx = kthread_launching;
    if (idx < 0 || idx >= KTHREAD_MAX || !kthreads[idx].active) return;
    kthreads[idx].func(kthreads[idx].data);
    kthreads[idx].active = false;
}

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

            kthread_launching = i;
            kt->pid = sched_spawn(name, kthread_wrapper);

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
