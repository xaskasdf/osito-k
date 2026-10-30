/* Native per-process command staging contexts, separate from GPU batch DMA. */
#ifndef OSITOK_INTEL_GPU_CONTEXTS_H
#define OSITOK_INTEL_GPU_CONTEXTS_H
#include "intel_gpu_memory.h"
#include "intel_gpu_fences.h"
#include "../win32/d3dkmt_registry.h"
#include "../win32/ntsyscall.h"
#define INTEL_GPU_CONTEXTS 16u
struct intel_gpu_contexts;
typedef struct {
    struct intel_gpu_contexts *owner;
    intel_gpu_memory_cpu_lease_t cpu;
    NT_VM_DRIVER_VIEW vm;
    uint64_t id,epoch;
    DWORD pid,device,resource,references;
    DWORD node,command_bytes,allocation_count,patch_count,allocation_offset,patch_offset;
    bool busy,closing,retained,vm_done,quarantined;
} intel_gpu_command_context_t;
typedef struct intel_gpu_contexts {
    struct intel_gpu_contexts *self;
    intel_gpu_memory_t *memory;
    intel_gpu_fences_t *fences;
    intel_gpu_command_context_t contexts[INTEL_GPU_CONTEXTS];
    void (*wake)(void *);
    void (*mapping_wake)(void *);
    void *wake_context;
    uint64_t next_id;
    unsigned lock,cursor,callbacks;
    bool closing;
} intel_gpu_contexts_t;
typedef struct intel_gpu_context_lease {
    struct intel_gpu_context_lease *self;
    intel_gpu_contexts_t *owner;
    uint64_t id;
    DWORD pid,device,node;
    void *commands,*allocations,*patches;
    DWORD command_bytes,allocation_count,patch_count;
    uint64_t epoch;
} intel_gpu_context_lease_t;
/* Stable zeroed driver-owned storage, retained independently of the client's
 * stack/CR3. Initialize before KMT registration. A BSP worker must drain both
 * this owner and its native memory manager when wake retains a hint. Wake is
 * bounded and IRQ-safe, must not collect synchronously or free the owner.
 * Empty requires shutdown, every provider cookie returned, leases released,
 * VM callbacks quiescent and context cleanup transferred to native memory.
 * The memory manager can still own failed/quarantined frees after empty(). */
bool intel_gpu_contexts_init(intel_gpu_contexts_t *,intel_gpu_memory_t *,void (*wake)(void *),void *);
/* Configure once, before engine startup or publishing work. Both managers share the same device
 * namespace; their storage remains alive through context/engine retirement. */
bool intel_gpu_contexts_attach_fences(intel_gpu_contexts_t *,intel_gpu_fences_t *);
/* A static IRQ-safe hint callback; no collection inside the callback. The
 * owner and callback remain valid through engine/context retirement. */
bool intel_gpu_contexts_watch_mappings(intel_gpu_contexts_t *,void (*)(void *));
/* Validated private UMD staging protocol; current-process BSP only. Outputs
 * change only on success. CPU mappings stay writable for command construction;
 * these bytes are NEVER directly executable GPU batches. Render must capture
 * and validate commands/handle lists into separate kernel-owned submissions. */
NTSTATUS intel_gpu_context_create(intel_gpu_contexts_t *,DWORD pid,DWORD device,
    const D3DKMT_CONTEXT_DESCRIPTION *,uint64_t *cookie,D3DKMT_CONTEXT_BUFFERS *);
/* Metadata-only close consumes the provider's external cookie retention.
 * Revocation/cache operations and reclamation run in the deferred collector. */
NTSTATUS intel_gpu_context_close(intel_gpu_contexts_t *,DWORD pid,DWORD device,uint64_t cookie);
/* Retain kernel aliases while capturing commands. No pointer from an app is
 * interpreted here. Stable noncopyable lease; acquisition is not GPU execution
 * admission, and submission must recheck close under its own publication gate. */
NTSTATUS intel_gpu_context_acquire(intel_gpu_contexts_t *,DWORD,DWORD,uint64_t,intel_gpu_context_lease_t *);
NTSTATUS intel_gpu_context_release(intel_gpu_context_lease_t *);
NTSTATUS intel_gpu_contexts_collect(intel_gpu_contexts_t *);
bool intel_gpu_contexts_need_retry(intel_gpu_contexts_t *);
void intel_gpu_contexts_shutdown(intel_gpu_contexts_t *);
bool intel_gpu_contexts_empty(intel_gpu_contexts_t *);
/* Unique IDs never revive. A closed/reclaimed staging identity may release its
 * native hardware-cache slot only after independent queue/DMA retirement. */
bool intel_gpu_context_closed(intel_gpu_contexts_t *,DWORD pid,DWORD device,uint64_t id);
#endif
