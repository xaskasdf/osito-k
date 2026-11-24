/* Persistent kernel task over one native BCS queue. No user/DDI ABI. */
#ifndef OSITOK_INTEL_BCS_WORKER_H
#define OSITOK_INTEL_BCS_WORKER_H
#include "intel_bcs_queue.h"
#include "../kernel/sched_signal.h"
typedef struct intel_bcs_worker {
    struct intel_bcs_worker *self;
    intel_bcs_queue_t *queue;
    sched_signal_t wake;
    intel_bcs_queue_wakeup_t notify;
    void *notify_context;
    int pid, task;
    unsigned stop, retry, finished, op_done, shutdown_op;
    unsigned started, sleeps, operations, ap_operations, bsp_operations;
    unsigned irq_sequence, irq_fault, irq_waits, watchdog_wakes;
    bool interrupt_driven;
    enum intel_bcs_status op_status, last_status;
} intel_bcs_worker_t;
/* Stable zeroed driver storage; BSP task caller with IRQs enabled. Queue and
 * pinned bundles already exist and the engine is enabled. The worker becomes
 * their ONLY step/shutdown owner until stop + successful reap. Caller may
 * enqueue/cancel/read results and close contexts concurrently, but must not
 * drive the engine itself or remove the installed wake sink.
 * Optional notify obeys the queue wake contract and lives through reap. Each
 * client must retain its own signal/arm-recheck protocol; a shared consumed
 * event is not a substitute for per-client fence waits. The caller retains
 * the worker task's parent/lifetime until reap (use a persistent driver task).
 * Queue publish/unpublish, context destruction and allocator cleanup remain
 * the device owner's responsibility after the worker is reaped. */
enum intel_bcs_status intel_bcs_worker_start(intel_bcs_worker_t *,
    intel_bcs_queue_t *, intel_bcs_queue_wakeup_t notify, void *context,
    bool interrupt_driven);
/* IRQ-safe retained hint; never treats an interrupt as a completed fence.
 * Disable/detach its hardware source before releasing/reusing this storage. */
void intel_bcs_worker_interrupt(intel_bcs_worker_t *, bool fault);
/* Asynchronous, idempotent stop. A repeated request retries a quarantined
 * engine retirement; it never forces uncertain DMA storage to be released. */
enum intel_bcs_status intel_bcs_worker_stop(intel_bcs_worker_t *);
/* Nonblocking join: PENDING/BUSY retains the entire object. OK means the
 * engine is retired, wake callback detached and native task fully reaped. */
enum intel_bcs_status intel_bcs_worker_reap(intel_bcs_worker_t *);
bool intel_bcs_worker_idle(const intel_bcs_worker_t *);
#endif
