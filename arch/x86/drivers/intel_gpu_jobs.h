/* Persistent native work and ordered KMT synchronization ownership. */
#ifndef OSITOK_INTEL_GPU_JOBS_H
#define OSITOK_INTEL_GPU_JOBS_H
#include "intel_gpu_capture.h"
#define INTEL_GPU_JOBS 16u
enum intel_gpu_job_phase { INTEL_GPU_JOB_FREE,INTEL_GPU_JOB_PREPARING,INTEL_GPU_JOB_QUEUED,
    INTEL_GPU_JOB_CLAIMED,INTEL_GPU_JOB_RETIRING };
typedef struct {
    intel_gpu_capture_t capture;
    D3DKMT_SYNC_LEASE synchronization;
    uint64_t id;
    DWORD pid;
    OKMT_HANDLE context;
    /* Immutable sorted membership, installed with the PREPARING reservation.
     * Zero count preserves the ordinary single-context representation. */
    DWORD context_count;
    OKMT_HANDLE contexts[OKMT_MAX_BROADCAST_CONTEXTS];
    enum intel_gpu_job_phase phase;
    NTSTATUS result;
    intel_bcs_queue_t *queue;
    uint64_t fence;
    unsigned publication;
    unsigned age; /* successful dispatches bypassing this context head, max 15 */
    bool busy,cancel,report,quarantined,staged,sync;
} intel_gpu_job_t;
typedef struct intel_gpu_jobs {
    struct intel_gpu_jobs *self;
    intel_gpu_contexts_t *contexts;
    intel_gpu_job_t jobs[INTEL_GPU_JOBS];
    void (*ready)(void *);
    void (*done)(void *,uint64_t,DWORD,NTSTATUS);
    void *callback_context;
    uint64_t next_id;
    unsigned lock,cursor,callbacks;
    DWORD execution; /* atomic native executor snapshot, errors survive shutdown */
    DWORD fault; /* atomic sticky adapter loss; normal stop leaves this zero */
    void (*faulted)(void *);
    void *fault_context;
    bool closing;
} intel_gpu_jobs_t;
typedef struct intel_gpu_job_lease {
    struct intel_gpu_job_lease *self;
    intel_gpu_jobs_t *owner;
    const intel_gpu_capture_t *capture;
    uint64_t id;
    DWORD pid;
} intel_gpu_job_lease_t;
/* Zeroed stable driver storage. The executor wake is a retained IRQ-safe hint;
 * completion reports once AFTER capture/resource retirement, outside locks.
 * Callbacks may inspect metadata but must not collect/free/reuse this owner.
 * Keep them and their context alive until shutdown + empty. Attach to the
 * permanent service before publishing a native adapter; service owns collect.
 * A native executor must drain queued work and retain claimed leases until
 * no submission, completed execution or acknowledged reset proves retirement. */
bool intel_gpu_jobs_init(intel_gpu_jobs_t *,intel_gpu_contexts_t *,void (*)(void *),
    void (*)(void *,uint64_t,DWORD,NTSTATUS),void *);
/* Trusted executor only. Metadata publication never proves DMA retirement.
 * Fatal state is sticky for this owner; normal stop cannot hide a fault. */
void intel_gpu_jobs_set_execution(intel_gpu_jobs_t *,DWORD);
/* Trusted terminal engine failure. Notify KMT outside jobs/engine locks and
 * cancel pending work; submitted backing remains owned until retirement.
 * Install the optional callback before executor/adapter publication. */
void intel_gpu_jobs_fail(intel_gpu_jobs_t *,DWORD);
NTSTATUS intel_gpu_jobs_execution(intel_gpu_jobs_t *,DWORD *);
/* Cancel only stable queued/claimed captures of this native device. Preparing
 * work rechecks registry error before publication; submitted DMA stays owned. */
void intel_gpu_jobs_device_error(intel_gpu_jobs_t *,DWORD,uint64_t);
/* Kernel inputs/outputs, not the PE ABI. Failed submission leaves output
 * unchanged and any partial capture collector-owned. Backpressure is BUSY.
 * Publication linearizes with KMT context/device/allocation close and adapter
 * removal. Client cancellation cannot abandon the persistent capture. */
NTSTATUS intel_gpu_jobs_submit(intel_gpu_jobs_t *,DWORD,OKMT_HANDLE,DWORD,DWORD,DWORD,DWORD,uint64_t *);
NTSTATUS intel_gpu_jobs_submit_sync(intel_gpu_jobs_t *,DWORD,OKMT_HANDLE,const D3DKMT_SYNC_COMMAND *,uint64_t *);
DWORD intel_gpu_jobs_pending(intel_gpu_jobs_t *,DWORD,OKMT_HANDLE);
/* Native executor ownership. Only the oldest unfinished job in each context
 * can advance. Metadata waits leave other contexts eligible. take processes
 * a bounded number of synchronization commands before returning a GPU lease.
 * Claim revalidates KMT admission; enqueue below
 * supplies the final admission transaction over owned native mappings. */
NTSTATUS intel_gpu_jobs_take(intel_gpu_jobs_t *,intel_gpu_job_lease_t *);
/* Persistent native executor only, with exclusive access to the stable engine
 * context/mappings. Copies GPU addresses from those owned mappings, reserves a
 * non-executable queue slot, commits KMT admission and finishes publication.
 * Cancellation/close before commit prevents execution. Accepted work can
 * finish after logical close, with backing retained through native retirement.
 * Even a failed enqueue may own a cancelled native reservation; poll it before
 * manual retire/retry. A full queue leaves the claim available for retry.
 * Once reserved, poll owns native-result consumption; do not consume externally.
 * Queue unpublish is blocked until that result is consumed. */
NTSTATUS intel_gpu_jobs_enqueue(intel_gpu_job_lease_t *,const intel_bcs_context_t *);
/* Nonblocking completion: BUSY retains everything; terminal native completion
 * or acknowledged reset transfers capture retirement to the cleanup service
 * and clears the lease. No timeout or missing fence authorizes reclamation. */
NTSTATUS intel_gpu_jobs_poll(intel_gpu_job_lease_t *);
/* Only the trusted executor calls retire after proving no DMA can reference
 * the job. Timeout, app exit and cancellation alone are insufficient proof.
 * The completion callback waits until all capture leases actually release. */
NTSTATUS intel_gpu_jobs_retire(intel_gpu_job_lease_t *,NTSTATUS);
NTSTATUS intel_gpu_jobs_cancel(intel_gpu_jobs_t *,DWORD,uint64_t);
BOOL intel_gpu_jobs_cancelled(const intel_gpu_job_lease_t *);
NTSTATUS intel_gpu_jobs_collect(intel_gpu_jobs_t *);
bool intel_gpu_jobs_need_retry(intel_gpu_jobs_t *);
void intel_gpu_jobs_shutdown(intel_gpu_jobs_t *);
bool intel_gpu_jobs_empty(intel_gpu_jobs_t *);
typedef struct { unsigned queued,claimed,preparing; bool closing; } intel_gpu_jobs_snapshot_t;
NTSTATUS intel_gpu_jobs_snapshot(intel_gpu_jobs_t *,intel_gpu_jobs_snapshot_t *);
#endif
