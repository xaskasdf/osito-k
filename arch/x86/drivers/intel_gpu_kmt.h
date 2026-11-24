/* Native Intel device lifetime behind the Microsoft KMT boundary. */
#ifndef OSITOK_INTEL_GPU_KMT_H
#define OSITOK_INTEL_GPU_KMT_H
#include "intel_gpu_memory.h"
#include "../win32/d3dkmt_registry.h"
#include "intel_gpu_contexts.h"

typedef struct intel_gpu_kmt {
    struct intel_gpu_kmt *self;
    intel_gpu_memory_t *memory;
    D3DKMT_DEVICE_PROVIDER device;
    D3DKMT_ALLOCATION_PROVIDER allocation;
    D3DKMT_CONTEXT_PROVIDER context;
    intel_gpu_contexts_t *contexts;
    struct intel_gpu_jobs *jobs;
    D3DKMT_RENDER_PROVIDER render;
    D3DKMT_MONITORED_PROVIDER monitored;
    void (*wake)(void *);
    void *wake_context;
} intel_gpu_kmt_t;

/* Initialize private zeroed stable storage before registration/concurrency.
 * The native manager must already be published for process-exit notification.
 * This does not register an adapter or advertise any rendering capability.
 * Owner retains this object, its memory manager and wake callback until KMT
 * unregister succeeds; resources/pins may require further deferred cleanup.
 * Wake must retain a hint (e.g. sched_signal_set), be IRQ-safe and bounded,
 * and never wait, free storage, or collect synchronously. The owning worker
 * drains logical closes and memory through their independent retire rules. */
bool intel_gpu_kmt_provider(intel_gpu_kmt_t *, intel_gpu_memory_t *,
    void (*wake)(void *), void *wake_context);
/* Attach native staging contexts before registering any KMT adapter. The
 * context owner shares this memory manager and deferred worker wake target;
 * its storage outlives all native cleanup, independently of KMT unregistration.
 * This supplies real CPU command/list views, not Render/Submit or capabilities. */
bool intel_gpu_kmt_context_provider(intel_gpu_kmt_t *,intel_gpu_contexts_t *);
/* Opt-in captured copy submission. Attach the stable job owner and its
 * executor/collector before publishing this provider. No render capability
 * is advertised; native copy validation still rejects other command formats. */
bool intel_gpu_kmt_render_provider(intel_gpu_kmt_t *,struct intel_gpu_jobs *);
bool intel_gpu_kmt_monitored_provider(intel_gpu_kmt_t *);
typedef struct {
    D3DKMT_CONTEXT_LEASE reference;
    intel_gpu_context_lease_t native;
} intel_gpu_kmt_context_lease_t;
NTSTATUS intel_gpu_kmt_acquire_context(DWORD,OKMT_HANDLE,intel_gpu_kmt_context_lease_t *);
NTSTATUS intel_gpu_kmt_release_context(intel_gpu_kmt_context_lease_t *);

typedef struct {
    D3DKMT_DEVICE_LEASE reference;
    intel_gpu_memory_t *memory;
    uint32_t device;
} intel_gpu_kmt_device_lease_t;
/* Private native-driver operation binding from a process-owned public handle.
 * Rejects foreign providers; no unchecked cookie/context casts. Lifetime and
 * cancellation rules are those of D3DKMT_DEVICE_LEASE. GPU resource creation,
 * acquisition and publication must independently honor logical device close. */
NTSTATUS intel_gpu_kmt_acquire_device(DWORD,OKMT_HANDLE,intel_gpu_kmt_device_lease_t *);
NTSTATUS intel_gpu_kmt_release_device(intel_gpu_kmt_device_lease_t *);
typedef struct {
    D3DKMT_ALLOCATION_LEASE reference;
    intel_gpu_memory_cpu_lease_t access;
} intel_gpu_kmt_cpu_lease_t;
/* Native kernel access from a typed allocation, validating both device and
 * allocation providers before interpreting cookies. Holds group/device/native
 * backing through close or process exit. Caller owns stable zeroed storage and
 * must revoke CPU mappings before release; normal cancellation cannot abandon
 * it on a client stack. This grants no user address or runtime Lock export.
 * Successful release and STATUS_UNSUCCESSFUL (cache IO) consume ownership;
 * DEVICE_BUSY retains it. A persistent VM/driver owner must retry cleanup. */
NTSTATUS intel_gpu_kmt_acquire_cpu(DWORD,OKMT_HANDLE,OKMT_HANDLE,intel_gpu_kmt_cpu_lease_t *);
NTSTATUS intel_gpu_kmt_release_cpu(intel_gpu_kmt_cpu_lease_t *);
NTSTATUS intel_gpu_kmt_cpu_physical(intel_gpu_kmt_cpu_lease_t *,
    uint64_t *physical,uint64_t *rounded_bytes);

typedef struct {
    D3DKMT_ALLOCATION_LEASE reference;
    intel_gpu_memory_resident_lease_t native;
} intel_gpu_kmt_resident_lease_t;
NTSTATUS intel_gpu_kmt_acquire_resident(DWORD,OKMT_HANDLE,OKMT_HANDLE,intel_gpu_kmt_resident_lease_t *);
NTSTATUS intel_gpu_kmt_release_resident(intel_gpu_kmt_resident_lease_t *);
#endif
