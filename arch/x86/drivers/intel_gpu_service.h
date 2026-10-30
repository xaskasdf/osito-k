/* Permanent BSP reclamation service for native Intel driver owners. */
#ifndef OSITOK_INTEL_GPU_SERVICE_H
#define OSITOK_INTEL_GPU_SERVICE_H
#include "intel_gpu_contexts.h"
#include "intel_gpu_fences.h"
struct intel_gpu_jobs;
#define INTEL_GPU_SERVICE_OWNERS 8u
/* Stable callback with no per-owner pointer. Safe on BSP/AP/IRQ and after
 * detach: it only retains a global wake hint. Supply this callback and NULL
 * context to both the KMT provider and its command-context manager. Native
 * engine/resource producers must also notify after dropping a residency/pin,
 * after an operation that blocked collection, or when retrying a failed free.
 * KMT close/release and context VM retirement already send those hints. */
void intel_gpu_service_notify(void *unused);
/* BSP task, IRQs enabled, before public KMT adapter registration. Objects and
 * allocator callbacks remain alive through successful detach. This becomes
 * their ONLY deferred collector; do not collect manually while attached.
 * A failed start can be retried, preserving a previously spawned blocked task.
 * Output changes only on success. No render capability is registered here. */
NTSTATUS intel_gpu_service_attach(intel_gpu_memory_t *,intel_gpu_contexts_t *,uint64_t *ticket);
NTSTATUS intel_gpu_service_attach_jobs(intel_gpu_memory_t *,intel_gpu_contexts_t *,struct intel_gpu_jobs *,uint64_t *ticket);
/* Optional native coherent-fence owner, with the same permanent wake callback.
 * jobs may be NULL; fences is required and must share the memory manager. */
NTSTATUS intel_gpu_service_attach_fences(intel_gpu_memory_t *,intel_gpu_contexts_t *,struct intel_gpu_jobs *,intel_gpu_fences_t *,uint64_t *ticket);
/* Nonblocking BSP detach. Caller first withdraws the KMT adapter and all
 * producer callbacks, shuts down both managers, and unpublishes fully drained
 * native memory. BUSY retains service ownership; never reset/reuse objects.
 * The permanent task has kernel CR3 and no application parent/stack lifetime. */
NTSTATUS intel_gpu_service_detach(uint64_t ticket);
/* Indefinite sleep with no pending wake/retry; not a claim that all owners or
 * resources are empty. Leases and quarantined frees wait for a producer hint. */
BOOL intel_gpu_service_idle(void);
#endif
