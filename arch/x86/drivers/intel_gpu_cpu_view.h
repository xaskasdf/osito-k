/* Persistent ownership of native KMT CPU leases and process VM references. */
#ifndef OSITOK_INTEL_GPU_CPU_VIEW_H
#define OSITOK_INTEL_GPU_CPU_VIEW_H
#include "intel_gpu_kmt.h"
#include "../win32/ntsyscall.h"
#define INTEL_GPU_CPU_VIEWS 64u
struct intel_gpu_cpu_views;
typedef struct {
    struct intel_gpu_cpu_views *owner;
    intel_gpu_kmt_cpu_lease_t cpu;
    NT_VM_DRIVER_VIEW vm;
    uint64_t ticket;
    DWORD pid;
    OKMT_HANDLE device, allocation;
    bool busy, closing, vm_done, quarantined;
} intel_gpu_cpu_view_t;
typedef struct intel_gpu_cpu_views {
    struct intel_gpu_cpu_views *self;
    intel_gpu_cpu_view_t views[INTEL_GPU_CPU_VIEWS];
    void (*wake)(void *);
    void *wake_context;
    uint64_t next_ticket;
    unsigned lock, cursor, callbacks;
    bool closing;
} intel_gpu_cpu_views_t;

/* Stable zeroed driver-owned storage, never client stack storage. The owner
 * and wake target live through shutdown until callers/collectors quiesce and
 * empty() succeeds. Wake must
 * retain a notification, be IRQ-safe and bounded, and not destroy the owner.
 * No VM, allocator, cache or KMT callback runs under this manager's lock.
 * Acquire/release require BSP and a non-abandonable synchronous caller (KMT
 * cancellation scope for PE callers). After acquire returns, process exit can
 * retire the mapping independently; its callback retains all deferred cleanup.
 * This module neither registers a rendering adapter nor exports runtime APIs.
 */
bool intel_gpu_cpu_views_init(intel_gpu_cpu_views_t *,void (*wake)(void *),void *);
NTSTATUS intel_gpu_cpu_view_acquire(intel_gpu_cpu_views_t *,DWORD pid,
    OKMT_HANDLE device,OKMT_HANDLE allocation,BOOL compat32,BOOL writable,
    uint64_t *ticket,uint64_t *address);
/* Successful or final cache-IO release consumes the ticket. BUSY retains the
 * closing record for retry; the CPU VA may already be revoked. No GPU access
 * can be granted while the native CPU lease remains. */
NTSTATUS intel_gpu_cpu_view_release(intel_gpu_cpu_views_t *,DWORD pid,uint64_t ticket);
/* Validate the entire captured kernel list before revoking any mapping.
 * An error during retirement can consume part of the list; persistent records
 * retain everything that could not be released synchronously. */
NTSTATUS intel_gpu_cpu_views_unlock(intel_gpu_cpu_views_t *,DWORD pid,
    OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *allocations);
/* Retryable cleanup only. Unsafe quarantines remain retained but do not keep
 * an idle worker on a timer. A later safe VM callback supplies a new hint. */
bool intel_gpu_cpu_views_need_retry(intel_gpu_cpu_views_t *);
/* Bounded: one closing record per call. AP collectors handle already-retired
 * VM references only. BSP collection also revokes views requested by shutdown.
 * Inconsistent VM backing is retained indefinitely, so empty() stays false. */
/* BUSY includes a callback still returning: notifications are hints, and a
 * draining owner must retry BUSY (with backoff), even if no new hint arrives. */
NTSTATUS intel_gpu_cpu_views_collect(intel_gpu_cpu_views_t *);
void intel_gpu_cpu_views_shutdown(intel_gpu_cpu_views_t *);
bool intel_gpu_cpu_views_empty(intel_gpu_cpu_views_t *);
#endif
