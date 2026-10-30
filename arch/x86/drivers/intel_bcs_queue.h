/* Kernel-owned copy-work queue over a native Gen9 BCS or RCS engine. */
#ifndef OSITOK_INTEL_BCS_QUEUE_H
#define OSITOK_INTEL_BCS_QUEUE_H
#include "intel_bcs.h"
#include "intel_rcs_draw.h"
#include "intel_rcs_sampled.h"
#include "intel_rcs_vertex.h"

/* Kernel-owned snapshot. No borrowed program/vertex CPU pointer enters the
 * queue. The mapping owner retains geometry, program and all unique sampled
 * allocations through retirement. ABI3 selects the sampled descriptor. */
typedef struct {
    union { intel_rcs_primitive_draw_t primitive; intel_rcs_sampled_draw_t sampled; };
    struct intel_d3d9_program program;
    struct intel_d3d9_vertex_program vertex_program; /* ABI zero disables VS. */
    intel_rcs_vertex_binding_t vertex;
    intel_rcs_index_binding_t indices; /* GPU zero disables indexed fetch. */
    unsigned scissor_count;
    intel_rcs_scissor_t scissors[INTEL_RCS_MAX_SCISSORS];
} intel_rcs_queue_draw_t;
typedef bool (*intel_rcs_queue_prepare_t)(void *,const intel_rcs_queue_draw_t *,
    uint64_t,uint64_t *,uint32_t *);

#define INTEL_BCS_QUEUE_CONTEXTS 8u
#define INTEL_BCS_QUEUE_JOBS 32u

enum intel_bcs_job_phase {
    INTEL_BCS_JOB_EMPTY, INTEL_BCS_JOB_STAGED, INTEL_BCS_JOB_QUEUED, INTEL_BCS_JOB_PREPARING, INTEL_BCS_JOB_RUNNING,
    INTEL_BCS_JOB_COMPLETE, INTEL_BCS_JOB_CANCELLED, INTEL_BCS_JOB_FAILED,
    INTEL_BCS_JOB_ABORTED
};
typedef struct {
    enum intel_bcs_job_phase phase;
    enum intel_bcs_status error;
} intel_bcs_job_result_t;
enum intel_bcs_queue_event {
    INTEL_BCS_EVENT_WORK = 1u,
    INTEL_BCS_EVENT_RESULT = 2u,
    INTEL_BCS_EVENT_FAULT = 4u,
    INTEL_BCS_EVENT_STATE = 8u,
    INTEL_BCS_EVENT_SPACE = 16u,
    INTEL_BCS_EVENT_ALL = 31u
};
typedef void (*intel_bcs_queue_wakeup_t)(void *, uint32_t events);
typedef struct {
    uint32_t queued, active, terminal, staged;
    enum intel_bcs_status fault;
    bool closing, stopped, worker_busy;
} intel_bcs_queue_snapshot_t;
typedef struct {
    /* CPU-only: validate the fixed owned mappings, encode/flush the saved
     * context and ring. No MMIO submission, user pointers or worker reentry.
     * Called only when the engine has no outstanding job. */
    bool (*prepare)(void *, const intel_bcs_copy_t *, uint64_t token,
                    uint64_t *descriptor, uint32_t *tail);
    bool (*token_read)(void *, uint64_t *);
    /* Optional pair for per-job surface ownership, separate from structural
     * residency. Acquire runs before prepare/MMIO. Every attempted acquire,
     * including partial failure, is paired with retire after no submission,
     * proven completion or successful engine reset. BUSY acquisition retries
     * the same queued fence after retirement; other errors fail the job.
     * Retire is idempotent and bounded: BUSY retains ownership and the active
     * job for retry, other errors quarantine. No callback runs under a lock.
     * Neither callbacks nor their private storage may be abandoned on exit. */
    enum intel_bcs_status (*acquire)(void *,uint64_t token);
    enum intel_bcs_status (*retire)(void *,uint64_t token);
} intel_bcs_queue_ops_t;
typedef struct {
    void *backing;
    uint64_t owner, id, epoch;
    uint64_t replacement_owner;
    uint32_t references;
    bool closing, published, process_exited, updating;
    bool replacement_exited;
} intel_bcs_queue_context_t;
typedef struct {
    unsigned *state;
    unsigned pending,accepted,rejected;
} intel_bcs_queue_gate_t;

typedef struct {
    uint64_t owner, context, fence;
    intel_bcs_copy_t copy;
    intel_rcs_queue_draw_t draw;
    intel_bcs_job_result_t result;
    intel_bcs_job_result_t retired_result;
    intel_bcs_queue_gate_t gate;
    bool access_started, retiring, retry,retained,is_draw;
} intel_bcs_queue_job_t;
typedef struct {
    intel_bcs_state_t *engine;
    intel_gt_io_t io;
    intel_bcs_queue_ops_t ops;
    intel_rcs_queue_prepare_t prepare_draw;
    void *engine_context;
    intel_bcs_queue_context_t contexts[INTEL_BCS_QUEUE_CONTEXTS];
    intel_bcs_queue_job_t jobs[INTEL_BCS_QUEUE_JOBS];
    uint64_t next_context, next_fence;
    uint32_t active; /* JOBS means none */
    enum intel_bcs_status fault;
    unsigned lock;
    intel_bcs_queue_wakeup_t wakeup;
    void *wakeup_context;
    uint32_t events, notifying;
    bool initialized, closing, stopped, worker_busy, registered;
} intel_bcs_queue_t;

/* Metadata operations are protected by a short IRQ-safe lock. Only one
 * worker may touch engine state; competing step/shutdown calls return BUSY.
 * No metadata lock spans callbacks, MMIO, cache flushes, polling or reset.
 * This is not yet a user/DDI API or an IRQ worker. Initialize private zeroed storage once
 * after enabling a fresh engine. Do not separately submit/close that engine.
 * IDs/fences never wrap or alias stale entries during the queue lifetime.
 * The initial fence is nonzero, unique to this engine session and not MAX. */
enum intel_bcs_status intel_bcs_queue_init(intel_bcs_queue_t *,
    intel_bcs_state_t *, const intel_gt_io_t *, void *engine_context,
    const intel_bcs_queue_ops_t *, uint64_t first_fence);
/* RCS-only initialization; acquire/retire are mandatory. A null copy prepare
 * callback creates a draw-only queue. Initialization precedes publication. */
enum intel_bcs_status intel_bcs_queue_init_draw(intel_bcs_queue_t *,
    intel_bcs_state_t *,const intel_gt_io_t *,void *,const intel_bcs_queue_ops_t *,
    intel_rcs_queue_prepare_t,uint64_t);

/* Attach transfers lifetime responsibility for a stable, resident bundle:
 * context/ring, HWSP, page tables and all mapped surfaces. The kernel caller
 * validates DMA ownership/permissions beforehand. Keep even an idle/closed
 * bundle alive until release_context succeeds AFTER engine shutdown. This
 * deliberately also retains the bundle containing the global HWSP.
 * Only update_context may replace surface mappings and release their old
 * residency after invalidation; the structural bundle remains resident until reset.
 * A bundle may be attached once. owner is a nonzero Win32 process identity;
 * the caller must hold a live process reference when admitting new contexts.
 * MMIO callbacks receive the active bundle, or engine_context at shutdown. */
enum intel_bcs_status intel_bcs_queue_attach(intel_bcs_queue_t *, uint64_t owner,
    void *backing, uint64_t *context);
/* Nonzero epoch is an NT-assigned incarnation captured by its native device.
 * Exit notification is synchronous before PID readmission; deferred work
 * keeps its original closed context and unique context/fence IDs. An epoch
 * never clears a previous incarnation's exit mark or frees its DMA bundle. */
enum intel_bcs_status intel_bcs_queue_attach_epoch(intel_bcs_queue_t *,uint64_t owner,
    uint64_t epoch,void *backing,uint64_t *context);
enum intel_bcs_status intel_bcs_queue_enqueue(intel_bcs_queue_t *, uint64_t owner,
    uint64_t context, const intel_bcs_copy_t *, uint64_t *fence);
/* Two-phase kernel admission. Stage reserves a non-executable job and retains
 * its result across process exit. The stable, aligned state word belongs to a
 * persistent driver owner. External admission CASes pending->accepted while
 * serialized with its own close rules. Queue close/cancel competes with
 * pending->rejected; no callback or foreign lock is taken under the queue lock.
 * Finish-stage detaches the state word and either queues accepted work or
 * reports cancellation. PENDING returns BUSY and keeps the word attached.
 * Always finish a successful stage, including external admission failure,
 * before releasing/reusing the word. Failed stage leaves fence untouched.
 * Explicitly consume its terminal result before unpublish, even after exit.
 * Accepted work can finish after external logical close; caller retains all
 * mappings/backing until native terminal completion or proven reset. */
enum intel_bcs_status intel_bcs_queue_stage(intel_bcs_queue_t *,uint64_t,uint64_t,
    const intel_bcs_copy_t *,const intel_bcs_queue_gate_t *,uint64_t *);
/* Same admission, cancellation and retirement rules as copies. The complete
 * descriptor/header is captured by value before enqueue returns. Metadata
 * validation belongs to prepare, before MMIO. This is not a public shader ABI. */
enum intel_bcs_status intel_bcs_queue_enqueue_draw(intel_bcs_queue_t *,uint64_t,uint64_t,
    const intel_rcs_queue_draw_t *,uint64_t *);
enum intel_bcs_status intel_bcs_queue_stage_draw(intel_bcs_queue_t *,uint64_t,uint64_t,
    const intel_rcs_queue_draw_t *,const intel_bcs_queue_gate_t *,uint64_t *);
enum intel_bcs_status intel_bcs_queue_finish_stage(intel_bcs_queue_t *,uint64_t,uint64_t,unsigned *);
/* Kernel-only mapping transaction, mutually exclusive with step/shutdown and
 * other updates. Admission requires no active engine job and no queued work
 * for this context. Its enqueue returns BUSY during the callback; other
 * contexts can still enqueue. Close/exit may mark it closing concurrently.
 * The callback runs WITHOUT metadata locks, receives the immutable bundle as
 * its MMIO context, and must finish boundedly without waiting for this queue.
 * INVALID/BUSY must leave mappings unchanged and retain no new references.
 * Any failure after mutation must return QUARANTINED, retain all DMA backing,
 * and rely on shutdown before cleanup. Queue faults stop further dispatch.
 * Success includes an acknowledged hardware TLB invalidation, not just a CPU
 * cache flush. The callback owns all engine access for its duration. */
typedef enum intel_bcs_status (*intel_bcs_queue_update_t)(void *backing,
    void *argument, const intel_gt_io_t *, intel_bcs_state_t *);
enum intel_bcs_status intel_bcs_queue_update_context(intel_bcs_queue_t *,
    uint64_t owner, uint64_t context, intel_bcs_queue_update_t, void *argument);
/* Trusted removal-only callback under the same idle-engine gate. Logical
 * context close/exit does not prevent removing translations after prior work
 * retires. It grants no new mapping/submission or renewed context identity.
 * Engine/queue fault or shutdown still requires ordinary teardown instead. */
enum intel_bcs_status intel_bcs_queue_remove_context_mappings(intel_bcs_queue_t *,
    uint64_t owner,uint64_t context,intel_bcs_queue_update_t,void *argument);
/* Replace a CLOSED logical identity in the same retained hardware bundle.
 * No active engine work, references or unread results may remain. The bundle
 * itself, its hardware ID, state/ring and GGTT mappings must stay fixed.
 * The callback follows update_context's mutation/retention contract and must
 * acknowledge TLB invalidation before dropping old surface residency.
 * Concurrent exit is tracked for both old and replacement owners. Success
 * assigns a fresh nonwrapping queue ID, preserving replacement exit/close.
 * Failure keeps the old logical ID; quarantine requires engine shutdown.
 * This does not authorize freeing any hardware context storage online. */
enum intel_bcs_status intel_bcs_queue_retarget_context(intel_bcs_queue_t *,
    uint64_t owner,uint64_t context,uint64_t replacement_owner,uint64_t epoch,
    intel_bcs_queue_update_t,void *argument,uint64_t *replacement_context);
/* Results persist until explicitly consumed. Full queues apply backpressure;
 * completed/unread results of live owners are never overwritten. Terminal
 * results of exited owners are reclaimed on admission/worker steps. Pending results cannot be
 * consumed. A completed fence permits data reuse, not context destruction. */
enum intel_bcs_status intel_bcs_queue_result(intel_bcs_queue_t *, uint64_t owner,
    uint64_t fence, bool consume, intel_bcs_job_result_t *);
/* Queued work is cancelled without touching hardware. Preparation/running work returns
 * BUSY: cancellation cannot promise to undo GPU writes already in progress.
 * Closing a context cancels queued/preparing work (after the CPU callback
 * returns) and lets submitted work finish. Closed contexts cannot be reused. */
enum intel_bcs_status intel_bcs_queue_cancel(intel_bcs_queue_t *, uint64_t owner,
    uint64_t fence);
enum intel_bcs_status intel_bcs_queue_close_context(intel_bcs_queue_t *,
    uint64_t owner, uint64_t context);
enum intel_bcs_status intel_bcs_queue_release_context(intel_bcs_queue_t *,
    uint64_t owner, uint64_t context);
/* One bounded check OR one FIFO dispatch; never both. The caller can observe
 * completion/cache-sync data before the next dispatch. Errors poison dispatch
 * and leave uncertain work pending until shutdown proves DMA retirement.
 * BUSY preserves work but generates no hint just for retrying: the caller
 * must arrange a bounded timer/resource wait, never depend on self-wakes. */
enum intel_bcs_status intel_bcs_queue_step(intel_bcs_queue_t *);
/* Thread-context, bounded engine retirement. Cancels queued work; active work
 * becomes ABORTED/FAILED only after retirement. A failed reset retains all
 * bundles/results and may be retried; it must never authorize RAM release. */
enum intel_bcs_status intel_bcs_queue_shutdown(intel_bcs_queue_t *);

/* One kernel-owned event sink, installed before starting its consumer. The
 * initial synchronous ALL notification closes the install-before-wait gap.
 * Every notification runs AFTER queue/registry locks have been released.
 * Callbacks may overlap and arrive out of order on BSP/AP/IRQ callers. They
 * must be bounded, IRQ-safe, and only signal deferred work (no GPU MMIO, waits,
 * scheduler-table access from APs, or queue mutations). Snapshot/result peek
 * are allowed. Events are wake hints, never completion or DMA-release proof:
 * the consumer must recheck queue predicates/results after each wake and use
 * a retained signal or an atomic arm/recheck protocol before sleeping.
 * Job results remain owned by the submitting process until consumed.
 * Keep queue, callback code and context stable until unwatch succeeds; BUSY
 * leaves the sink attached. Shutdown does not wait for callbacks. Unpublish
 * also requires unwatch, so teardown cannot release an in-flight callback. */
enum intel_bcs_status intel_bcs_queue_watch(intel_bcs_queue_t *,
    intel_bcs_queue_wakeup_t, void *);
enum intel_bcs_status intel_bcs_queue_unwatch(intel_bcs_queue_t *);
enum intel_bcs_status intel_bcs_queue_snapshot(intel_bcs_queue_t *,
    intel_bcs_queue_snapshot_t *);

/* Register the queue for Win32 process-exit notifications. Storage and
 * callbacks remain kernel-owned through successful unpublish, which requires
 * completed engine shutdown. Registry lock order is registry -> queue only.
 * Process exit never performs MMIO, waits for a worker, or releases DMA RAM;
 * it closes all matching contexts and rejects late admission for that owner.
 * The device worker must still drain/retire and reclaim the closed bundles. */
enum intel_bcs_status intel_bcs_queue_publish(intel_bcs_queue_t *);
enum intel_bcs_status intel_bcs_queue_unpublish(intel_bcs_queue_t *);
void intel_bcs_release_process(uint32_t process_id);
#endif
