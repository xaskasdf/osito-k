/* Permanent, detached kernel service for captured native copy work. */
#ifndef OSITOK_INTEL_GPU_EXECUTOR_H
#define OSITOK_INTEL_GPU_EXECUTOR_H
#include "intel_gpu_jobs.h"
#define INTEL_GPU_EXECUTOR_OWNERS 4u

/* Device-owned mapping selector. Called only by this executor, on its joined
 * AP operation (BSP fallback on UP). No user pointers, sleeping or reentry into
 * the executor. Return a stable native context for the captured PID/device and
 * resources. Existing context_rebind supplies exclusive update + acknowledged
 * TLB invalidation. BUSY leaves work owned for retry; other errors fail work.
 * QUARANTINED requires engine shutdown and retains uncertain mappings.
 * Keep all native contexts/bundles and this callback alive through detach. */
typedef enum intel_bcs_status (*intel_gpu_executor_bind_t)(void *,const intel_gpu_capture_t *,
    const intel_bcs_context_t **);
/* Global retained hint; no borrowed owner pointer. Use as jobs.ready; safe
 * before attachment, on BSP/AP/IRQ and after detach. An IRQ is only a hint;
 * real queue completion still proves retirement. Faults call stop(ticket). */
void intel_gpu_executor_notify(void *unused);
NTSTATUS intel_gpu_executor_start(void);
/* BSP task with IRQs enabled. Queue is enabled/published, empty and has no
 * worker/wake sink; jobs have no claims and use notify above. Installs its
 * queue sink and becomes the ONLY take/enqueue/poll/step/shutdown owner.
 * Native bind callbacks own device mapping decisions; no adapter is advertised.
 * Output changes only on success. A permanent detached kernel-CR3 task joins
 * each AP operation before reusing any slot; client exit cannot kill it. */
NTSTATUS intel_gpu_executor_attach(intel_gpu_jobs_t *,intel_bcs_queue_t *,
    intel_gpu_executor_bind_t,void *,bool interrupt_driven,uint64_t *ticket);
/* Optional device maintenance runs on the same joined executor, with no
 * claimed job. BUSY retries on the timer; other failures retire the engine.
 * A global notify wakes it even when no new application work is submitted. */
typedef enum intel_bcs_status (*intel_gpu_executor_maintain_t)(void *);
NTSTATUS intel_gpu_executor_attach_managed(intel_gpu_jobs_t *,intel_bcs_queue_t *,
    intel_gpu_executor_bind_t,intel_gpu_executor_maintain_t,void *,bool,uint64_t *);
/* Nonblocking stop and explicit retry of failed engine retirement. Safe on
 * IRQ/AP. Never frees RAM or assumes a timeout means DMA has stopped. */
NTSTATUS intel_gpu_executor_stop(uint64_t ticket);
/* BSP after stop, native retirement, callback quiescence and jobs.empty.
 * BUSY retains all ownership. Device owner subsequently unpublishes queue,
 * destroys native mappings and detaches its separate cleanup service. Disable
 * hardware IRQ producers before detach; late global hints retain no owner. */
NTSTATUS intel_gpu_executor_detach(uint64_t ticket);
bool intel_gpu_executor_idle(void);
bool intel_gpu_executor_quiescent(void);
#endif
