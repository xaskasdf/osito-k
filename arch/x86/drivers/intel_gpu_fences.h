/* Native coherent fence storage, owned independently of public KMT handles. */
#ifndef OSITOK_INTEL_GPU_FENCES_H
#define OSITOK_INTEL_GPU_FENCES_H
#include "intel_gpu_memory.h"
#include "intel_bcs_context.h"
#include "../win32/d3dkmt_registry.h"
#include "../win32/ntsyscall.h"
#define INTEL_GPU_FENCES 32u
/* Private native address window, outside the surface/program windows. The
 * manager never reuses an address, including failed creations. Reservation
 * alone is not a GPU mapping or a public WDDM allocation. */
#define INTEL_GPU_FENCE_VA_BASE 0x400000000ULL
#define INTEL_GPU_FENCE_VA_PAGES 0x100000ULL
struct intel_gpu_fences;
typedef struct {
    struct intel_gpu_fences *owner;
    intel_gpu_memory_coherent_lease_t coherent;
    intel_gpu_coherent_reference_t vm_reference;
    NT_VM_DRIVER_VIEW vm;
    uint64_t id,epoch;
    DWORD pid,device,resource,references;
    bool busy,closing,retained,vm_done,quarantined;
} intel_gpu_fence_t;
typedef struct intel_gpu_fences {
    struct intel_gpu_fences *self;
    intel_gpu_memory_t *memory;
    intel_gpu_fence_t fences[INTEL_GPU_FENCES];
    void (*wake)(void *);
    void (*mapping_wake)(void *);
    void *wake_context;
    uint64_t next_id;
    unsigned lock,cursor,callbacks;
    bool closing;
} intel_gpu_fences_t;
typedef struct intel_gpu_fence_lease {
    struct intel_gpu_fence_lease *self;
    intel_gpu_fences_t *owner;
    intel_gpu_coherent_reference_t reference;
    intel_gpu_buffer_t *buffer;
    void *cpu;
    uint64_t id,epoch,physical,dma,bytes,gpu;
    DWORD pid,device,resource;
} intel_gpu_fence_lease_t;
/* Stable zeroed private storage. Wake is bounded/IRQ-safe, cannot collect
 * synchronously or free the owner. BSP collection revokes VM views and hands
 * closed resources back to native memory; that manager may still retain failed
 * frees. Empty requires shutdown, returned cookies, no users and no callbacks. */
bool intel_gpu_fences_init(intel_gpu_fences_t *,intel_gpu_memory_t *,void (*wake)(void *),void *);
/* Current-process BSP only. One quota-charged page, initial value at offset 0,
 * read-only WB CPU view in the requested ABI. Outputs change only on success.
 * Cookie retention survives logical close/exit; it must be returned with close.
 * No GPU address/capability is fabricated: callers establish a real coherent
 * mapping separately. Public creation needs its own parent admission gate. */
NTSTATUS intel_gpu_fence_create(intel_gpu_fences_t *,DWORD pid,DWORD device,
    uint64_t initial,BOOL compat32,uint64_t *cookie,uint64_t *cpu_address);
/* Public factory backing: x64 CPU view is read-only; x86 is writable for
 * cmpxchg8b atomic reads, as required by the monitored-fence ABI. The GPU VA
 * belongs to the stable per-device window and is installed by the native
 * executor before any subsequently captured GPU work can execute. */
NTSTATUS intel_gpu_fence_create_monitored(intel_gpu_fences_t *,DWORD,DWORD,
    uint64_t initial,BOOL compat32,D3DKMT_MONITORED_BACKING *);
NTSTATUS intel_gpu_fence_close(intel_gpu_fences_t *,DWORD,DWORD,uint64_t cookie);
/* Stable, noncopyable lease. Keeps backing and its owner alive through VM,
 * process and device teardown. Each GPU mapping/use must retain one until
 * independent unbind/retirement/reset proof. This is not submission admission
 * or proof of 64-bit GPU write atomicity. Kernel pointers never go to clients. */
NTSTATUS intel_gpu_fence_acquire(intel_gpu_fences_t *,DWORD,DWORD,uint64_t,intel_gpu_fence_lease_t *);
NTSTATUS intel_gpu_fence_release(intel_gpu_fence_lease_t *);
/* Duplicate a captured use, even after logical close. The source must remain
 * exclusively retained during this call. Does not admit new client work. */
NTSTATUS intel_gpu_fence_retain(const intel_gpu_fence_lease_t *,intel_gpu_fence_lease_t *);
/* Configure an immortal, IRQ-safe global wake hint before work publication.
 * It receives NULL, owns no engine pointer, and cannot collect synchronously. */
bool intel_gpu_fences_watch_mappings(intel_gpu_fences_t *,void (*)(void *));
/* Private capture snapshot, under the manager lock. Retains all live objects
 * in this device's address space. Partial failure leaves count owned leases
 * for the persistent capture collector. Caller supplies zeroed stable slots. */
NTSTATUS intel_gpu_fences_capture(intel_gpu_fences_t *,DWORD,DWORD,uint64_t epoch,
    intel_gpu_fence_lease_t *,unsigned capacity,unsigned *count);
bool intel_gpu_fence_closed(const intel_gpu_fence_lease_t *);
typedef struct intel_gpu_fence_mapping {
    struct intel_gpu_fence_mapping *self;
    intel_gpu_fence_lease_t lease;
    intel_bcs_context_t *context;
    uint64_t queue_id;
} intel_gpu_fence_mapping_t;
/* Sole device executor, stable zeroed storage. Caller supplies the native
 * context's actual memory manager and device identity and holds PAT4 through
 * retirement. Captured uses may survive client close. No guest input or new
 * submission admission is accepted here. On EVERY result, if mapping->self
 * is set, close it before reusing its storage or the native context storage.
 * BUSY/uncertain TLB publication keeps the backing and manager alive.
 * Retarget/destroy must have removed all old references before close can
 * release the mapping lease; ordinary surface rebind preserves it. */
enum intel_bcs_status intel_gpu_fence_map(const intel_gpu_fence_lease_t *,
    intel_bcs_context_t *,intel_gpu_memory_t *,DWORD device,const intel_ppgtt_pat_t *,
    intel_gpu_fence_mapping_t *);
enum intel_bcs_status intel_gpu_fence_unmap(intel_gpu_fence_mapping_t *);
NTSTATUS intel_gpu_fences_collect(intel_gpu_fences_t *);
bool intel_gpu_fences_need_retry(intel_gpu_fences_t *);
void intel_gpu_fences_shutdown(intel_gpu_fences_t *);
bool intel_gpu_fences_empty(intel_gpu_fences_t *);
#endif
