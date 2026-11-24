/* Owned native GPU RAM. Kernel-only objects, not CPU pointers from a client. */
#ifndef OSITOK_INTEL_GPU_BUFFER_H
#define OSITOK_INTEL_GPU_BUFFER_H
#include "intel_ggtt.h"
#define INTEL_GPU_BUFFER_MAX_BYTES (256ULL * 1024 * 1024)
typedef struct {
    /* Allocation either returns a complete owned allocation, or no allocation.
     * Validate checks CPU mapping, DMA domain and physical exclusions before
     * the core touches RAM. Release failure means nothing was freed. */
    bool (*allocate)(void *, uint32_t pages, uint64_t *dma, void **cpu);
    bool (*validate)(void *, uint64_t dma, void *cpu, uint32_t pages);
    bool (*release)(void *, uint64_t dma, void *cpu, uint32_t pages);
    bool (*sync)(void *, uint64_t dma, void *cpu, uint32_t pages);
    /* Optional CPU mapping provenance. Certify one contiguous WB PMM span
     * for this owned allocation, excluding guards/device MMIO. DMA may be
     * translated; never infer CPU physical RAM from its bus address. */
    bool (*cpu_physical)(void *, uint64_t dma, void *cpu, uint32_t pages,
                         uint64_t *physical);
} intel_gpu_allocator_t;
struct intel_gpu_cpu_access;
struct intel_gpu_access;
struct intel_gpu_coherent_access;
typedef struct intel_gpu_buffer {
    struct intel_gpu_buffer *self;
    intel_gpu_allocator_t allocator;
    void *allocator_context, *cpu;
    uint64_t owner, bytes, dma;
    uint32_t pages, pins, residents;
    struct intel_gpu_cpu_access *cpu_access;
    struct intel_gpu_access *gpu_access;
    struct intel_gpu_coherent_access *coherent_access;
    unsigned busy;
    bool started, live, validated, ready, closing, discarded;
} intel_gpu_buffer_t;
enum intel_gpu_buffer_status {
    INTEL_GPU_BUFFER_OK, INTEL_GPU_BUFFER_INVALID, INTEL_GPU_BUFFER_BUSY,
    INTEL_GPU_BUFFER_NO_MEMORY, INTEL_GPU_BUFFER_IO, INTEL_GPU_BUFFER_QUARANTINED,
    INTEL_GPU_BUFFER_UNSUPPORTED, INTEL_GPU_BUFFER_NOT_RESIDENT
};
/* Initialize private zeroed storage once. Objects/allocator callbacks cannot
 * move or be reused while referenced. All memory is zeroed, including rounded
 * page padding, before publication. Invalid allocator output is retained and
 * reported as quarantined without accessing or freeing the suspect memory.
 * Operations are nonblocking on contention/reentry, including callbacks.
 * No spinlock or IRQ-off wait spans an allocator or cache operation. */
enum intel_gpu_buffer_status intel_gpu_buffer_create(intel_gpu_buffer_t *,
    const intel_gpu_allocator_t *, void *, uint64_t owner, uint64_t bytes);
enum intel_gpu_buffer_status intel_gpu_buffer_pin(intel_gpu_buffer_t *, uint64_t owner);
enum intel_gpu_buffer_status intel_gpu_buffer_unpin(intel_gpu_buffer_t *, uint64_t owner);
/* Residency retains a stable DMA mapping/backing only. It permits CPU access
 * and never authorizes hardware execution. Structural context mappings hold
 * this reference until acknowledged unbind/reset. */
enum intel_gpu_buffer_status intel_gpu_buffer_reside(intel_gpu_buffer_t *,uint64_t owner);
enum intel_gpu_buffer_status intel_gpu_buffer_unreside(intel_gpu_buffer_t *,uint64_t owner);
typedef struct intel_gpu_access {
    struct intel_gpu_access *self;
    intel_gpu_buffer_t *buffer;
    uint64_t owner;
} intel_gpu_access_t;
/* Exclusive per-job ownership, separate from residency and legacy pins.
 * Stable zeroed storage, never copied. Acquire prepares caches before granting
 * access. The driver may encode/flush data before submitting hardware under
 * this gate. Release requires proof of no submission, completed execution or
 * retired/reset DMA; a timeout alone is not proof. BUSY retains the lease. */
enum intel_gpu_buffer_status intel_gpu_buffer_acquire_gpu(intel_gpu_buffer_t *,
    uint64_t owner,intel_gpu_access_t *);
enum intel_gpu_buffer_status intel_gpu_buffer_release_gpu(intel_gpu_access_t *);
/* Close rejects new pins, residency and access immediately. BUSY retains all
 * bytes while any reference/access remains. Dropping a reference does not free
 * the allocation; the owner/retirement worker retries close
 * to reclaim a deferred allocation. Successful close is idempotent. */
enum intel_gpu_buffer_status intel_gpu_buffer_close(intel_gpu_buffer_t *, uint64_t owner);
/* Offered-allocation backing lifecycle, distinct from logical destruction.
 * The memory manager must first deny new acquisitions and retain the stable
 * object. Discard frees only unreferenced certified backing: pins, GPU mappings,
 * CPU/GPU/coherent accesses and concurrent operations return BUSY unchanged.
 * Failure retains the old backing; success preserves identity/size/allocator.
 * Recommit allocates and zeroes new backing, including page padding. NO_MEMORY
 * leaves it discarded and retryable; failed validation quarantines the new
 * backing, while failed cache preparation retains it for retry/close. The owner
 * must keep all live or quarantined pages charged until actual release.
 * Neither operation grants a public allocation handle or performs implicit
 * GPU eviction. Calls never wait for references or another buffer operation. */
enum intel_gpu_buffer_status intel_gpu_buffer_discard(intel_gpu_buffer_t *,uint64_t owner);
enum intel_gpu_buffer_status intel_gpu_buffer_recommit(intel_gpu_buffer_t *,uint64_t owner);
/* Caller already excludes concurrent CPU/GPU writes. A pin keeps the CPU/DMA
 * addresses stable; sync alone does not grant ownership or imply GPU idle. */
enum intel_gpu_buffer_status intel_gpu_buffer_sync(intel_gpu_buffer_t *, uint64_t owner);
typedef struct intel_gpu_cpu_access {
    struct intel_gpu_cpu_access *self;
    intel_gpu_buffer_t *buffer;
    uint64_t owner;
    void *cpu;
    uint64_t bytes;
} intel_gpu_cpu_access_t;
/* Exclusive full-buffer CPU access, with cache synchronization on both edges.
 * Stable zeroed private storage; never copy. Existing legacy pins and active
 * GPU access exclude CPU access; idle residency does not. CPU access blocks
 * new legacy pins/GPU access, generic sync and reclamation. This is not a user VA or a GPU fence.
 * Stop CPU access/unmap client views before release. OK and IO consume the
 * access; failed final sync leaves the buffer unready for GPU pinning. BUSY
 * keeps ownership and must be retried. No callback runs under a metadata lock. */
enum intel_gpu_buffer_status intel_gpu_buffer_acquire_cpu(intel_gpu_buffer_t *,
    uint64_t owner,intel_gpu_cpu_access_t *);
enum intel_gpu_buffer_status intel_gpu_buffer_release_cpu(intel_gpu_cpu_access_t *);
/* Requires the exclusive CPU lease; output changes only on success. Runs the
 * allocator certificate outside metadata locks. Rounded bytes include already
 * zeroed page padding. This does not grant a new lease or GPU access. */
enum intel_gpu_buffer_status intel_gpu_buffer_cpu_physical(intel_gpu_cpu_access_t *,
    uint64_t *physical, uint64_t *rounded_bytes);

typedef struct intel_gpu_coherent_access {
    struct intel_gpu_coherent_access *self;
    intel_gpu_buffer_t *buffer;
    void *cpu;
    uint64_t owner,physical,dma,bytes;
    uint32_t references;
    bool closing;
} intel_gpu_coherent_access_t;
typedef struct intel_gpu_coherent_reference {
    struct intel_gpu_coherent_reference *self;
    intel_gpu_coherent_access_t *access;
} intel_gpu_coherent_reference_t;
/* Concurrent CPU/GPU access to owned WB PMM, distinct from surface locks.
 * The stable kernel owner must establish I/O-coherent GPU mappings (including
 * cache policy and DMA-domain provenance), serialize writers as its protocol
 * requires, and keep all GPU mappings/CPU views alive through their references.
 * This does not map memory or certify atomic GPU writes. Acquisition rejects
 * prior residency/pins/access, certifies CPU physical RAM independently of DMA,
 * and performs the initial cache handoff before publishing any address.
 * While held, ordinary CPU/GPU leases, pins and explicit cache sync are denied.
 * New residency is permitted solely to retain coherent GPU mappings.
 */
enum intel_gpu_buffer_status intel_gpu_buffer_acquire_coherent(intel_gpu_buffer_t *,
    uint64_t owner,intel_gpu_coherent_access_t *);
/* Each CPU view/active GPU use holds a stable, noncopyable reference. Drop only
 * after safe VM retirement or physical GPU retirement; uncertain removal keeps
 * the reference indefinitely. Calls are nonblocking on contention. */
enum intel_gpu_buffer_status intel_gpu_buffer_retain_coherent(intel_gpu_coherent_access_t *,
    intel_gpu_coherent_reference_t *);
enum intel_gpu_buffer_status intel_gpu_buffer_drop_coherent(intel_gpu_coherent_reference_t *);
/* Denies new references immediately. BUSY retains access until all references
 * and GPU residency have gone. The owner must stop its own CPU/GPU accesses
 * and quiesce concurrent calls on this access before final release. Stable
 * storage alone is not a lifetime reference. OK/IO consume access; IO leaves
 * backing unready. A retained reference protects the access from consumption.
 * Neither timeout nor process exit alone proves safe retirement.
 */
enum intel_gpu_buffer_status intel_gpu_buffer_release_coherent(intel_gpu_coherent_access_t *);
#endif
