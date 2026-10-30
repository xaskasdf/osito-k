/* Device-scoped allocation handles over native GPU buffers. Internal API. */
#ifndef OSITOK_INTEL_GPU_MEMORY_H
#define OSITOK_INTEL_GPU_MEMORY_H
#include "intel_gpu_buffer.h"
#define INTEL_GPU_MEMORY_DEVICES 16u
#define INTEL_GPU_MEMORY_RESOURCES 64u
typedef struct {
    uint32_t id, pid, pages, allocations, references;
    uint64_t epoch;
    bool closing, exiting;
} intel_gpu_memory_device_t;
struct intel_gpu_memory_cpu_lease;
struct intel_gpu_memory_coherent_lease;
struct intel_gpu_memory_reclaim_work;
typedef struct {
    intel_gpu_buffer_t buffer;
    intel_gpu_cpu_access_t cpu_access;
    struct intel_gpu_memory_cpu_lease *cpu_holder;
    intel_gpu_coherent_access_t coherent_access;
    struct intel_gpu_memory_coherent_lease *coherent_holder;
    uint32_t id, device, pid, pages, references, pending_work, priority;
    uint32_t residency_refs;
    uint32_t offer_priority;
    struct intel_gpu_memory_reclaim_work *reclaim_owner;
    bool busy, closing, discarded, trim_requested;
} intel_gpu_memory_resource_t;
typedef struct intel_gpu_memory {
    struct intel_gpu_memory *self;
    intel_gpu_allocator_t allocator;
    void *allocator_context;
    intel_gpu_memory_device_t devices[INTEL_GPU_MEMORY_DEVICES];
    intel_gpu_memory_resource_t resources[INTEL_GPU_MEMORY_RESOURCES];
    uint32_t pages, page_limit, process_page_limit, next_id, cursor;
    unsigned lock;
    void (*mapping_wake)(void *);
    bool initialized, closing, registered, reclaiming, trimming;
} intel_gpu_memory_t;

enum { INTEL_GPU_PROPERTY_RESIDENCY=1, INTEL_GPU_PROPERTY_SET_PRIORITY=2,
       INTEL_GPU_PROPERTY_GET_PRIORITY=3 };
/* Unoffered native buffers own pinned shared physical backing until retirement.
 * Offering explicitly allows discard under pressure; the allocation identity
 * survives without backing. General migration is not implemented. All selected
 * resources are validated before any output or priority is changed. */
enum intel_gpu_buffer_status intel_gpu_memory_properties(intel_gpu_memory_t *,
    uint32_t,uint32_t,unsigned,const uint64_t *,uint32_t,uint32_t *);
/* Device residency requirements are separate from lifetime pins, captured DMA
 * leases and CPU mappings. Owned allocations start with one requirement.
 * Make increments each listed occurrence; Evict decrements, saturating at zero.
 * Validate the full batch before mutation. These two residency operations do
 * not allocate, page, migrate or free RAM; explicit offer/reclaim manages
 * discard separately. Existing submitted work keeps its own DMA leases;
 * new capture/admission is denied once the requirement count reaches zero. */
enum intel_gpu_buffer_status intel_gpu_memory_residency(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,bool make,const uint64_t *cookies,uint32_t count);

/* Offer closes new CPU/GPU admission atomically for the entire captured list.
 * Existing work/mappings retain backing and retire normally. Priorities 1..3
 * select lower-valued offered storage first; AUTO (4) uses the current eviction
 * hint relative to NORMAL. Offering alone does not free RAM.
 * Trim makes a bounded pass, honoring every buffer reference, and returns the
 * actual rounded pages released. pid==0 permits all owners; otherwise only
 * that PID's resources can be discarded (including another device/epoch).
 * Allocator operations run unlocked; failed release retains its full charge. */
enum intel_gpu_buffer_status intel_gpu_memory_offer(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,const uint64_t *cookies,uint32_t count,uint32_t priority);
enum intel_gpu_buffer_status intel_gpu_memory_trim(intel_gpu_memory_t *,
    uint32_t pid,uint32_t target_pages,uint32_t *released_pages);
/* Pressure also requests retirement of idle hardware mappings. A stable global
 * executor notification runs unlocked; it never frees pages on the GPU worker.
 * The BSP collector completes the bounded deferred pass after references drop.
 * Reclaim cancels outstanding pressure on that allocation. */
bool intel_gpu_memory_watch_mappings(intel_gpu_memory_t *,void (*)(void *));
bool intel_gpu_memory_mapping_unwanted(intel_gpu_memory_t *,const intel_gpu_buffer_t *);
enum intel_gpu_buffer_status intel_gpu_memory_trim_deferred(intel_gpu_memory_t *,uint32_t *);
/* Reclaim is prepared outside publication locks. Preparation may allocate;
 * partial/OOM results remain offered and retain their actual page charge.
 * The stable, noncopyable work owns all selected resource gates until release.
 * Commit is bounded metadata only, revalidates close/exit, and consumes its
 * single attempt. Discard results remain available for atomic PE publication.
 * Abort/release preserves offered/lost state; successful commit reopens usage
 * after release. A failed prepare releases every gate internally. */
typedef struct intel_gpu_memory_reclaim_work {
    struct intel_gpu_memory_reclaim_work *self;
    intel_gpu_memory_t *memory;
    uint32_t pid,device,count;
    uint64_t selected;
    uint32_t ids[INTEL_GPU_MEMORY_RESOURCES];
    uint8_t slots[1024];
    uint32_t discarded[1024];
    bool attempted;
} intel_gpu_memory_reclaim_work_t;
enum intel_gpu_buffer_status intel_gpu_memory_reclaim_prepare(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,const uint64_t *,uint32_t,intel_gpu_memory_reclaim_work_t *);
enum intel_gpu_buffer_status intel_gpu_memory_reclaim_commit(intel_gpu_memory_reclaim_work_t *);
enum intel_gpu_buffer_status intel_gpu_memory_reclaim_release(intel_gpu_memory_reclaim_work_t *);
typedef struct intel_gpu_memory_lease {
    struct intel_gpu_memory_lease *self;
    intel_gpu_memory_t *memory;
    intel_gpu_buffer_t *buffer;
    uint32_t pid, device, resource;
} intel_gpu_memory_lease_t;

typedef struct intel_gpu_memory_cpu_lease {
    struct intel_gpu_memory_cpu_lease *self;
    intel_gpu_memory_t *memory;
    intel_gpu_buffer_t *buffer;
    uint32_t pid,device,resource;
    void *cpu;
    uint64_t bytes;
} intel_gpu_memory_cpu_lease_t;

typedef struct intel_gpu_memory_coherent_lease {
    struct intel_gpu_memory_coherent_lease *self;
    intel_gpu_memory_t *memory;
    intel_gpu_buffer_t *buffer;
    intel_gpu_coherent_access_t *access;
    uint32_t pid,device,resource;
} intel_gpu_memory_coherent_lease_t;
/* Device-scoped coherent backing. Uses the existing rounded-page quota across
 * all devices/incarnations of a PID. Cache/provenance callbacks run outside the
 * manager lock; publication rechecks logical close/exit after preparation.
 * Rollback retains any unfinished access in the stable resource for collection.
 * The holder keeps its slot/backing charged through close and process exit.
 * Stable noncopyable private storage. The caller establishes coherent GPU
 * mappings; this grants no GPU VA or public monitored-fence capability.
 */
enum intel_gpu_buffer_status intel_gpu_memory_acquire_coherent(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,uint32_t resource,intel_gpu_memory_coherent_lease_t *);
/* Retain a CPU view or GPU use with manager close/exit admission. Use this entry
 * rather than the raw buffer retain when access belongs to a managed resource.
 * Drop through intel_gpu_buffer_drop_coherent only after independent safe VM
 * or physical GPU retirement. Notify the owning service after dropping it.
 */
enum intel_gpu_buffer_status intel_gpu_memory_retain_coherent(intel_gpu_memory_coherent_lease_t *,
    intel_gpu_coherent_reference_t *);
/* BUSY preserves the closing access and its holder. OK/IO consume the lease
 * only after references/residency retire. Release is valid after process/device
 * close. Quiesce callers before final release and wake the deferred collector.
 */
enum intel_gpu_buffer_status intel_gpu_memory_release_coherent(intel_gpu_memory_coherent_lease_t *);

typedef struct intel_gpu_memory_resident_lease {
    struct intel_gpu_memory_resident_lease *self;
    intel_gpu_memory_t *memory;
    intel_gpu_buffer_t *buffer;
    uint32_t pid,device,resource;
    bool pending_work;
} intel_gpu_memory_resident_lease_t;
/* Retain owned backing for command capture/mapping without granting GPU access
 * or excluding CPU access. Noncopyable, stable kernel storage. Logical close
 * denies new retention; BUSY release preserves it. Notify the owning worker
 * after release, and separately acquire GPU access before hardware execution. */
enum intel_gpu_buffer_status intel_gpu_memory_acquire_resident(intel_gpu_memory_t *,
    uint32_t,uint32_t,uint32_t,intel_gpu_memory_resident_lease_t *);
enum intel_gpu_buffer_status intel_gpu_memory_release_resident(intel_gpu_memory_resident_lease_t *);
/* Promote a captured residency lease before publishing queued GPU work. This
 * excludes subsequent managed CPU locks through queue waits and execution.
 * A CPU lease acquired earlier stays valid: execution must wait for its normal
 * release. Internal kernel snapshots still use the exclusive buffer gate.
 * Release the resident lease only after proof of no submission or safe GPU
 * retirement; failed release retains both the hazard and the backing. */
enum intel_gpu_buffer_status intel_gpu_memory_reserve_work(intel_gpu_memory_resident_lease_t *);

/* Private zeroed stable storage. Limits charge rounded surface pages across
 * ALL devices of a PID, including allocation in progress and failed cleanup.
 * Driver-private context/table/HWSP allocations have a separate lifetime.
 * IDs share one namespace and never wrap within this manager lifetime.
 * Allocation pressure may collect a bounded pass of logically closed backing
 * and then discard eligible offered storage, respecting each PID's quota.
 * Live GPU/CPU references and failed cleanup stay charged. Reentrant pressure
 * never starts a nested collection; the caller's device is retained throughout.
 * No allocator/cache/free callback runs under the short IRQ-safe metadata lock. */
bool intel_gpu_memory_init(intel_gpu_memory_t *, const intel_gpu_allocator_t *,
    void *, uint32_t total_pages, uint32_t process_pages);
/* Caller must serialize NEW device admission with process teardown and prove
 * the process is live (future DDI/process admission gate). Existing device
 * handles are safe against concurrent exit; this is not a user-facing API. */
enum intel_gpu_buffer_status intel_gpu_memory_open_device(intel_gpu_memory_t *,
    uint32_t pid, uint32_t *device);
/* KMT/driver ownership: create with one external reference atomically, so an
 * exit notification plus collection cannot reap the slot before publication.
 * Logical close denies new work but keeps the slot until release_device and
 * all resources are reclaimed. Release requires prior logical close, changes
 * metadata only, and must be followed by a deferred collector wake. */
enum intel_gpu_buffer_status intel_gpu_memory_open_device_retained(intel_gpu_memory_t *,
    uint32_t pid, uint32_t *device);
/* Kernel-assigned process incarnation, never a value supplied by a PE client.
 * Admission is serialized with NT teardown; nonzero epochs never repeat.
 * An older exited incarnation keeps its devices/backing closed and charged.
 * Legacy internal callers use epoch zero and preserve PID-only admission. */
enum intel_gpu_buffer_status intel_gpu_memory_open_device_epoch(intel_gpu_memory_t *,
    uint32_t pid,uint64_t epoch,uint32_t *device,bool retained);
enum intel_gpu_buffer_status intel_gpu_memory_device_epoch(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,uint64_t *epoch);
enum intel_gpu_buffer_status intel_gpu_memory_release_device(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device);
enum intel_gpu_buffer_status intel_gpu_memory_create(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device, uint64_t bytes, uint32_t *resource);
/* Atomic external ownership for KMT allocation cookies. On success the native
 * record/backing remains until logical close and release_resource, even if
 * process exit reaches the collector before KMT publishes the cookie. On
 * failure no external reference escapes; partial backing stays collector-owned.
 * This retains storage, not admission or synchronization of GPU accesses. */
enum intel_gpu_buffer_status intel_gpu_memory_create_retained(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device, uint64_t bytes, uint32_t *resource);
enum intel_gpu_buffer_status intel_gpu_memory_release_resource(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device, uint32_t resource);
/* Lease storage starts zeroed, cannot move/copy, and stays private to its
 * holder. Pin exposes a kernel pointer only; ownership/synchronization of GPU
 * writes still comes from the engine/context. Contexts may take an additional
 * buffer pin before releasing a lease. Close retains all such references. */
enum intel_gpu_buffer_status intel_gpu_memory_acquire(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device, uint32_t resource, intel_gpu_memory_lease_t *);
enum intel_gpu_buffer_status intel_gpu_memory_release(intel_gpu_memory_lease_t *);
/* Exclusive CPU ownership backed by the buffer access gate. Private stable
 * zeroed storage; publication rechecks native close/exit after cache sync.
 * The internal access stays collector-owned if rollback cannot finish.
 * OK/IO release consume the lease; BUSY retains it. Kernel address only: the
 * future VM owner must revoke every user mapping before dropping this lease. */
enum intel_gpu_buffer_status intel_gpu_memory_acquire_cpu(intel_gpu_memory_t *,
    uint32_t pid,uint32_t device,uint32_t resource,intel_gpu_memory_cpu_lease_t *);
enum intel_gpu_buffer_status intel_gpu_memory_release_cpu(intel_gpu_memory_cpu_lease_t *);
/* Logical close is metadata-only and immediately denies new acquisitions.
 * Worker collect reclaims at most one allocation, outside the metadata lock.
 * Failed/pinned/quarantined buffers retain their slot and full quota charge. */
enum intel_gpu_buffer_status intel_gpu_memory_close(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device, uint32_t resource);
enum intel_gpu_buffer_status intel_gpu_memory_close_device(intel_gpu_memory_t *,
    uint32_t pid, uint32_t device);
enum intel_gpu_buffer_status intel_gpu_memory_collect(intel_gpu_memory_t *);
/* Bounded service work hint: current closed candidates with no metadata lease,
 * CPU holder or operation in flight. Not a reclamation guarantee: buffer pins,
 * residency, cache failures and concurrent changes can retain them. Zero still
 * permits one collect to reap empty devices. Dropping a blocker must wake the
 * owning service; do not poll this count while waiting for another owner. */
unsigned intel_gpu_memory_collect_budget(intel_gpu_memory_t *);
void intel_gpu_memory_shutdown(intel_gpu_memory_t *);
/* Register stable manager storage for exit notification. Unpublish requires
 * shutdown, release of external device references, and full reclamation.
 * Lock order: registry -> manager -> buffer;
 * no callbacks may acquire manager metadata from a buffer pin/unpin. */
enum intel_gpu_buffer_status intel_gpu_memory_publish(intel_gpu_memory_t *);
enum intel_gpu_buffer_status intel_gpu_memory_unpublish(intel_gpu_memory_t *);
void intel_gpu_memory_release_process(uint32_t pid);
#endif
