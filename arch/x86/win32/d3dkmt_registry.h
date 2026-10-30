#ifndef OSITO_D3DKMT_REGISTRY_H
#define OSITO_D3DKMT_REGISTRY_H

#include "nttypes.h"
#include "d3dkmt_abi.h"

#define D3DKMT_MAX_ADAPTERS 16u
#define D3DKMT_MAX_HANDLES 1024u
#define D3DKMT_MAX_ADAPTER_PRIVATE_BYTES 128u
#define D3DKMT_STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define D3DKMT_STATUS_DEVICE_REMOVED ((NTSTATUS)0xC00002B6)
#define D3DKMT_STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define D3DKMT_STATUS_DEVICE_ERROR ((NTSTATUS)0xC01E0200)

/* Kernel-owned discovery data. Registration is a driver readiness boundary:
 * register only a working native driver, with its real capabilities and UMD
 * paths. GOP scanout and an unqualified compute channel are not render DDIs.
 * Data is copied; no caller pointers are retained. No adapters are synthesized.
 */
typedef struct {
    DWORD source_count;
    DWORD precise_present_regions;
    DWORD driver_version;
    DWORD adapter_type;
    OKMT_QUERY_DEVICE_IDS device_ids;
    OKMT_WCHAR umd32[260];
    OKMT_WCHAR umd64[260];
    DWORD private_bytes;
    BYTE private_data[D3DKMT_MAX_ADAPTER_PRIVATE_BYTES];
    /* Native device namespace, optional for adapters with no named device.
     * Distinct from a GDI display name and never implies scanout ownership. */
    OKMT_WCHAR device_name[260];
} D3DKMT_ADAPTER_DESCRIPTION;

NTSTATUS d3dkmt_register_adapter(const D3DKMT_ADAPTER_DESCRIPTION *, OKMT_LUID *);
/* Native driver's kernel-device lifetime, separate from its UMD DDI device.
 * Called without registry locks, including rollback and process exit. Both
 * callbacks must be bounded metadata operations: no MMIO, waits or DMA free.
 * open must atomically admit a live process (Intel: nt_process_open_gpu_device_retained)
 * and return no allocation on failure. close logically releases the device;
 * the driver worker retains GPU resources until their verified retirement.
 * A failed close retains the cookie and is retried through collect_devices.
 * Callbacks/context must remain stable until unregister returns SUCCESS. */
typedef struct {
    NTSTATUS (*open)(void *, DWORD pid, uint64_t *cookie);
    NTSTATUS (*close)(void *, DWORD pid, uint64_t cookie);
    void *context;
    /* Optional bounded snapshot, outside registry locks with device retained.
     * Only real native execution state; no MMIO, waits or client pointers. */
    NTSTATUS (*execution)(void *,DWORD,uint64_t,DWORD *);
    /* Registry already rejected future execution. Cancel this device's queued
     * work and wake its executor, unlocked and with the provider retained.
     * Bounded metadata only; never reset an engine or release active DMA. */
    void (*error)(void *,DWORD,uint64_t);
    /* Optional stable, aligned atomic snapshot owned by the trusted driver:
     * 0 healthy; execution error 2..7 after loss. Publish before notifying
     * d3dkmt_report_driver_error. Storage outlives all registered references.
     * Also closes admission if fault publication races adapter registration. */
    const DWORD *fault_state;
} D3DKMT_DEVICE_PROVIDER;
NTSTATUS d3dkmt_report_driver_error(const D3DKMT_DEVICE_PROVIDER *);
NTSTATUS d3dkmt_query_device_execution(DWORD,OKMT_HANDLE,DWORD *);
NTSTATUS d3dkmt_mark_device_error(DWORD,OKMT_HANDLE,DWORD);
NTSTATUS d3dkmt_register_driver(const D3DKMT_ADAPTER_DESCRIPTION *,
    const D3DKMT_DEVICE_PROVIDER *, OKMT_LUID *);
/* A retained native fence backing. value is a kernel address, never copied to
 * a PE output. The provider cookie owns it until close succeeds; captures keep
 * the registry object and its parent alive across logical destroy. */
typedef struct {
    uint64_t cookie,cpu_address,gpu_address;
    volatile uint64_t *value;
} D3DKMT_MONITORED_BACKING;
typedef struct {
    NTSTATUS (*create)(void *,DWORD,uint64_t,uint64_t,BOOL,D3DKMT_MONITORED_BACKING *);
    NTSTATUS (*close)(void *,DWORD,uint64_t,uint64_t);
    /* Stable wake hint, invoked unlocked with the device retained. */
    void (*notify)(void *);
} D3DKMT_MONITORED_PROVIDER;
/* Kernel memory backend, after the KMD has validated/translated UMD layout.
 * Not a PE ABI or a substitute runtime. These are owned buffers; the registry
 * groups them by resource. Importing client memory and sharing need separate DDIs.
 * allocate returns a retained native cookie or no cookie on failure. Any
 * partial native allocation on failure remains owned by its native collector.
 * close is metadata-only; it drops that retention, schedules cleanup and
 * returns success only after ownership has passed to the native collector.
 * Both callbacks execute outside registry locks. context is the device
 * provider's context and stays alive until unregister succeeds. */
typedef struct { uint64_t bytes; } D3DKMT_ALLOCATION_DESCRIPTION;
#define D3DKMT_MAX_ALLOCATION_PRIVATE_BYTES 256u
typedef struct {
    DWORD bytes;
    BYTE data[D3DKMT_MAX_ALLOCATION_PRIVATE_BYTES];
} D3DKMT_PRIVATE_ALLOCATION;
/* Prepared offer/reclaim provider. Storage and captures are private, stable and
 * retained until finish. Prepare may allocate/cache-sync, unlocked. Commit is
 * bounded metadata only, with no callbacks/allocation/free/wait/MMIO; it may run
 * in the PE publication section. Finish is always unlocked, including abort.
 * A failed prepare must release its own partial transaction. Retained partial
 * backing may remain collector-owned. Results use 0=preserved, 1=discarded.
 * This interface retains commitment; it never reports NOT_COMMITTED. */
#define D3DKMT_OFFER_STORAGE_BYTES 8192u
enum { D3DKMT_OFFER=0, D3DKMT_RECLAIM=1, D3DKMT_RECLAIM2=2 };
typedef struct {
    NTSTATUS (*prepare)(void *,DWORD,uint64_t,DWORD,DWORD,DWORD,const uint64_t *,DWORD,void *,DWORD *);
    NTSTATUS (*commit)(void *);
    void (*finish)(void *);
} D3DKMT_OFFER_PROVIDER;
typedef struct {
    NTSTATUS (*allocate)(void *,DWORD,uint64_t,const D3DKMT_ALLOCATION_DESCRIPTION *,uint64_t *);
    NTSTATUS (*close)(void *,DWORD,uint64_t,uint64_t);
    /* Optional pure validator/translator over captured kernel bytes. Never
     * retain input, allocate, wait or execute client code. */
    NTSTATUS (*translate)(void *,const void *,DWORD,D3DKMT_ALLOCATION_DESCRIPTION *);
    /* Optional bounded metadata transaction over retained backing cookies.
     * Query returns actual residency; set stores eviction priority hints.
     * Validate the complete batch before changing any priority. No callbacks,
     * MMIO, waits or allocations under provider locks; no caller pointers.
     * Cookies and values are private stable captures, not retained on return. */
    NTSTATUS (*properties)(void *,DWORD,uint64_t,DWORD,const uint64_t *,DWORD,DWORD *);
    /* Optional immediate residency commit on already owned physical backing.
     * Caller retains every cookie. Validate the entire batch before mutation;
     * any failure must leave it unchanged. Synchronous bounded metadata ONLY:
     * no allocation, VM, callbacks, I/O, MMIO, waiting or retained input. This
     * callback may run with BSP interrupts disabled for atomic PE publication.
     * Paging/migration requires a separate prepared asynchronous transaction;
     * never hide such work in this immediate commit interface. */
    NTSTATUS (*residency)(void *,DWORD,uint64_t,BOOL,const uint64_t *,DWORD);
    const D3DKMT_OFFER_PROVIDER *offer;
} D3DKMT_ALLOCATION_PROVIDER;

/* Caller captures inputs[] before prepare. All arrays have bounded capacity;
 * groups are retained/busy until release, allocations hold independent leases.
 * Duplicate resources map to the same captured members. Quiesce commit callers
 * before release; a work object cannot move, be copied or be reused while live. */
typedef struct d3dkmt_offer_work {
    struct d3dkmt_offer_work *self;
    DWORD pid,count,allocation_count,group_count,mode,priority,options;
    OKMT_HANDLE device,queue;
    BOOL resources,attempted,prepared;
    uint64_t device_cookie;
    void *provider_context;
    const D3DKMT_OFFER_PROVIDER *provider;
    OKMT_HANDLE inputs[D3DKMT_MAX_HANDLES],handles[D3DKMT_MAX_HANDLES];
    OKMT_HANDLE groups[D3DKMT_MAX_HANDLES],membership[D3DKMT_MAX_HANDLES];
    uint64_t cookies[D3DKMT_MAX_HANDLES];
    DWORD values[D3DKMT_MAX_HANDLES],results[D3DKMT_MAX_HANDLES];
    uint64_t storage[D3DKMT_OFFER_STORAGE_BYTES/sizeof(uint64_t)];
} D3DKMT_OFFER_WORK;
NTSTATUS d3dkmt_offer_prepare(DWORD,OKMT_HANDLE,BOOL,DWORD,DWORD,DWORD,DWORD,D3DKMT_OFFER_WORK *);
NTSTATUS d3dkmt_offer_commit(D3DKMT_OFFER_WORK *);
NTSTATUS d3dkmt_offer_release(D3DKMT_OFFER_WORK *);

/* Kernel-owned stable storage, never copy while prepared. Handles/cookies may
 * repeat; each occurrence owns a reference. Only release may drop ownership.
 * Prepare/release are outside PE copy sections; commit is bounded metadata. */
typedef struct d3dkmt_residency_work {
    struct d3dkmt_residency_work *self;
    DWORD pid,count;
    OKMT_HANDLE device,queue;
    BOOL make,attempted;
    uint64_t device_cookie;
    void *provider_context;
    NTSTATUS (*commit)(void *,DWORD,uint64_t,BOOL,const uint64_t *,DWORD);
    OKMT_HANDLE handles[D3DKMT_MAX_HANDLES];
    uint64_t cookies[D3DKMT_MAX_HANDLES];
} D3DKMT_RESIDENCY_WORK;
NTSTATUS d3dkmt_residency_prepare(DWORD,OKMT_HANDLE,BOOL,DWORD,D3DKMT_RESIDENCY_WORK *);
NTSTATUS d3dkmt_residency_commit(D3DKMT_RESIDENCY_WORK *);
NTSTATUS d3dkmt_residency_release(D3DKMT_RESIDENCY_WORK *);
enum { D3DKMT_PROPERTY_RESIDENCY=1, D3DKMT_PROPERTY_SET_PRIORITY=2 };
NTSTATUS d3dkmt_register_memory_driver(const D3DKMT_ADAPTER_DESCRIPTION *,
    const D3DKMT_DEVICE_PROVIDER *,const D3DKMT_ALLOCATION_PROVIDER *,OKMT_LUID *);
#define D3DKMT_MAX_CONTEXT_PRIVATE_BYTES 256u
typedef struct {
    DWORD node,engine,flags,hint,private_bytes;
    BOOL compat32;
    BYTE private_data[D3DKMT_MAX_CONTEXT_PRIVATE_BYTES];
} D3DKMT_CONTEXT_DESCRIPTION;
typedef struct {
    uint64_t commands,allocations,patches,gpu_commands;
    DWORD command_bytes,allocation_count,patch_count;
} D3DKMT_CONTEXT_BUFFERS;
typedef struct { DWORD offset,bytes,allocations,patches; } D3DKMT_RENDER_DESCRIPTION;
#define D3DKMT_MAX_SYNC_OBJECTS 32u
typedef struct {
    DWORD type;
    uint64_t initial;
    DWORD maximum;
} D3DKMT_SYNC_DESCRIPTION;
/* Kernel-only provider. PE input can never supply callbacks. References are
 * acquired outside registry locks and signaled/released by the BSP collector,
 * after the operation has committed at the head of its context stream. */
typedef struct {
    NTSTATUS (*retain)(DWORD,uint64_t,PVOID *);
    void (*signal)(PVOID,DWORD);
    void (*release)(PVOID);
} D3DKMT_SYNC_EVENT_PROVIDER;
typedef struct {
    DWORD count;
    OKMT_HANDLE objects[D3DKMT_MAX_SYNC_OBJECTS];
    uint64_t value;
    BOOL signal,extended;
    BOOL vector_values;
    uint64_t values[D3DKMT_MAX_SYNC_OBJECTS];
    uint64_t event_handle;
    const D3DKMT_SYNC_EVENT_PROVIDER *events;
    BOOL allow_rewind;
    DWORD broadcast_count; /* zero = primary context only; otherwise includes primary */
    OKMT_HANDLE broadcast[OKMT_MAX_BROADCAST_CONTEXTS];
} D3DKMT_SYNC_COMMAND;
struct d3dkmt_context_lease;
/* Called outside registry locks with a retained context/device. Capture and
 * validate staging into driver-owned storage before accepting work. SUCCESS
 * means admission, never GPU completion. Accepted work must outlive this
 * call and process exit. No user pointer, MMIO or GPU wait crosses this API.
 * Driver admission must still linearize against logical close/removal. */
typedef struct {
    NTSTATUS (*submit)(const struct d3dkmt_context_lease *,const D3DKMT_RENDER_DESCRIPTION *,DWORD *);
    NTSTATUS (*synchronize)(const struct d3dkmt_context_lease *,const D3DKMT_SYNC_COMMAND *);
} D3DKMT_RENDER_PROVIDER;
/* Context creation consumes captured kernel bytes. The provider validates its
 * node/engine/flags/private protocol and allocates real per-process buffers;
 * no invented rendering context or DMA address is supplied by the registry.
 * open returns a retained cookie or no ownership on failure. close transfers
 * lifetime to the native retirement worker; it must not free in-flight DMA.
 * Both execute outside registry locks. A failed close retains the cookie for
 * collect_contexts. Provider storage remains retained by the parent device.
 * Output CPU addresses belong to pid (and fit compat32 when requested), never
 * kernel pointers. The worker revokes them before freeing their backing. */
typedef struct {
    NTSTATUS (*open)(void *,DWORD,uint64_t,const D3DKMT_CONTEXT_DESCRIPTION *,
                     uint64_t *,D3DKMT_CONTEXT_BUFFERS *);
    NTSTATUS (*close)(void *,DWORD,uint64_t,uint64_t);
} D3DKMT_CONTEXT_PROVIDER;
NTSTATUS d3dkmt_register_context_driver(const D3DKMT_ADAPTER_DESCRIPTION *,
    const D3DKMT_DEVICE_PROVIDER *,const D3DKMT_ALLOCATION_PROVIDER *,
    const D3DKMT_CONTEXT_PROVIDER *,OKMT_LUID *);
NTSTATUS d3dkmt_register_render_driver(const D3DKMT_ADAPTER_DESCRIPTION *,
    const D3DKMT_DEVICE_PROVIDER *,const D3DKMT_ALLOCATION_PROVIDER *,
    const D3DKMT_CONTEXT_PROVIDER *,const D3DKMT_RENDER_PROVIDER *,OKMT_LUID *);
NTSTATUS d3dkmt_register_monitored_driver(const D3DKMT_ADAPTER_DESCRIPTION *,
    const D3DKMT_DEVICE_PROVIDER *,const D3DKMT_ALLOCATION_PROVIDER *,
    const D3DKMT_CONTEXT_PROVIDER *,const D3DKMT_RENDER_PROVIDER *,
    const D3DKMT_MONITORED_PROVIDER *,OKMT_LUID *);
NTSTATUS d3dkmt_create_context(DWORD,OKMT_HANDLE,const D3DKMT_CONTEXT_DESCRIPTION *,
    OKMT_HANDLE *,D3DKMT_CONTEXT_BUFFERS *);
NTSTATUS d3dkmt_destroy_context(DWORD,OKMT_HANDLE);
typedef struct d3dkmt_context_lease {
    struct d3dkmt_context_lease *self;
    DWORD pid;
    OKMT_HANDLE device,handle;
    uint64_t device_cookie,cookie;
    D3DKMT_DEVICE_PROVIDER provider;
    D3DKMT_CONTEXT_PROVIDER context;
    D3DKMT_RENDER_PROVIDER render;
    D3DKMT_CONTEXT_BUFFERS buffers;
    /* Atomic registry storage, valid only while this context lease is held. */
    const LONG *priority;
} D3DKMT_CONTEXT_LEASE;
NTSTATUS d3dkmt_context_priority(DWORD,OKMT_HANDLE,BOOL,LONG *);
LONG d3dkmt_context_priority_snapshot(const D3DKMT_CONTEXT_LEASE *);
/* OS-managed legacy synchronization. No shared/monitored-fence mappings are
 * implied. An accepted operation owns these references until the scheduler
 * retires it. Public destroy prevents new captures, not already queued uses.
 * Stable, noncopyable kernel storage; never expose the lease to a PE caller. */
typedef struct d3dkmt_sync_lease {
    struct d3dkmt_sync_lease *self;
    D3DKMT_CONTEXT_LEASE context;
    D3DKMT_SYNC_COMMAND command;
    PVOID event;
    BOOL event_signal;
    DWORD extra_count;
    D3DKMT_CONTEXT_LEASE extra[OKMT_MAX_BROADCAST_CONTEXTS-1];
} D3DKMT_SYNC_LEASE;
NTSTATUS d3dkmt_create_sync(DWORD,OKMT_HANDLE,const D3DKMT_SYNC_DESCRIPTION *,OKMT_HANDLE *);
NTSTATUS d3dkmt_create_monitored_sync(DWORD,OKMT_HANDLE,uint64_t initial,BOOL compat,
    OKMT_HANDLE *,uint64_t *cpu_address,uint64_t *gpu_address);
/* Paging queue and its monitored fence are distinct process-owned handles.
 * Destruction closes admission and invalidates future waits; captured waits
 * retain backing and the queue/device until their normal release. */
NTSTATUS d3dkmt_create_paging_queue(DWORD,OKMT_HANDLE,LONG,BOOL,
    OKMT_HANDLE *,OKMT_HANDLE *,uint64_t *);
NTSTATUS d3dkmt_destroy_paging_queue(DWORD,OKMT_HANDLE);
NTSTATUS d3dkmt_collect_syncs(void);
NTSTATUS d3dkmt_destroy_sync(DWORD,OKMT_HANDLE);
NTSTATUS d3dkmt_sync_capture(DWORD,OKMT_HANDLE,const D3DKMT_SYNC_COMMAND *,D3DKMT_SYNC_LEASE *);
NTSTATUS d3dkmt_sync_release(D3DKMT_SYNC_LEASE *);
/* Only execute at the head of the context stream, after prior GPU retirement.
 * BUSY consumes nothing. Success atomically commits publication and all object
 * updates under the registry lock. Cancellation competes on publication. */
NTSTATUS d3dkmt_sync_step(D3DKMT_SYNC_LEASE *,unsigned *publication);
/* Jointly validate every retained broadcast context. Publish performs the
 * single PREPARED->QUEUED admission; false only checks continued validity. */
NTSTATUS d3dkmt_sync_admit(D3DKMT_SYNC_LEASE *,unsigned *publication,BOOL publish);
NTSTATUS d3dkmt_sync_uses_monitored(const D3DKMT_CONTEXT_LEASE *,DWORD,const OKMT_HANDLE *,BOOL *);
NTSTATUS d3dkmt_signal_sync_cpu(DWORD,OKMT_HANDLE,DWORD,const OKMT_HANDLE *,const uint64_t *,BOOL rewind);
/* Stable noncopyable storage owned by the driver operation. Acquiring retains
 * both context and device through logical close/exit/removal. It does not
 * authorize new publication after close or prove hardware retirement. */
NTSTATUS d3dkmt_acquire_context(DWORD,OKMT_HANDLE,D3DKMT_CONTEXT_LEASE *);
NTSTATUS d3dkmt_release_context(D3DKMT_CONTEXT_LEASE *);
NTSTATUS d3dkmt_collect_contexts(void);
#define D3DKMT_MAX_ALLOCATION_BATCH 64u
/* Input/output are private kernel buffers stable for the entire call. All
 * handles publish atomically with parent admission under the registry lock.
 * Any failure leaves outputs unchanged; already-created native allocations
 * remain owned until rollback/collection succeeds. */
NTSTATUS d3dkmt_create_allocations(DWORD,OKMT_HANDLE,DWORD,
    const D3DKMT_ALLOCATION_DESCRIPTION *,OKMT_HANDLE *);
NTSTATUS d3dkmt_create_private_allocations(DWORD,OKMT_HANDLE,DWORD,
    const D3DKMT_PRIVATE_ALLOCATION *,OKMT_HANDLE *);
NTSTATUS d3dkmt_destroy_allocations(DWORD,OKMT_HANDLE,DWORD,const OKMT_HANDLE *);
/* Resource groups have typed device-local handles. Create may be empty;
 * append must have allocations. A failed append preserves previous members.
 * New group/member outputs publish together, or remain unchanged on failure.
 * An explicit destroy list must contain only members of one group (or only
 * standalone allocations). Destroying a group closes every member, but pins
 * and failed native cleanup retain ownership until actual retirement. */
NTSTATUS d3dkmt_create_resource_allocations(DWORD,OKMT_HANDLE,OKMT_HANDLE,BOOL,DWORD,
    const D3DKMT_ALLOCATION_DESCRIPTION *,OKMT_HANDLE *,OKMT_HANDLE *);
NTSTATUS d3dkmt_create_private_resource_allocations(DWORD,OKMT_HANDLE,OKMT_HANDLE,BOOL,DWORD,
    const D3DKMT_PRIVATE_ALLOCATION *,OKMT_HANDLE *,OKMT_HANDLE *);
NTSTATUS d3dkmt_destroy_resource(DWORD,OKMT_HANDLE,OKMT_HANDLE);
typedef struct d3dkmt_allocation_lease {
    struct d3dkmt_allocation_lease *self;
    DWORD pid;
    OKMT_HANDLE device,handle;
    uint64_t device_cookie,cookie;
    D3DKMT_DEVICE_PROVIDER provider;
    D3DKMT_ALLOCATION_PROVIDER memory;
} D3DKMT_ALLOCATION_LEASE;
/* Same stable, private, noncopyable ownership rules as device leases below.
 * A lease retains the allocation and its parent device, not GPU execution
 * ordering. Acquire native buffer pins before submitting work to the GPU. */
NTSTATUS d3dkmt_acquire_allocation(DWORD,OKMT_HANDLE,OKMT_HANDLE,D3DKMT_ALLOCATION_LEASE *);
NTSTATUS d3dkmt_release_allocation(D3DKMT_ALLOCATION_LEASE *);
/* Heap-owned private scratch, never placed on a Win32 thread's small stack.
 * Snapshot admission retains all selected allocations together. Resource
 * append is excluded until the provider completes; logical close may proceed
 * and retirement is deferred by the retained allocation references. */
typedef struct {
    OKMT_HANDLE handles[D3DKMT_MAX_HANDLES];
    DWORD values[D3DKMT_MAX_HANDLES];
    uint64_t cookies[D3DKMT_MAX_HANDLES];
} D3DKMT_ALLOCATION_PROPERTIES_WORK;
_Static_assert(sizeof(D3DKMT_ALLOCATION_PROPERTIES_WORK)==16*D3DKMT_MAX_HANDLES,
    "bounded allocation metadata workspace");
NTSTATUS d3dkmt_allocation_properties(DWORD,OKMT_HANDLE,OKMT_HANDLE,DWORD,DWORD,
    D3DKMT_ALLOCATION_PROPERTIES_WORK *);
NTSTATUS d3dkmt_collect_allocations(void);
enum {
    D3DKMT_WORK_PREPARED, D3DKMT_WORK_QUEUED, D3DKMT_WORK_CLAIMED, D3DKMT_WORK_CANCELLED,
    D3DKMT_WORK_SUBMITTED
};
/* Internal work publication over already retained, stable kernel leases.
 * Validate parent, context, all allocations and adapter under the registry
 * lock, then CAS a driver-owned state word. No callback/allocation/notification
 * executes under that lock. PREPARED->QUEUED, QUEUED->CLAIMED and
 * CLAIMED->SUBMITTED are allowed;
 * cancellation competes on the same word. Failure leaves the word unchanged.
 * SUBMITTED requires a non-executable native queue reservation sharing this
 * word, so native close/cancel can reject admission before it commits. Native
 * finish-stage runs after unlock and publishes only accepted work. Accepted
 * work retains all backing and may finish after external logical close. */
NTSTATUS d3dkmt_admit_context_work(const D3DKMT_CONTEXT_LEASE *,
    const D3DKMT_ALLOCATION_LEASE *const *,DWORD,unsigned *,unsigned,unsigned);
NTSTATUS d3dkmt_context_work_status(const D3DKMT_CONTEXT_LEASE *,
    const D3DKMT_ALLOCATION_LEASE *const *,DWORD);
/* Removal immediately hides the adapter. BUSY retains driver ownership while
 * native devices or pending callbacks exist; retry after those are drained. */
NTSTATUS d3dkmt_unregister_adapter(OKMT_LUID);
NTSTATUS d3dkmt_create_device(DWORD pid, OKMT_HANDLE adapter, DWORD flags, OKMT_HANDLE *);
/* Successful destroy closes user admission immediately. Native retirement may
 * be deferred while kernel operations hold a lease; failed provider cleanup
 * stays owned for collect_devices rather than reviving the public handle. */
NTSTATUS d3dkmt_destroy_device(DWORD pid, OKMT_HANDLE device);
typedef struct d3dkmt_device_lease {
    struct d3dkmt_device_lease *self;
    DWORD pid;
    OKMT_HANDLE handle;
    uint64_t cookie;
    D3DKMT_DEVICE_PROVIDER provider;
} D3DKMT_DEVICE_LEASE;
/* Kernel-only, zeroed stable storage; never copy or expose it to user code.
 * The holder must outlive its operation and release exactly once on every
 * completion/cancellation path. A persistent driver work item owns leases
 * which outlive a client thread. No registry lock spans a provider operation.
 * These protect native cookie/provider lifetime, not DMA pins or admission
 * after a concurrent close. New resource publication must recheck ownership. */
NTSTATUS d3dkmt_acquire_device(DWORD pid, OKMT_HANDLE, D3DKMT_DEVICE_LEASE *);
NTSTATUS d3dkmt_release_device(D3DKMT_DEVICE_LEASE *);
#define D3DKMT_MAX_CPU_WAITS 64u
/* Stable, noncopyable owned wait. Completion is sticky across fence rewinds;
 * public handle close does not revoke an already captured synchronization use.
 * Device/process close and adapter removal cancel pending waits. */
typedef struct d3dkmt_cpu_wait_lease {
    struct d3dkmt_cpu_wait_lease *self;
    D3DKMT_DEVICE_LEASE device;
    DWORD count;
    BOOL any;
    OKMT_HANDLE objects[D3DKMT_MAX_SYNC_OBJECTS];
    uint64_t values[D3DKMT_MAX_SYNC_OBJECTS];
    NTSTATUS status;
} D3DKMT_CPU_WAIT_LEASE;
BOOL d3dkmt_cpu_wait_install_waker(void (*)(void));
NTSTATUS d3dkmt_cpu_wait_capture(DWORD,OKMT_HANDLE,DWORD,const OKMT_HANDLE *,const uint64_t *,BOOL,D3DKMT_CPU_WAIT_LEASE *);
NTSTATUS d3dkmt_cpu_wait_status(D3DKMT_CPU_WAIT_LEASE *);
NTSTATUS d3dkmt_cpu_wait_cancel(D3DKMT_CPU_WAIT_LEASE *);
NTSTATUS d3dkmt_cpu_wait_release(D3DKMT_CPU_WAIT_LEASE *);
/* Retry at most one closing device; safe for a deferred driver worker. */
NTSTATUS d3dkmt_collect_devices(void);
/* Immortal kernel service callback: notification only, never inline collection.
 * Eligible work needs bounded timer retries; busy/leased objects instead wake
 * the service when their producer releases ownership. Groups retire with their
 * children. A provider close may fail without dropping its registry reference. */
BOOL d3dkmt_install_retirement_waker(void (*)(void));
DWORD d3dkmt_collect_budget(void);
/* All buffers below are captured kernel memory, never user pointers. */
NTSTATUS d3dkmt_enumerate_adapters(DWORD pid, DWORD capacity,
                                   OKMT_ADAPTER_INFO *, DWORD *count);
NTSTATUS d3dkmt_open_adapter(DWORD pid, OKMT_LUID, OKMT_HANDLE *);
/* Only the display owner publishes a GDI source mapping. A render-only or
 * prototype adapter cannot bind a screen. Names and outputs are kernel-owned
 * buffers; lookup and handle creation share the registry lock. Removal hides
 * mappings immediately, even while device retirement remains pending. */
NTSTATUS d3dkmt_bind_display(OKMT_LUID,DWORD source,const OKMT_WCHAR *name,BOOL primary);
NTSTATUS d3dkmt_unbind_display(OKMT_LUID,DWORD source);
/* Only the registered display owner publishes modes for a bound source.
 * Kernel inputs are copied before publication; NULL/zero withdraws the list.
 * Publication/release can allocate/free and must be outside caller spinlocks.
 * Each query retains an immutable snapshot across update/unbind/removal, with
 * no driver callback or large data copy under the registry lock. No firmware
 * modes, render-only capabilities or synthetic source are inferred here. */
#define D3DKMT_MAX_DISPLAY_MODES 65536u
#define D3DKMT_STATUS_INVALID_SOURCE ((NTSTATUS)0xC01E0304)
#define D3DKMT_STATUS_NO_DISPLAY_TARGET ((NTSTATUS)0xC01E0333)
typedef struct d3dkmt_display_snapshot {
    struct d3dkmt_display_snapshot *self;
    const OKMT_DISPLAY_MODE *modes;
    DWORD count;
    void *retained;
} D3DKMT_DISPLAY_SNAPSHOT;
NTSTATUS d3dkmt_publish_display_modes(OKMT_LUID,DWORD,const OKMT_DISPLAY_MODE *,DWORD);
NTSTATUS d3dkmt_acquire_display_modes(DWORD,OKMT_HANDLE,DWORD,D3DKMT_DISPLAY_SNAPSHOT *);
NTSTATUS d3dkmt_release_display_modes(D3DKMT_DISPLAY_SNAPSHOT *);
/* Read-only display queries, installed by the actual display owner. Calls run
 * outside registry/copy locks. Callback/context storage remains alive until
 * provider withdrawal or adapter unregister returns SUCCESS. Replacement and
 * withdrawal return BUSY while an admitted operation retains the provider.
 * A valid but unbound source has no scanout (blank, line zero). */
#define D3DKMT_STATUS_PRESENT_OCCLUDED ((NTSTATUS)0xC01E0006)
typedef struct { BOOL in_vertical_blank; DWORD scanline; } D3DKMT_SCANLINE_SAMPLE;
typedef struct {
    NTSTATUS (*scanline)(void *,DWORD,D3DKMT_SCANLINE_SAMPLE *);
    void *context;
} D3DKMT_DISPLAY_PROVIDER;
typedef struct d3dkmt_display_lease {
    struct d3dkmt_display_lease *self;
    void *retained;
    uint64_t epoch;
    DWORD source;
    BOOL bound;
    D3DKMT_DISPLAY_PROVIDER provider;
} D3DKMT_DISPLAY_LEASE;
NTSTATUS d3dkmt_set_display_provider(OKMT_LUID,const D3DKMT_DISPLAY_PROVIDER *);
NTSTATUS d3dkmt_acquire_display(DWORD,OKMT_HANDLE,DWORD,D3DKMT_DISPLAY_LEASE *);
NTSTATUS d3dkmt_display_status(const D3DKMT_DISPLAY_LEASE *);
NTSTATUS d3dkmt_sample_display(D3DKMT_DISPLAY_LEASE *,D3DKMT_SCANLINE_SAMPLE *);
NTSTATUS d3dkmt_release_display(D3DKMT_DISPLAY_LEASE *);
NTSTATUS d3dkmt_open_adapter_name(DWORD pid,const OKMT_WCHAR *name,BOOL gdi_name,
    OKMT_ADAPTER_INFO *,DWORD *source);
NTSTATUS d3dkmt_close_adapter(DWORD pid, OKMT_HANDLE);
void d3dkmt_release_process(DWORD pid);
DWORD d3dkmt_query_size(DWORD type);
NTSTATUS d3dkmt_query_adapter(DWORD pid, OKMT_HANDLE, DWORD type,
                               void *data, DWORD bytes, BOOL compat32);

#endif
