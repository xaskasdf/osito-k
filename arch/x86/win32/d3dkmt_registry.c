/* Graphics adapter references are a separate namespace from NT handles.
 * A reference belongs to one process, is never inherited implicitly, and
 * cannot become valid again after slot reuse or adapter removal. */
#include "d3dkmt_registry.h"
#include "../drivers/intel_bcs_queue.h"
#include "../drivers/intel_gpu_memory.h"
#ifdef __KERNEL_X86__
extern void *kmalloc(uint64_t);
extern void kfree(void *);
#define display_modes_allocate kmalloc
#define display_modes_free kfree
#else
#include <stdlib.h>
#define display_modes_allocate malloc
#define display_modes_free free
#endif

typedef struct {
    DWORD references,count;
    OKMT_DISPLAY_MODE modes[];
} KMT_DISPLAY_MODES;

static void display_modes_release(KMT_DISPLAY_MODES *modes)
{
    if (modes && __atomic_sub_fetch(&modes->references,1,__ATOMIC_ACQ_REL)==0)
        display_modes_free(modes);
}

typedef struct {
    BOOL present, occupied;
    OKMT_LUID luid;
    D3DKMT_ADAPTER_DESCRIPTION description;
    D3DKMT_DEVICE_PROVIDER provider;
    D3DKMT_ALLOCATION_PROVIDER memory;
    D3DKMT_CONTEXT_PROVIDER context;
    D3DKMT_RENDER_PROVIDER render;
    D3DKMT_MONITORED_PROVIDER monitored;
    DWORD devices;
    D3DKMT_DISPLAY_PROVIDER display;
    DWORD display_readers;
    uint64_t display_epoch;
} KMT_ADAPTER;
typedef struct {
    DWORD generation;
    DWORD owner;
    OKMT_LUID adapter;
    KMT_ADAPTER *driver;
    uint64_t cookie;
    DWORD leases,children;
    DWORD error_state;
    LONG scheduling_priority;
    OKMT_HANDLE parent,group,paging_sync;
    BOOL device,allocation,resource,context,synchronization,paging,busy,closing;
    DWORD sync_type,sync_maximum;
    uint64_t sync_value;
    volatile uint64_t *monitored_value;
    D3DKMT_CONTEXT_BUFFERS buffers;
} KMT_REFERENCE;

static KMT_ADAPTER adapters[D3DKMT_MAX_ADAPTERS];
static struct {
    BOOL used,primary;
    OKMT_LUID adapter;
    DWORD source;
    OKMT_WCHAR name[32];
    KMT_DISPLAY_MODES *modes;
} display_sources[64];
static KMT_REFERENCE references[D3DKMT_MAX_HANDLES];
static volatile unsigned registry_lock;
static DWORD adapter_serial;
static DWORD collect_cursor;
static DWORD allocation_cursor;
static DWORD context_cursor;
static DWORD sync_cursor;
static D3DKMT_CPU_WAIT_LEASE *cpu_waiters[D3DKMT_MAX_CPU_WAITS];
static void (*cpu_wait_waker)(void);
static void (*retirement_waker)(void);
static BOOL cpu_waits_update_locked(void);
static void cpu_waits_wake(void);
#define HANDLE_SLOT_BITS 10u
#define HANDLE_GENERATION_MAX (0xffffffffu >> HANDLE_SLOT_BITS)
_Static_assert(D3DKMT_MAX_HANDLES == (1u << HANDLE_SLOT_BITS), "handle encoding");

static uint64_t d3dkmt_lock(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(&registry_lock,1,__ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}

static void d3dkmt_unlock(uint64_t flags)
{
    __atomic_store_n(&registry_lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

BOOL d3dkmt_install_retirement_waker(void (*wake)(void))
{
    if (!wake) return FALSE;
    void (*expected)(void)=NULL;
    return __atomic_compare_exchange_n(&retirement_waker,&expected,wake,FALSE,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) || expected==wake;
}

static void retirement_wake(void)
{
    void (*wake)(void)=__atomic_load_n(&retirement_waker,__ATOMIC_ACQUIRE);
    if (wake) wake();
}

DWORD d3dkmt_collect_budget(void)
{
    uint64_t flags=d3dkmt_lock();
    DWORD budget=0;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        const KMT_REFERENCE *r=&references[i];
        if (!r->owner || !r->closing || r->busy || r->leases) continue;
        if (r->allocation || r->context ||
            (r->synchronization && r->sync_type==OKMT_SYNC_MONITORED) ||
            (r->device && !r->children)) budget++;
    }
    d3dkmt_unlock(flags);
    return budget;
}

static BOOL luid_equal(OKMT_LUID a, OKMT_LUID b)
{
    return a.low==b.low && a.high==b.high;
}

static KMT_ADAPTER *adapter_lookup(OKMT_LUID luid)
{
    for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++)
        if (adapters[i].present && luid_equal(adapters[i].luid,luid))
            return &adapters[i];
    return NULL;
}

static KMT_REFERENCE *reference_lookup(DWORD pid, OKMT_HANDLE handle)
{
    DWORD generation=handle >> HANDLE_SLOT_BITS;
    KMT_REFERENCE *r=&references[handle & (D3DKMT_MAX_HANDLES-1)];
    return pid && generation && r->owner==pid && r->generation==generation ? r : NULL;
}

static KMT_REFERENCE *handle_lookup(DWORD pid, OKMT_HANDLE handle)
{
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    return r && !r->device && !r->allocation && !r->resource && !r->context && !r->synchronization && !r->paging ? r : NULL;
}

static DWORD handle_available(void)
{
    DWORD count=0;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++)
        if (!references[i].owner && references[i].generation<HANDLE_GENERATION_MAX)
            count++;
    return count;
}

static OKMT_HANDLE handle_allocate(DWORD pid, OKMT_LUID luid)
{
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        KMT_REFERENCE *r=&references[i];
        if (!r->owner && r->generation<HANDLE_GENERATION_MAX) {
            r->generation++;
            r->owner=pid;
            r->adapter=luid;
            r->driver=NULL; r->cookie=0; r->leases=r->children=0; r->parent=r->group=r->paging_sync=0;
            r->error_state=0;
            __atomic_store_n(&r->scheduling_priority,0,__ATOMIC_RELAXED);
            r->device=r->allocation=r->resource=r->context=r->synchronization=r->paging=r->busy=r->closing=FALSE;
            r->sync_type=r->sync_maximum=0; r->sync_value=0;
            r->monitored_value=NULL;
            return (r->generation << HANDLE_SLOT_BITS) | i;
        }
    }
    return 0;
}

static DWORD kmt_name_length(const OKMT_WCHAR *name,DWORD capacity)
{
    if (name) for (DWORD i=0;i<capacity;i++) if (!name[i]) return i;
    return capacity;
}

static BOOL kmt_name_equal(const OKMT_WCHAR *a,const OKMT_WCHAR *b)
{
    /* Registered device/display names are OS-generated ASCII. Non-ASCII
     * callers compare literally; no locale-dependent folding under the lock. */
    for (DWORD i=0;i<260;i++) {
        OKMT_WCHAR x=a[i],y=b[i];
        if (x>='A' && x<='Z') x+='a'-'A';
        if (y>='A' && y<='Z') y+='a'-'A';
        if (x!=y) return FALSE;
        if (!x) return TRUE;
    }
    return FALSE;
}

static NTSTATUS adapter_register_full(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,
    const D3DKMT_CONTEXT_PROVIDER *context,const D3DKMT_RENDER_PROVIDER *render,
    const D3DKMT_MONITORED_PROVIDER *monitored,OKMT_LUID *luid)
{
    if (!description || !luid || !description->driver_version ||
        description->precise_present_regions>1 || description->device_ids.physical_index ||
        description->private_bytes>D3DKMT_MAX_ADAPTER_PRIVATE_BYTES ||
        description->umd32[259] || description->umd64[259] ||
        kmt_name_length(description->device_name,260)==260 ||
        ((description->adapter_type & OKMT_ADAPTER_RENDER) &&
         (!description->umd32[0] || !description->umd64[0])))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    if (description->device_name[0]) for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) {
        if (adapters[i].occupied && kmt_name_equal(description->device_name,adapters[i].description.device_name)) {
            d3dkmt_unlock(flags); return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    NTSTATUS status=STATUS_NO_MEMORY;
    if (adapter_serial!=0xffffffffu) {
        for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) {
            if (adapters[i].occupied) continue;
            adapters[i].description=*description;
            adapters[i].provider=provider ? *provider : (D3DKMT_DEVICE_PROVIDER){0};
            adapters[i].memory=memory ? *memory : (D3DKMT_ALLOCATION_PROVIDER){0};
            adapters[i].context=context ? *context : (D3DKMT_CONTEXT_PROVIDER){0};
            adapters[i].render=render ? *render : (D3DKMT_RENDER_PROVIDER){0};
            adapters[i].monitored=monitored ? *monitored : (D3DKMT_MONITORED_PROVIDER){0};
            adapters[i].devices=0;
            adapters[i].display=(D3DKMT_DISPLAY_PROVIDER){0};
            adapters[i].display_readers=0;adapters[i].display_epoch=0;
            /* Graphics-reserved LUID domain, with no reuse during this boot. */
            adapters[i].luid=(OKMT_LUID){++adapter_serial,0x4b4d54};
            adapters[i].present=adapters[i].occupied=TRUE;
            *luid=adapters[i].luid;
            status=STATUS_SUCCESS;
            break;
        }
    }
    d3dkmt_unlock(flags);
    return status;
}

static NTSTATUS adapter_register(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,
    const D3DKMT_CONTEXT_PROVIDER *context,const D3DKMT_RENDER_PROVIDER *render,OKMT_LUID *luid)
{ return adapter_register_full(description,provider,memory,context,render,NULL,luid); }

NTSTATUS d3dkmt_register_monitored_driver(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,
    const D3DKMT_CONTEXT_PROVIDER *context,const D3DKMT_RENDER_PROVIDER *render,
    const D3DKMT_MONITORED_PROVIDER *monitored,OKMT_LUID *luid)
{
    if (!provider || !provider->open || !provider->close || !memory || !memory->allocate ||
        !memory->close || !context || !context->open || !context->close ||
        !render || !render->submit || !monitored || !monitored->create || !monitored->close)
        return STATUS_INVALID_PARAMETER;
    return adapter_register_full(description,provider,memory,context,render,monitored,luid);
}

NTSTATUS d3dkmt_register_adapter(const D3DKMT_ADAPTER_DESCRIPTION *description,OKMT_LUID *luid)
{ return adapter_register(description,NULL,NULL,NULL,NULL,luid); }

NTSTATUS d3dkmt_register_driver(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,OKMT_LUID *luid)
{
    if (!provider || !provider->open || !provider->close) return STATUS_INVALID_PARAMETER;
    return adapter_register(description,provider,NULL,NULL,NULL,luid);
}

NTSTATUS d3dkmt_register_memory_driver(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,OKMT_LUID *luid)
{
    if (!provider || !provider->open || !provider->close ||
        !memory || !memory->allocate || !memory->close) return STATUS_INVALID_PARAMETER;
    return adapter_register(description,provider,memory,NULL,NULL,luid);
}

NTSTATUS d3dkmt_register_context_driver(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,
    const D3DKMT_CONTEXT_PROVIDER *context,OKMT_LUID *luid)
{
    if (!provider || !provider->open || !provider->close ||
        !memory || !memory->allocate || !memory->close ||
        !context || !context->open || !context->close) return STATUS_INVALID_PARAMETER;
    return adapter_register(description,provider,memory,context,NULL,luid);
}

NTSTATUS d3dkmt_register_render_driver(const D3DKMT_ADAPTER_DESCRIPTION *description,
    const D3DKMT_DEVICE_PROVIDER *provider,const D3DKMT_ALLOCATION_PROVIDER *memory,
    const D3DKMT_CONTEXT_PROVIDER *context,const D3DKMT_RENDER_PROVIDER *render,OKMT_LUID *luid)
{
    if (!provider || !provider->open || !provider->close ||
        !memory || !memory->allocate || !memory->close ||
        !context || !context->open || !context->close || !render || !render->submit)
        return STATUS_INVALID_PARAMETER;
    return adapter_register(description,provider,memory,context,render,luid);
}

NTSTATUS d3dkmt_unregister_adapter(OKMT_LUID luid)
{
    KMT_DISPLAY_MODES *retired[64]; DWORD retired_count=0;
    uint64_t flags=d3dkmt_lock();
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) {
        KMT_ADAPTER *a=&adapters[i];
        if (!a->occupied || !luid_equal(a->luid,luid)) continue;
        a->present=FALSE;
        for (DWORD j=0;j<64;j++) if (display_sources[j].used &&
            luid_equal(display_sources[j].adapter,luid)) {
            retired[retired_count++]=display_sources[j].modes;
            display_sources[j].modes=NULL; display_sources[j].used=FALSE;
        }
        status=a->devices || a->display_readers ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
        if (!a->devices && !a->display_readers) a->occupied=FALSE;
        break;
    }
    d3dkmt_unlock(flags);
    for (DWORD i=0;i<retired_count;i++) display_modes_release(retired[i]);
    /* Existing references can still be closed; queries report DEVICE_REMOVED. */
    flags=d3dkmt_lock(); BOOL wake=cpu_waits_update_locked(); d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    return status;
}

NTSTATUS d3dkmt_enumerate_adapters(DWORD pid, DWORD capacity,
                                   OKMT_ADAPTER_INFO *output, DWORD *count)
{
    if (!pid || !count) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    DWORD found=0;
    for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) found+=adapters[i].present!=0;
    NTSTATUS status=STATUS_SUCCESS;
    if (output && capacity<found) status=D3DKMT_STATUS_BUFFER_TOO_SMALL;
    else if (output && handle_available()<found) status=STATUS_NO_MEMORY;
    else {
        if (output) {
            DWORD j=0;
            for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) {
                KMT_ADAPTER *a=&adapters[i];
                if (!a->present) continue;
                output[j++]=(OKMT_ADAPTER_INFO){handle_allocate(pid,a->luid),a->luid,
                    a->description.source_count,(int)a->description.precise_present_regions};
            }
        }
        *count=found;
    }
    d3dkmt_unlock(flags);
    return status;
}

NTSTATUS d3dkmt_open_adapter(DWORD pid, OKMT_LUID luid, OKMT_HANDLE *handle)
{
    if (!pid || !handle) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_ADAPTER *a=adapter_lookup(luid);
    OKMT_HANDLE opened=a ? handle_allocate(pid,luid) : 0;
    if (opened) *handle=opened;
    d3dkmt_unlock(flags);
    return !a ? STATUS_INVALID_PARAMETER : opened ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

NTSTATUS d3dkmt_bind_display(OKMT_LUID luid,DWORD source,const OKMT_WCHAR *name,BOOL primary)
{
    DWORD length=kmt_name_length(name,32);
    if (!length || length==32 || (primary!=FALSE && primary!=TRUE)) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_ADAPTER *a=adapter_lookup(luid);
    if (!a || !(a->description.adapter_type&OKMT_ADAPTER_DISPLAY) ||
        (a->description.adapter_type&OKMT_ADAPTER_PROTOTYPE) || source>=a->description.source_count) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    if (a->display_epoch==UINT64_MAX) { d3dkmt_unlock(flags); return STATUS_INSUFFICIENT_RESOURCES; }
    DWORD free_slot=64,existing=64;
    for (DWORD i=0;i<64;i++) {
        if (!display_sources[i].used || !adapter_lookup(display_sources[i].adapter)) {
            if (free_slot==64) free_slot=i;
            continue;
        }
        if (luid_equal(display_sources[i].adapter,luid) && display_sources[i].source==source) {
            existing=i; continue;
        }
        if ((primary && display_sources[i].primary) || kmt_name_equal(name,display_sources[i].name)) {
            d3dkmt_unlock(flags); return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    DWORD slot=existing!=64 ? existing : free_slot;
    if (slot==64) { d3dkmt_unlock(flags); return STATUS_NO_MEMORY; }
    display_sources[slot].used=TRUE; display_sources[slot].primary=primary;
    display_sources[slot].adapter=luid; display_sources[slot].source=source;
    for (DWORD i=0;i<=length;i++) display_sources[slot].name[i]=name[i];
    a->display_epoch++;
    d3dkmt_unlock(flags); return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_unbind_display(OKMT_LUID luid,DWORD source)
{
    KMT_DISPLAY_MODES *retired=NULL;
    uint64_t flags=d3dkmt_lock(); NTSTATUS status=STATUS_INVALID_PARAMETER;
    for (DWORD i=0;i<64;i++) if (display_sources[i].used &&
        luid_equal(display_sources[i].adapter,luid) && display_sources[i].source==source) {
        retired=display_sources[i].modes; display_sources[i].modes=NULL;
        display_sources[i].used=FALSE; status=STATUS_SUCCESS; break;
    }
    KMT_ADAPTER *a=adapter_lookup(luid);
    if (!status && a && a->display_epoch!=UINT64_MAX) a->display_epoch++;
    d3dkmt_unlock(flags); display_modes_release(retired); return status;
}

NTSTATUS d3dkmt_publish_display_modes(OKMT_LUID luid,DWORD source,
    const OKMT_DISPLAY_MODE *input,DWORD count)
{
    if (count>D3DKMT_MAX_DISPLAY_MODES || (count && !input) || (!count && input))
        return STATUS_INVALID_PARAMETER;
    KMT_DISPLAY_MODES *replacement=NULL;
    if (count) {
        replacement=display_modes_allocate(sizeof(*replacement)+(SIZE_T)count*sizeof(*input));
        if (!replacement) return STATUS_NO_MEMORY;
        replacement->references=1; replacement->count=count;
        /* The trusted producer keeps input stable for this call. Validate the
         * captured copy; never publish a pointer into its mutable mode table. */
        for (DWORD i=0;i<count;i++) replacement->modes[i]=input[i];
        for (DWORD i=0;i<count;i++) {
            const OKMT_DISPLAY_MODE *m=&replacement->modes[i];
            if (!m->width || !m->height || !m->format || !m->refresh_denominator ||
                m->scanline_order>3 || m->orientation<1 || m->orientation>4 ||
                m->fixed_output>2 || (m->flags[0]&~3u) || (m->flags[1]&~0x1ffu)) {
                display_modes_release(replacement); return STATUS_INVALID_PARAMETER;
            }
        }
    }
    KMT_DISPLAY_MODES *retired=NULL;
    uint64_t flags=d3dkmt_lock();
    KMT_ADAPTER *a=adapter_lookup(luid);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (a && (a->description.adapter_type&OKMT_ADAPTER_DISPLAY) &&
        !(a->description.adapter_type&OKMT_ADAPTER_PROTOTYPE) && source<a->description.source_count) {
        for (DWORD i=0;i<64;i++) if (display_sources[i].used &&
            luid_equal(display_sources[i].adapter,luid) && display_sources[i].source==source) {
            retired=display_sources[i].modes; display_sources[i].modes=replacement;
            replacement=NULL; status=STATUS_SUCCESS; break;
        }
    }
    d3dkmt_unlock(flags);
    display_modes_release(retired); display_modes_release(replacement);
    return status;
}

NTSTATUS d3dkmt_acquire_display_modes(DWORD pid,OKMT_HANDLE handle,DWORD source,
    D3DKMT_DISPLAY_SNAPSHOT *snapshot)
{
    if (!snapshot || snapshot->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=handle_lookup(pid,handle);
    KMT_ADAPTER *a=r ? adapter_lookup(r->adapter) : NULL;
    NTSTATUS status=!r ? STATUS_INVALID_PARAMETER : !a ? D3DKMT_STATUS_DEVICE_REMOVED :
        !(a->description.adapter_type&OKMT_ADAPTER_DISPLAY) ||
        (a->description.adapter_type&OKMT_ADAPTER_PROTOTYPE) ? STATUS_NOT_SUPPORTED :
        D3DKMT_STATUS_INVALID_SOURCE;
    if (a && status==D3DKMT_STATUS_INVALID_SOURCE && source<a->description.source_count) {
        for (DWORD i=0;i<64;i++) if (display_sources[i].used &&
            luid_equal(display_sources[i].adapter,r->adapter) && display_sources[i].source==source) {
            KMT_DISPLAY_MODES *modes=display_sources[i].modes;
            status=D3DKMT_STATUS_NO_DISPLAY_TARGET;
            if (modes) {
                DWORD refs=__atomic_load_n(&modes->references,__ATOMIC_RELAXED);
                /* A publisher holds one ref under this lock. Releases can run
                 * concurrently; no acquisition can increment past UINT32_MAX. */
                while (refs!=UINT32_MAX && !__atomic_compare_exchange_n(&modes->references,&refs,refs+1,
                    FALSE,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) { }
                status=refs==UINT32_MAX ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
                if (!status) *snapshot=(D3DKMT_DISPLAY_SNAPSHOT){snapshot,modes->modes,modes->count,modes};
            }
            break;
        }
    }
    d3dkmt_unlock(flags); return status;
}

NTSTATUS d3dkmt_release_display_modes(D3DKMT_DISPLAY_SNAPSHOT *snapshot)
{
    if (!snapshot || snapshot->self!=snapshot || !snapshot->retained) return STATUS_INVALID_PARAMETER;
    KMT_DISPLAY_MODES *modes=snapshot->retained;
    *snapshot=(D3DKMT_DISPLAY_SNAPSHOT){0};
    display_modes_release(modes); return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_set_display_provider(OKMT_LUID luid,const D3DKMT_DISPLAY_PROVIDER *provider)
{
    if (provider && !provider->scanline) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();KMT_ADAPTER *a=adapter_lookup(luid);
    NTSTATUS status=!a ? STATUS_INVALID_PARAMETER :
        !(a->description.adapter_type&OKMT_ADAPTER_DISPLAY) ||
        (a->description.adapter_type&OKMT_ADAPTER_PROTOTYPE) ? STATUS_NOT_SUPPORTED :
        a->display_readers ? D3DKMT_STATUS_DEVICE_BUSY :
        a->display_epoch==UINT64_MAX ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    if (!status) { a->display=provider ? *provider : (D3DKMT_DISPLAY_PROVIDER){0};a->display_epoch++; }
    d3dkmt_unlock(flags);return status;
}

/* Only noncopyable kernel-owned leases reach this helper. Validate storage
 * identity before dereferencing the retained adapter or decrementing counts. */
static KMT_ADAPTER *display_lease_adapter(const D3DKMT_DISPLAY_LEASE *lease)
{
    if (!lease || lease->self!=lease) return NULL;
    for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) if (lease->retained==&adapters[i])
        return adapters[i].occupied && adapters[i].display_readers ? &adapters[i] : NULL;
    return NULL;
}

NTSTATUS d3dkmt_acquire_display(DWORD pid,OKMT_HANDLE handle,DWORD source,D3DKMT_DISPLAY_LEASE *lease)
{
    if (!lease || lease->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();KMT_REFERENCE *r=handle_lookup(pid,handle);
    KMT_ADAPTER *a=r ? adapter_lookup(r->adapter) : NULL;
    NTSTATUS status=!r ? STATUS_INVALID_PARAMETER : !a ? D3DKMT_STATUS_DEVICE_REMOVED :
        !(a->description.adapter_type&OKMT_ADAPTER_DISPLAY) ||
        (a->description.adapter_type&OKMT_ADAPTER_PROTOTYPE) ? STATUS_NOT_SUPPORTED :
        source>=a->description.source_count ? STATUS_INVALID_PARAMETER :
        !a->display.scanline ? STATUS_NOT_SUPPORTED :
        a->display_readers==UINT32_MAX ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    if (!status) {
        BOOL bound=FALSE;
        for (DWORD i=0;i<64;i++) if (display_sources[i].used &&
            luid_equal(display_sources[i].adapter,a->luid) && display_sources[i].source==source) {
            bound=TRUE;break;
        }
        a->display_readers++;
        *lease=(D3DKMT_DISPLAY_LEASE){lease,a,a->display_epoch,source,bound,a->display};
    }
    d3dkmt_unlock(flags);return status;
}

NTSTATUS d3dkmt_display_status(const D3DKMT_DISPLAY_LEASE *lease)
{
    uint64_t flags=d3dkmt_lock();KMT_ADAPTER *a=display_lease_adapter(lease);
    NTSTATUS status=!a ? STATUS_INVALID_PARAMETER : !a->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        lease->epoch!=a->display_epoch ? D3DKMT_STATUS_PRESENT_OCCLUDED : STATUS_SUCCESS;
    d3dkmt_unlock(flags);return status;
}

NTSTATUS d3dkmt_sample_display(D3DKMT_DISPLAY_LEASE *lease,D3DKMT_SCANLINE_SAMPLE *output)
{
    if (!output) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_display_status(lease);
    if (status) return status;
    D3DKMT_SCANLINE_SAMPLE sample={TRUE,0};
    if (lease->bound) status=lease->provider.scanline(lease->provider.context,lease->source,&sample);
    NTSTATUS current=d3dkmt_display_status(lease);
    if (current) return current;
    if (!status && sample.in_vertical_blank!=FALSE && sample.in_vertical_blank!=TRUE)
        status=STATUS_UNSUCCESSFUL;
    if (!status) *output=sample;
    return status;
}

NTSTATUS d3dkmt_release_display(D3DKMT_DISPLAY_LEASE *lease)
{
    uint64_t flags=d3dkmt_lock();KMT_ADAPTER *a=display_lease_adapter(lease);
    if (a) { a->display_readers--;*lease=(D3DKMT_DISPLAY_LEASE){0}; }
    d3dkmt_unlock(flags);return a ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS d3dkmt_open_adapter_name(DWORD pid,const OKMT_WCHAR *name,BOOL gdi_name,
    OKMT_ADAPTER_INFO *output,DWORD *source)
{
    DWORD capacity=gdi_name ? 32 : 260;
    DWORD length=kmt_name_length(name,capacity);
    if (!pid || !output || !source || length==capacity) return STATUS_INVALID_PARAMETER;
    if (!length) return gdi_name ? STATUS_UNSUCCESSFUL : STATUS_OBJECT_PATH_SYNTAX_BAD;
    static const OKMT_WCHAR primary_name[]={'D','I','S','P','L','A','Y',0};
    uint64_t flags=d3dkmt_lock(); KMT_ADAPTER *a=NULL; DWORD present_source=0;
    if (gdi_name) {
        BOOL primary=kmt_name_equal(name,primary_name);
        for (DWORD i=0;i<64;i++) {
            if (!display_sources[i].used ||
                !(primary ? display_sources[i].primary : kmt_name_equal(name,display_sources[i].name))) continue;
            a=adapter_lookup(display_sources[i].adapter);
            if (a) { present_source=display_sources[i].source; break; }
        }
    } else for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++) {
        if (adapters[i].present && kmt_name_equal(name,adapters[i].description.device_name)) { a=&adapters[i]; break; }
    }
    NTSTATUS status=gdi_name ? STATUS_UNSUCCESSFUL : STATUS_INVALID_PARAMETER;
    if (a) {
        OKMT_HANDLE handle=handle_allocate(pid,a->luid);
        status=handle ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        if (handle) {
            *output=(OKMT_ADAPTER_INFO){handle,a->luid,a->description.source_count,(int)a->description.precise_present_regions};
            *source=present_source;
        }
    }
    d3dkmt_unlock(flags); return status;
}

NTSTATUS d3dkmt_close_adapter(DWORD pid, OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=handle_lookup(pid,handle);
    if (r) r->owner=0;
    d3dkmt_unlock(flags);
    return r ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static NTSTATUS device_retire(DWORD pid, OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->device || !r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    if (r->busy || r->leases || r->children) { d3dkmt_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    r->busy=TRUE;
    KMT_ADAPTER *a=r->driver;
    D3DKMT_DEVICE_PROVIDER provider=a->provider;
    uint64_t cookie=r->cookie;
    d3dkmt_unlock(flags);
    NTSTATUS status=provider.close(provider.context,pid,cookie);
    flags=d3dkmt_lock();
    if (status==STATUS_SUCCESS) { r->owner=0; a->devices--; }
    r->busy=FALSE;
    d3dkmt_unlock(flags);
    return status;
}

/* Resource handles own no driver cookie. They retain their device and every
 * member retains the group until its native cleanup has actually succeeded.
 * Caller holds the registry lock; pending append/create keeps busy set. */
static void resource_retire_locked(KMT_REFERENCE *r)
{
    if (r->owner && (r->resource || r->paging) && r->closing && !r->busy && !r->leases && !r->children) {
        KMT_REFERENCE *parent=reference_lookup(r->owner,r->parent);
        parent->children--; r->owner=0;
    }
}

static void sync_retire_locked(KMT_REFERENCE *r)
{
    if (r->owner && r->synchronization && r->closing && !r->leases && !r->busy &&
        r->sync_type!=OKMT_SYNC_MONITORED) {
        KMT_REFERENCE *parent=reference_lookup(r->owner,r->parent);
        parent->children--; r->owner=0;
    }
}

static NTSTATUS monitored_retire(DWORD pid,OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->synchronization || r->sync_type!=OKMT_SYNC_MONITORED || !r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    if (r->busy || r->leases) { d3dkmt_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    r->busy=TRUE;
    OKMT_HANDLE device=r->parent;
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    KMT_ADAPTER *a=parent->driver;
    uint64_t cookie=r->cookie,device_cookie=parent->cookie;
    d3dkmt_unlock(flags);
    NTSTATUS status=a->monitored.close(a->provider.context,pid,device_cookie,cookie);
    flags=d3dkmt_lock();
    if (status==STATUS_SUCCESS) {
        if (r->group) {
            KMT_REFERENCE *queue=reference_lookup(pid,r->group);
            queue->children--; resource_retire_locked(queue);
        }
        r->owner=0; r->monitored_value=NULL; parent->children--;
    }
    r->busy=FALSE;
    BOOL retire=parent->closing && !parent->busy && !parent->leases && !parent->children;
    d3dkmt_unlock(flags);
    if (retire) (void)device_retire(pid,device);
    return status;
}

NTSTATUS d3dkmt_collect_syncs(void)
{
    uint64_t flags=d3dkmt_lock();
    DWORD pid=0; OKMT_HANDLE handle=0; BOOL pending=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        DWORD slot=(sync_cursor+i)&(D3DKMT_MAX_HANDLES-1);
        KMT_REFERENCE *r=&references[slot];
        if (!r->owner || !r->synchronization || r->sync_type!=OKMT_SYNC_MONITORED || !r->closing) continue;
        pending=TRUE;
        if (r->busy || r->leases) continue;
        pid=r->owner; handle=(r->generation<<HANDLE_SLOT_BITS)|slot;
        sync_cursor=(slot+1)&(D3DKMT_MAX_HANDLES-1); break;
    }
    d3dkmt_unlock(flags);
    return handle ? monitored_retire(pid,handle) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

static NTSTATUS allocation_retire(DWORD pid,OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->allocation || !r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    if (r->busy || r->leases) { d3dkmt_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    r->busy=TRUE;
    OKMT_HANDLE device=r->parent;
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    KMT_ADAPTER *a=parent->driver;
    uint64_t cookie=r->cookie,device_cookie=parent->cookie;
    d3dkmt_unlock(flags);
    NTSTATUS status=a->memory.close(a->provider.context,pid,device_cookie,cookie);
    flags=d3dkmt_lock();
    if (status==STATUS_SUCCESS) {
        if (r->group) {
            KMT_REFERENCE *group=reference_lookup(pid,r->group);
            group->children--; resource_retire_locked(group);
        }
        r->owner=0; parent->children--;
    }
    r->busy=FALSE;
    BOOL retire=parent->closing && !parent->busy && !parent->leases && !parent->children;
    d3dkmt_unlock(flags);
    if (retire) (void)device_retire(pid,device);
    return status;
}

static NTSTATUS context_retire(DWORD pid,OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->context || !r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    if (r->busy || r->leases) { d3dkmt_unlock(flags); return D3DKMT_STATUS_DEVICE_BUSY; }
    r->busy=TRUE;
    OKMT_HANDLE device=r->parent;
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    KMT_ADAPTER *a=parent->driver;
    uint64_t cookie=r->cookie,device_cookie=parent->cookie;
    d3dkmt_unlock(flags);
    NTSTATUS status=a->context.close(a->provider.context,pid,device_cookie,cookie);
    flags=d3dkmt_lock();
    if (status==STATUS_SUCCESS) { r->owner=0; parent->children--; }
    r->busy=FALSE;
    BOOL retire=parent->closing && !parent->busy && !parent->leases && !parent->children;
    d3dkmt_unlock(flags);
    if (retire) (void)device_retire(pid,device);
    return status;
}

void d3dkmt_release_process(DWORD pid)
{
    if (!pid) return;
    BOOL devices=FALSE;
    uint64_t flags=d3dkmt_lock();
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++)
        if (references[i].owner==pid) {
            if (references[i].device || references[i].allocation || references[i].resource || references[i].context || references[i].synchronization || references[i].paging) {
                references[i].closing=TRUE; devices=TRUE;
            }
            else references[i].owner=0;
        }
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++)
        if (references[i].owner==pid && (references[i].resource || references[i].paging)) resource_retire_locked(&references[i]);
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++)
        if (references[i].owner==pid && references[i].synchronization) sync_retire_locked(&references[i]);
    BOOL wake=cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    for (DWORD i=0;devices && i<D3DKMT_MAX_HANDLES;i++) {
        flags=d3dkmt_lock();
        KMT_REFERENCE *r=&references[i];
        BOOL context=r->context;
        OKMT_HANDLE h=r->owner==pid && (r->allocation || context) && !r->busy && !r->leases ?
            (r->generation<<HANDLE_SLOT_BITS)|i : 0;
        d3dkmt_unlock(flags);
        if (h) { if (context) (void)context_retire(pid,h); else (void)allocation_retire(pid,h); }
    }
    /* Pending opens are cancelled by their publication check. Ready device
     * closes run outside the registry lock; a failed close remains retryable. */
    for (DWORD i=0;devices && i<D3DKMT_MAX_HANDLES;i++) {
        flags=d3dkmt_lock();
        KMT_REFERENCE *r=&references[i];
        OKMT_HANDLE h=r->owner==pid && r->device && r->closing && !r->busy && !r->leases ?
            (r->generation<<HANDLE_SLOT_BITS)|i : 0;
        d3dkmt_unlock(flags);
        if (h) (void)device_retire(pid,h);
    }
    /* Metadata-only notification. The GPU worker retains native DMA bundles
     * until completion/retirement; never run MMIO or wait in process teardown. */
    intel_bcs_release_process(pid);
    intel_gpu_memory_release_process(pid);
    if (devices) retirement_wake();
}

NTSTATUS d3dkmt_create_device(DWORD pid, OKMT_HANDLE adapter, DWORD options, OKMT_HANDLE *device)
{
    if (!pid || !device || (options&~15u)) return STATUS_INVALID_PARAMETER;
    /* Legacy command buffers, VSync requests, timeout disable and test-mode
     * devices require additional contracts; no successful approximation. */
    if (options) return STATUS_NOT_SUPPORTED;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=handle_lookup(pid,adapter);
    KMT_ADAPTER *a=parent ? adapter_lookup(parent->adapter) : NULL;
    NTSTATUS status=!parent ? STATUS_INVALID_PARAMETER : !a ? D3DKMT_STATUS_DEVICE_REMOVED :
        !a->provider.open ? STATUS_NOT_SUPPORTED :
        (a->provider.fault_state && __atomic_load_n(a->provider.fault_state,__ATOMIC_ACQUIRE)) ?
            D3DKMT_STATUS_DEVICE_ERROR : STATUS_SUCCESS;
    OKMT_HANDLE handle=0;
    KMT_REFERENCE *r=NULL;
    D3DKMT_DEVICE_PROVIDER provider={0};
    if (status==STATUS_SUCCESS) {
        handle=handle_allocate(pid,a->luid);
        if (!handle) status=STATUS_NO_MEMORY;
        else {
            r=reference_lookup(pid,handle);
            r->driver=a; r->device=r->busy=TRUE;
            a->devices++;
            provider=a->provider;
        }
    }
    d3dkmt_unlock(flags);
    if (status!=STATUS_SUCCESS) return status;
    uint64_t cookie=0;
    status=provider.open(provider.context,pid,&cookie);
    flags=d3dkmt_lock();
    /* Pending ownership prevents slot/driver reuse while the callback runs. */
    if (status!=STATUS_SUCCESS) { r->owner=0; a->devices--; }
    else {
        r->cookie=cookie;
        if (r->closing) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!a->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else if (r->error_state || (provider.fault_state &&
            __atomic_load_n(provider.fault_state,__ATOMIC_ACQUIRE))) status=D3DKMT_STATUS_DEVICE_ERROR;
        if (status!=STATUS_SUCCESS) r->closing=TRUE;
        else *device=handle;
    }
    r->busy=FALSE;
    BOOL rollback=r->owner && r->closing;
    d3dkmt_unlock(flags);
    if (rollback) { (void)device_retire(pid,handle); retirement_wake(); }
    return status;
}

NTSTATUS d3dkmt_destroy_device(DWORD pid, OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->device || r->closing || r->busy) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    r->closing=TRUE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        KMT_REFERENCE *child=&references[i];
        if (child->owner==pid && (child->allocation || child->resource || child->context || child->synchronization || child->paging) && child->parent==handle) {
            child->closing=TRUE;
            if (child->resource || child->paging) resource_retire_locked(child);
            if (child->synchronization) sync_retire_locked(child);
        }
    }
    BOOL retire=!r->leases && !r->children;
    BOOL wake=cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        flags=d3dkmt_lock();
        KMT_REFERENCE *child=&references[i];
        BOOL context=child->context;
        OKMT_HANDLE h=child->owner==pid && (child->allocation || context) && child->parent==handle &&
            !child->busy && !child->leases ? (child->generation<<HANDLE_SLOT_BITS)|i : 0;
        d3dkmt_unlock(flags);
        if (h) { if (context) (void)context_retire(pid,h); else (void)allocation_retire(pid,h); }
    }
    /* Closing the handle and reclaiming its backing are separate events.
     * Callback failures retain ownership in the retry collector. */
    if (retire) (void)device_retire(pid,handle);
    retirement_wake();
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_acquire_device(DWORD pid, OKMT_HANDLE handle, D3DKMT_DEVICE_LEASE *lease)
{
    if (!lease || lease->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (r && r->device && !r->busy && !r->closing) {
        if (!r->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else if (r->leases==UINT32_MAX) status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            r->leases++;
            *lease=(D3DKMT_DEVICE_LEASE){lease,pid,handle,r->cookie,r->driver->provider};
            status=STATUS_SUCCESS;
        }
    }
    d3dkmt_unlock(flags);
    return status;
}

NTSTATUS d3dkmt_release_device(D3DKMT_DEVICE_LEASE *lease)
{
    if (!lease || lease->self!=lease) return STATUS_INVALID_PARAMETER;
    DWORD pid=lease->pid; OKMT_HANDLE handle=lease->handle;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->device || !r->leases || r->cookie!=lease->cookie ||
        r->driver->provider.context!=lease->provider.context ||
        r->driver->provider.open!=lease->provider.open ||
        r->driver->provider.close!=lease->provider.close) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    r->leases--;
    BOOL retire=!r->leases && r->closing && !r->busy;
    *lease=(D3DKMT_DEVICE_LEASE){0};
    d3dkmt_unlock(flags);
    if (retire) { (void)device_retire(pid,handle); retirement_wake(); }
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_query_device_execution(DWORD pid,OKMT_HANDLE handle,DWORD *output)
{
    if (!output) return STATUS_INVALID_PARAMETER;
    D3DKMT_DEVICE_LEASE lease={0};
    NTSTATUS status=d3dkmt_acquire_device(pid,handle,&lease);
    if (status!=STATUS_SUCCESS) return status;
    uint64_t flags=d3dkmt_lock();
    DWORD state=reference_lookup(pid,handle)->error_state;
    d3dkmt_unlock(flags);
    if (!state) status=lease.provider.execution ?
        lease.provider.execution(lease.provider.context,pid,lease.cookie,&state) : STATUS_NOT_SUPPORTED;
    if (status==STATUS_SUCCESS && (state<OKMT_EXECUTION_ACTIVE || state>OKMT_EXECUTION_DMA_PAGE_FAULT))
        status=STATUS_UNSUCCESSFUL;
    flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle); /* retained until release */
    if (r->error_state) { state=r->error_state; status=STATUS_SUCCESS; }
    if (status==STATUS_SUCCESS) {
        if (r->closing) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!r->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else *output=state;
    }
    d3dkmt_unlock(flags);
    (void)d3dkmt_release_device(&lease);
    return status;
}

NTSTATUS d3dkmt_mark_device_error(DWORD pid,OKMT_HANDLE handle,DWORD reason)
{
    D3DKMT_DEVICE_LEASE lease={0};
    NTSTATUS status=d3dkmt_acquire_device(pid,handle,&lease);
    if (status) return status;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,handle);
    BOOL first=FALSE,wake=FALSE;
    if (parent->closing) status=STATUS_PROCESS_IS_TERMINATING;
    else if (!parent->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    else if (!parent->error_state) {
        /* Windows accepts the reason bit without its severity bit too.
         * Differential PE32/PE64 oracle: first reason wins, unknown -> OOM. */
        reason&=0x7fffffffu;
        parent->error_state=!reason ? OKMT_EXECUTION_RESET : reason==6 ? OKMT_EXECUTION_DMA_FAULT :
            reason==7 ? OKMT_EXECUTION_HUNG : OKMT_EXECUTION_OUT_OF_MEMORY;
        first=TRUE;
        for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
            KMT_REFERENCE *r=&references[i];
            /* A busy close may already have passed ownership to its provider;
             * pending monitored creations roll back when they come back. */
            if (r->owner==pid && r->parent==handle && r->synchronization &&
                r->sync_type==OKMT_SYNC_MONITORED && !r->busy && r->monitored_value)
                __atomic_store_n(r->monitored_value,UINT64_MAX,__ATOMIC_RELEASE);
        }
        wake=cpu_waits_update_locked();
    }
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    if (first && lease.provider.error) lease.provider.error(lease.provider.context,pid,lease.cookie);
    (void)d3dkmt_release_device(&lease);
    return status;
}

static BOOL driver_error_matches(const KMT_ADAPTER *a,const D3DKMT_DEVICE_PROVIDER *p)
{
    return a && a->occupied && a->provider.context==p->context &&
        a->provider.open==p->open && a->provider.close==p->close &&
        a->provider.fault_state==p->fault_state;
}

NTSTATUS d3dkmt_report_driver_error(const D3DKMT_DEVICE_PROVIDER *provider)
{
    if (!provider || !provider->open || !provider->close || !provider->fault_state)
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    DWORD state=__atomic_load_n(provider->fault_state,__ATOMIC_ACQUIRE);
    if (state<OKMT_EXECUTION_RESET || state>OKMT_EXECUTION_DMA_PAGE_FAULT) {
        d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;
    }
    BOOL found=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_ADAPTERS;i++)
        found|=driver_error_matches(&adapters[i],provider);
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        KMT_REFERENCE *r=&references[i];
        if (r->owner && r->device && driver_error_matches(r->driver,provider) && !r->error_state)
            r->error_state=state;
    }
    /* One atomic registry transaction affects all processes sharing this
     * native executor. Pending opens/creations recheck before publication.
     * Busy closes may have returned backing to the provider; never touch it. */
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        KMT_REFERENCE *r=&references[i];
        if (!r->owner || !r->synchronization || r->sync_type!=OKMT_SYNC_MONITORED ||
            r->busy || !r->monitored_value) continue;
        KMT_REFERENCE *parent=reference_lookup(r->owner,r->parent);
        if (parent && parent->device && driver_error_matches(parent->driver,provider))
            __atomic_store_n(r->monitored_value,UINT64_MAX,__ATOMIC_RELEASE);
    }
    BOOL wake=cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    /* The driver closes its own queue. No device-close/error callback here:
     * no reentrant retirement or engine reset from inside fault publication. */
    return found ? STATUS_SUCCESS : D3DKMT_STATUS_DEVICE_REMOVED;
}

NTSTATUS d3dkmt_collect_devices(void)
{
    NTSTATUS allocations=d3dkmt_collect_allocations();
    NTSTATUS contexts=d3dkmt_collect_contexts();
    NTSTATUS syncs=d3dkmt_collect_syncs();
    uint64_t flags=d3dkmt_lock();
    DWORD pid=0; OKMT_HANDLE handle=0; BOOL pending=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        DWORD slot=(collect_cursor+i)&(D3DKMT_MAX_HANDLES-1);
        KMT_REFERENCE *r=&references[slot];
        if (r->owner && r->device && r->closing) {
            pending=TRUE;
            if (r->busy || r->leases || r->children) continue;
            pid=r->owner; handle=(r->generation<<HANDLE_SLOT_BITS)|slot;
            /* A persistently failing close cannot starve later devices. */
            collect_cursor=(slot+1)&(D3DKMT_MAX_HANDLES-1);
            break;
        }
    }
    d3dkmt_unlock(flags);
    return handle ? device_retire(pid,handle) : pending ? D3DKMT_STATUS_DEVICE_BUSY :
        contexts!=STATUS_SUCCESS ? contexts : syncs!=STATUS_SUCCESS ? syncs : allocations;
}

NTSTATUS d3dkmt_create_context(DWORD pid,OKMT_HANDLE device,
    const D3DKMT_CONTEXT_DESCRIPTION *description,OKMT_HANDLE *output,D3DKMT_CONTEXT_BUFFERS *buffers)
{
    if (!pid || !description || !output || !buffers ||
        description->private_bytes>D3DKMT_MAX_CONTEXT_PRIVATE_BYTES || (description->flags&~127u))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    KMT_ADAPTER *a=NULL; KMT_REFERENCE *r=NULL;
    OKMT_HANDLE handle=0; uint64_t device_cookie=0;
    if (parent && parent->device && !parent->closing && !parent->busy) {
        a=parent->driver;
        status=!a->present ? D3DKMT_STATUS_DEVICE_REMOVED :
            parent->error_state ? D3DKMT_STATUS_DEVICE_ERROR :
            !a->context.open ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        if (status==STATUS_SUCCESS) {
            handle=handle_allocate(pid,a->luid);
            if (!handle) status=STATUS_NO_MEMORY;
            else {
                r=reference_lookup(pid,handle);
                r->context=r->busy=TRUE; r->parent=device; r->driver=a;
                parent->children++; device_cookie=parent->cookie;
            }
        }
    }
    d3dkmt_unlock(flags);
    if (status!=STATUS_SUCCESS) return status;
    uint64_t cookie=0; D3DKMT_CONTEXT_BUFFERS created={0};
    status=a->context.open(a->provider.context,pid,device_cookie,description,&cookie,&created);
    flags=d3dkmt_lock();
    if (status!=STATUS_SUCCESS) { r->owner=0; parent->children--; }
    else {
        r->cookie=cookie;
        if (r->closing || parent->closing) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!a->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else if (parent->error_state) status=D3DKMT_STATUS_DEVICE_ERROR;
        if (status!=STATUS_SUCCESS) r->closing=TRUE;
        else { r->buffers=created; *output=handle; *buffers=created; }
    }
    r->busy=FALSE;
    BOOL rollback=r->owner && r->closing;
    BOOL retire=parent->closing && !parent->busy && !parent->leases && !parent->children;
    d3dkmt_unlock(flags);
    if (rollback) (void)context_retire(pid,handle);
    else if (retire) (void)device_retire(pid,device);
    if (rollback || retire) retirement_wake();
    return status;
}

NTSTATUS d3dkmt_destroy_context(DWORD pid,OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->context || r->busy || r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    r->closing=TRUE;
    BOOL retire=!r->leases;
    d3dkmt_unlock(flags);
    if (retire) { (void)context_retire(pid,handle); retirement_wake(); }
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_acquire_context(DWORD pid,OKMT_HANDLE handle,D3DKMT_CONTEXT_LEASE *lease)
{
    if (!lease || lease->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (r && r->context && !r->busy && !r->closing) {
        KMT_REFERENCE *parent=reference_lookup(pid,r->parent);
        if (!r->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else if (parent->closing || parent->busy) status=STATUS_INVALID_PARAMETER;
        else if (parent->error_state) status=D3DKMT_STATUS_DEVICE_ERROR;
        else if (r->leases==UINT32_MAX) status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            r->leases++;
            *lease=(D3DKMT_CONTEXT_LEASE){lease,pid,r->parent,handle,parent->cookie,r->cookie,
                r->driver->provider,r->driver->context,r->driver->render,r->buffers,&r->scheduling_priority};
            status=STATUS_SUCCESS;
        }
    }
    d3dkmt_unlock(flags);
    return status;
}

LONG d3dkmt_context_priority_snapshot(const D3DKMT_CONTEXT_LEASE *lease)
{
    return lease && lease->self==lease && lease->priority ?
        __atomic_load_n(lease->priority,__ATOMIC_ACQUIRE) : 0;
}

NTSTATUS d3dkmt_context_priority(DWORD pid,OKMT_HANDLE handle,BOOL set,LONG *priority)
{
    if (!priority) return STATUS_INVALID_PARAMETER;
    if (set && (*priority < -7 || *priority > 7)) {
        DWORD raw=(DWORD)*priority;
        return (raw&0x40000000u) && !(raw&~0x4000001fu) ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER;
    }
    D3DKMT_CONTEXT_LEASE lease={0};
    NTSTATUS status=d3dkmt_acquire_context(pid,handle,&lease);
    if (status) return status;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle),*parent=reference_lookup(pid,lease.device);
    if (r->closing || parent->closing) status=STATUS_PROCESS_IS_TERMINATING;
    else if (!r->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    else if (parent->error_state) status=D3DKMT_STATUS_DEVICE_ERROR;
    else if (set) __atomic_store_n(&r->scheduling_priority,*priority,__ATOMIC_RELEASE);
    else *priority=__atomic_load_n(&r->scheduling_priority,__ATOMIC_ACQUIRE);
    d3dkmt_unlock(flags);
    (void)d3dkmt_release_context(&lease);
    return status;
}

NTSTATUS d3dkmt_release_context(D3DKMT_CONTEXT_LEASE *lease)
{
    if (!lease || lease->self!=lease) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    DWORD pid=lease->pid; OKMT_HANDLE handle=lease->handle;
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    KMT_REFERENCE *parent=reference_lookup(pid,lease->device);
    if (!r || !r->context || !r->leases || r->cookie!=lease->cookie || r->parent!=lease->device ||
        !parent || !parent->device || parent->cookie!=lease->device_cookie ||
        r->driver->provider.context!=lease->provider.context ||
        r->driver->provider.open!=lease->provider.open || r->driver->provider.close!=lease->provider.close ||
        r->driver->context.open!=lease->context.open || r->driver->context.close!=lease->context.close) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    r->leases--;
    BOOL retire=!r->leases && r->closing && !r->busy;
    *lease=(D3DKMT_CONTEXT_LEASE){0};
    d3dkmt_unlock(flags);
    if (retire) { (void)context_retire(pid,handle); retirement_wake(); }
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_collect_contexts(void)
{
    uint64_t flags=d3dkmt_lock();
    DWORD pid=0; OKMT_HANDLE handle=0; BOOL pending=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        DWORD slot=(context_cursor+i)&(D3DKMT_MAX_HANDLES-1);
        KMT_REFERENCE *r=&references[slot];
        if (r->owner && r->context && r->closing) {
            pending=TRUE;
            if (r->busy || r->leases) continue;
            pid=r->owner; handle=(r->generation<<HANDLE_SLOT_BITS)|slot;
            context_cursor=(slot+1)&(D3DKMT_MAX_HANDLES-1); break;
        }
    }
    d3dkmt_unlock(flags);
    return handle ? context_retire(pid,handle) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

/* Caller holds the registry lock; every input is retained kernel storage. */
static NTSTATUS context_work_validate(const D3DKMT_CONTEXT_LEASE *context,
    const D3DKMT_ALLOCATION_LEASE *const *allocations,DWORD count)
{
    if (!context || context->self!=context || count>D3DKMT_MAX_ALLOCATION_BATCH || (count && !allocations))
        return STATUS_INVALID_PARAMETER;
    KMT_REFERENCE *r=reference_lookup(context->pid,context->handle);
    KMT_REFERENCE *parent=reference_lookup(context->pid,context->device);
    if (!r || !r->context || !r->leases || r->cookie!=context->cookie || r->parent!=context->device ||
        !parent || !parent->device || parent->cookie!=context->device_cookie || parent->driver!=r->driver ||
        r->driver->provider.context!=context->provider.context ||
        r->driver->provider.open!=context->provider.open || r->driver->provider.close!=context->provider.close ||
        r->driver->context.open!=context->context.open || r->driver->context.close!=context->context.close)
        return STATUS_INVALID_PARAMETER;
    if (!r->driver->present) return D3DKMT_STATUS_DEVICE_REMOVED;
    if (r->closing || parent->closing) return STATUS_PROCESS_IS_TERMINATING;
    if (parent->error_state) return D3DKMT_STATUS_DEVICE_ERROR;
    if (r->busy || parent->busy) return D3DKMT_STATUS_DEVICE_BUSY;
    for (DWORD i=0;i<count;i++) {
        const D3DKMT_ALLOCATION_LEASE *lease=allocations[i];
        if (!lease || lease->self!=lease || lease->pid!=context->pid || lease->device!=context->device ||
            lease->device_cookie!=context->device_cookie) return STATUS_INVALID_PARAMETER;
        KMT_REFERENCE *allocation=reference_lookup(lease->pid,lease->handle);
        if (!allocation || !allocation->allocation || !allocation->leases || allocation->parent!=context->device ||
            allocation->cookie!=lease->cookie || allocation->driver!=r->driver ||
            lease->provider.context!=r->driver->provider.context || lease->provider.open!=r->driver->provider.open ||
            lease->provider.close!=r->driver->provider.close || lease->memory.allocate!=r->driver->memory.allocate ||
            lease->memory.close!=r->driver->memory.close || lease->memory.translate!=r->driver->memory.translate)
            return STATUS_INVALID_PARAMETER;
        if (allocation->closing) return STATUS_PROCESS_IS_TERMINATING;
        if (allocation->busy) return D3DKMT_STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
NTSTATUS d3dkmt_context_work_status(const D3DKMT_CONTEXT_LEASE *context,
    const D3DKMT_ALLOCATION_LEASE *const *allocations,DWORD count)
{
    uint64_t flags=d3dkmt_lock();
    NTSTATUS status=context_work_validate(context,allocations,count);
    d3dkmt_unlock(flags); return status;
}
NTSTATUS d3dkmt_admit_context_work(const D3DKMT_CONTEXT_LEASE *context,
    const D3DKMT_ALLOCATION_LEASE *const *allocations,DWORD count,unsigned *publication,
    unsigned expected,unsigned next)
{
    if (!publication || !((expected==D3DKMT_WORK_PREPARED && next==D3DKMT_WORK_QUEUED) ||
        (expected==D3DKMT_WORK_QUEUED && next==D3DKMT_WORK_CLAIMED) ||
        (expected==D3DKMT_WORK_CLAIMED && next==D3DKMT_WORK_SUBMITTED))) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    NTSTATUS status=context_work_validate(context,allocations,count);
    if (status==STATUS_SUCCESS && !__atomic_compare_exchange_n(publication,&expected,next,false,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) status=STATUS_PROCESS_IS_TERMINATING;
    d3dkmt_unlock(flags); return status;
}

NTSTATUS d3dkmt_create_sync(DWORD pid,OKMT_HANDLE device,const D3DKMT_SYNC_DESCRIPTION *description,OKMT_HANDLE *output)
{
    if (!description || !output || description->type<1 || description->type>3) return STATUS_INVALID_PARAMETER;
    if ((description->type==1 && description->initial>1) ||
        (description->type==2 && (!description->maximum || description->initial>description->maximum)))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=!parent || !parent->device || parent->busy || parent->closing ? STATUS_INVALID_PARAMETER :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        !parent->driver->context.open ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    if (status==STATUS_SUCCESS) {
        OKMT_HANDLE h=handle_allocate(pid,parent->adapter);
        if (!h) status=STATUS_NO_MEMORY;
        else {
            KMT_REFERENCE *r=reference_lookup(pid,h);
            r->synchronization=TRUE; r->parent=device; r->driver=parent->driver;
            r->sync_type=description->type; r->sync_maximum=description->maximum;
            /* InitialState describes ownership, not the signaled state. */
            r->sync_value=description->type==1 ? !description->initial : description->initial;
            parent->children++; *output=h;
        }
    }
    d3dkmt_unlock(flags); return status;
}

NTSTATUS d3dkmt_create_monitored_sync(DWORD pid,OKMT_HANDLE device,uint64_t initial,BOOL compat,
    OKMT_HANDLE *output,uint64_t *cpu_address,uint64_t *gpu_address)
{
    if (!pid || !output || !cpu_address || !gpu_address || (compat!=FALSE && compat!=TRUE))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=!parent || !parent->device || parent->busy || parent->closing ? STATUS_INVALID_PARAMETER :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state ? D3DKMT_STATUS_DEVICE_ERROR :
        !parent->driver->monitored.create ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    OKMT_HANDLE h=0;
    KMT_REFERENCE *r=NULL;
    if (status==STATUS_SUCCESS) {
        h=handle_allocate(pid,parent->adapter);
        if (!h) status=STATUS_NO_MEMORY;
        else {
            r=reference_lookup(pid,h); r->synchronization=TRUE; r->busy=TRUE;
            r->sync_type=OKMT_SYNC_MONITORED; r->parent=device; r->driver=parent->driver;
            parent->children++;
        }
    }
    if (status!=STATUS_SUCCESS) { d3dkmt_unlock(flags); return status; }
    KMT_ADAPTER *a=parent->driver; uint64_t device_cookie=parent->cookie;
    d3dkmt_unlock(flags);
    D3DKMT_MONITORED_BACKING backing={0};
    status=a->monitored.create(a->provider.context,pid,device_cookie,initial,compat,&backing);
    if (status==STATUS_SUCCESS && (!backing.cookie || !backing.value || ((uintptr_t)backing.value&7) ||
        !backing.cpu_address || (backing.cpu_address&7) || (compat && backing.cpu_address>UINT32_MAX-7) ||
        !backing.gpu_address || (backing.gpu_address&7))) status=STATUS_UNSUCCESSFUL;
    flags=d3dkmt_lock();
    r->cookie=backing.cookie; r->monitored_value=backing.value; r->busy=FALSE;
    if (status==STATUS_SUCCESS && (r->closing || parent->closing)) status=STATUS_PROCESS_IS_TERMINATING;
    if (status==STATUS_SUCCESS && !a->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    if (status==STATUS_SUCCESS && parent->error_state) status=D3DKMT_STATUS_DEVICE_ERROR;
    if (status==STATUS_SUCCESS) {
        *output=h; *cpu_address=backing.cpu_address; *gpu_address=backing.gpu_address;
    } else {
        r->closing=TRUE;
        if (!r->cookie) { r->owner=0; r->monitored_value=NULL; parent->children--; }
    }
    d3dkmt_unlock(flags);
    if (status!=STATUS_SUCCESS) {
        if (backing.cookie) (void)monitored_retire(pid,h);
        retirement_wake();
    }
    return status;
}

NTSTATUS d3dkmt_create_paging_queue(DWORD pid,OKMT_HANDLE device,LONG priority,BOOL compat,
    OKMT_HANDLE *queue_output,OKMT_HANDLE *sync_output,uint64_t *cpu_output)
{
    if (!pid || !queue_output || !sync_output || !cpu_output || priority<-1 || priority>1 ||
        (compat!=FALSE && compat!=TRUE)) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=!parent || !parent->device || parent->busy || parent->closing ? STATUS_INVALID_PARAMETER :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state ? D3DKMT_STATUS_DEVICE_ERROR :
        !parent->driver->monitored.create ? STATUS_NOT_SUPPORTED :
        handle_available()<2 ? STATUS_NO_MEMORY : STATUS_SUCCESS;
    if (status) { d3dkmt_unlock(flags);return status; }
    KMT_ADAPTER *a=parent->driver;
    OKMT_HANDLE queue=handle_allocate(pid,parent->adapter),sync=handle_allocate(pid,parent->adapter);
    KMT_REFERENCE *q=reference_lookup(pid,queue),*r=reference_lookup(pid,sync);
    q->paging=q->busy=TRUE;q->driver=a;q->parent=device;q->children=1;q->paging_sync=sync;
    q->scheduling_priority=priority;
    r->synchronization=r->busy=TRUE;r->driver=a;r->parent=device;r->group=queue;
    r->sync_type=OKMT_SYNC_MONITORED;parent->children+=2;
    uint64_t cookie=parent->cookie;
    d3dkmt_unlock(flags);
    D3DKMT_MONITORED_BACKING backing={0};
    status=a->monitored.create(a->provider.context,pid,cookie,0,compat,&backing);
    if (!status && (!backing.cookie || !backing.value || ((uintptr_t)backing.value&7) ||
        !backing.cpu_address || (backing.cpu_address&7) || (compat && backing.cpu_address>UINT32_MAX-7) ||
        !backing.gpu_address || (backing.gpu_address&7))) status=STATUS_UNSUCCESSFUL;
    flags=d3dkmt_lock();
    r->cookie=backing.cookie;r->monitored_value=backing.value;r->busy=q->busy=FALSE;
    if (!status && (q->closing || r->closing || parent->closing)) status=STATUS_PROCESS_IS_TERMINATING;
    if (!status && !a->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    if (!status && parent->error_state) status=D3DKMT_STATUS_DEVICE_ERROR;
    if (!status) {
        *queue_output=queue;*sync_output=sync;*cpu_output=backing.cpu_address;
    } else {
        q->closing=r->closing=TRUE;
        if (!backing.cookie) {
            r->owner=0;r->monitored_value=NULL;parent->children--;q->children=0;
            resource_retire_locked(q);
        }
    }
    d3dkmt_unlock(flags);
    if (status) {
        if (backing.cookie) (void)monitored_retire(pid,sync);
        retirement_wake();
    }
    return status;
}
NTSTATUS d3dkmt_destroy_paging_queue(DWORD pid,OKMT_HANDLE queue)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *q=reference_lookup(pid,queue);
    if (!q || !q->paging || q->busy || q->closing) { d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER; }
    OKMT_HANDLE sync=q->paging_sync;
    KMT_REFERENCE *r=reference_lookup(pid,sync),*parent=reference_lookup(pid,q->parent);
    if (parent->leases==UINT32_MAX) { d3dkmt_unlock(flags);return STATUS_INSUFFICIENT_RESOURCES; }
    parent->leases++;
    D3DKMT_DEVICE_LEASE retained={&retained,pid,q->parent,parent->cookie,parent->driver->provider};
    void (*notify)(void *)=parent->driver->monitored.notify;
    /* Windows wakes pending CPU waits on explicit paging-queue destruction.
     * Publish terminal completion before closing admission. Captured GPU/CPU
     * waits still retain the fence until retirement. This queue currently has
     * no asynchronous residency operations: adding them must drain their owned
     * work before this terminal publication, never bypass active paging DMA. */
    __atomic_store_n(r->monitored_value,UINT64_MAX,__ATOMIC_RELEASE);
    q->closing=r->closing=TRUE;
    BOOL wake=cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    if (notify) notify(retained.provider.context);
    (void)monitored_retire(pid,sync);
    (void)d3dkmt_release_device(&retained);
    retirement_wake();
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_destroy_sync(DWORD pid,OKMT_HANDLE handle)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->synchronization || r->closing) { d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER; }
    if (r->group) { d3dkmt_unlock(flags);return STATUS_ACCESS_DENIED; }
    BOOL monitored=r->sync_type==OKMT_SYNC_MONITORED;
    r->closing=TRUE; sync_retire_locked(r);
    d3dkmt_unlock(flags);
    if (monitored) (void)monitored_retire(pid,handle);
    retirement_wake();
    return STATUS_SUCCESS;
}

static NTSTATUS sync_contexts_validate(const D3DKMT_SYNC_LEASE *lease)
{
    if (lease->extra_count>=OKMT_MAX_BROADCAST_CONTEXTS) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=context_work_validate(&lease->context,NULL,0);
    for (DWORD i=0;!status && i<lease->extra_count;i++) {
        const D3DKMT_CONTEXT_LEASE *extra=&lease->extra[i];
        if (extra->pid!=lease->context.pid || extra->device!=lease->context.device ||
            extra->device_cookie!=lease->context.device_cookie) return STATUS_INVALID_PARAMETER;
        status=context_work_validate(extra,NULL,0);
    }
    return status;
}
static NTSTATUS sync_contexts_release(D3DKMT_SYNC_LEASE *lease)
{
    if (lease->extra_count>=OKMT_MAX_BROADCAST_CONTEXTS) return STATUS_INVALID_PARAMETER;
    while (lease->extra_count) {
        NTSTATUS status=d3dkmt_release_context(&lease->extra[lease->extra_count-1]);
        if (status) return status;
        lease->extra_count--;
    }
    return lease->context.self ? d3dkmt_release_context(&lease->context) : STATUS_SUCCESS;
}
NTSTATUS d3dkmt_sync_admit(D3DKMT_SYNC_LEASE *lease,unsigned *publication,BOOL publish)
{
    if (!lease || lease->self!=lease || (publish && !publication) || (publish!=FALSE && publish!=TRUE))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();NTSTATUS status=sync_contexts_validate(lease);
    unsigned expected=D3DKMT_WORK_PREPARED;
    if (!status && publish && !__atomic_compare_exchange_n(publication,&expected,D3DKMT_WORK_QUEUED,FALSE,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) status=STATUS_PROCESS_IS_TERMINATING;
    d3dkmt_unlock(flags);return status;
}
NTSTATUS d3dkmt_sync_capture(DWORD pid,OKMT_HANDLE context,const D3DKMT_SYNC_COMMAND *command,D3DKMT_SYNC_LEASE *lease)
{
    if (!command || !lease || lease->self || lease->context.self || lease->event || lease->extra_count || command->count>D3DKMT_MAX_SYNC_OBJECTS ||
        (command->signal!=FALSE && command->signal!=TRUE) || (command->extended!=FALSE && command->extended!=TRUE) ||
        (command->vector_values!=FALSE && command->vector_values!=TRUE) ||
        (command->vector_values && (!command->extended || !command->count || command->event_handle)) ||
        (command->allow_rewind!=FALSE && command->allow_rewind!=TRUE) ||
        (command->allow_rewind && (!command->signal || !command->extended || !command->count)) ||
        command->broadcast_count>OKMT_MAX_BROADCAST_CONTEXTS ||
        (command->broadcast_count && (!command->signal || !command->extended || command->broadcast[0]!=context)))
        return STATUS_INVALID_PARAMETER;
    for (DWORD i=0;i<command->broadcast_count;i++) {
        if (!command->broadcast[i]) return STATUS_INVALID_PARAMETER;
        for (DWORD j=0;j<i;j++) if (command->broadcast[j]==command->broadcast[i]) return STATUS_INVALID_PARAMETER;
    }
    BOOL event=command->event_handle!=0;
    if (event ? (command->count || !command->signal || !command->extended || !command->events ||
                 !command->events->retain || !command->events->signal || !command->events->release) :
                (!command->count || command->events)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_context(pid,context,&lease->context);
    if (status!=STATUS_SUCCESS) return status;
    PVOID owned=NULL;
    for (DWORD i=1;!status && i<command->broadcast_count;i++) {
        status=d3dkmt_acquire_context(pid,command->broadcast[i],&lease->extra[lease->extra_count]);
        if (!status) lease->extra_count++;
    }
    if (!status && event) status=command->events->retain(pid,command->event_handle,&owned);
    uint64_t flags=d3dkmt_lock();
    if (status==STATUS_SUCCESS) status=sync_contexts_validate(lease);
    KMT_REFERENCE *objects[D3DKMT_MAX_SYNC_OBJECTS];
    for (DWORD i=0;status==STATUS_SUCCESS && i<command->count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,command->objects[i]); objects[i]=r;
        if (!r || !r->synchronization || r->closing || r->busy || r->parent!=lease->context.device) status=STATUS_INVALID_PARAMETER;
        else if (command->signal && r->group) status=STATUS_ACCESS_DENIED;
        else if (r->leases==UINT32_MAX) status=STATUS_INSUFFICIENT_RESOURCES;
        else if (command->vector_values && r->sync_type!=OKMT_SYNC_MONITORED) status=STATUS_INVALID_PARAMETER;
        else if ((r->sync_type==3 || r->sync_type==OKMT_SYNC_MONITORED) && !command->extended)
            status=STATUS_INVALID_PARAMETER;
        for (DWORD j=0;j<i;j++) if (command->objects[j]==command->objects[i]) status=STATUS_INVALID_PARAMETER;
    }
    if (status==STATUS_SUCCESS) {
        for (DWORD i=0;i<command->count;i++) objects[i]->leases++;
        lease->command=*command; lease->event=owned; lease->event_signal=FALSE; lease->self=lease;
    }
    d3dkmt_unlock(flags);
    if (status!=STATUS_SUCCESS) {
        if (owned) command->events->release(owned);
        (void)sync_contexts_release(lease);
    }
    return status;
}

NTSTATUS d3dkmt_sync_release(D3DKMT_SYNC_LEASE *lease)
{
    if (!lease) return STATUS_INVALID_PARAMETER;
    if (!lease->self && !lease->event) return sync_contexts_release(lease);
    if (lease->self!=lease || lease->context.self!=&lease->context || (!lease->command.count && !lease->event) ||
        lease->command.count>D3DKMT_MAX_SYNC_OBJECTS) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *objects[D3DKMT_MAX_SYNC_OBJECTS];
    for (DWORD i=0;i<lease->command.count;i++) {
        KMT_REFERENCE *r=reference_lookup(lease->context.pid,lease->command.objects[i]); objects[i]=r;
        if (!r || !r->synchronization || !r->leases || r->parent!=lease->context.device) {
            d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
        }
    }
    for (DWORD i=0;i<lease->command.count;i++) { objects[i]->leases--; sync_retire_locked(objects[i]); }
    DWORD count=lease->command.count;
    PVOID event=lease->event;
    const D3DKMT_SYNC_EVENT_PROVIDER *events=lease->command.events;
    BOOL signal=lease->event_signal;
    lease->self=NULL; lease->event=NULL; lease->event_signal=FALSE; lease->command=(D3DKMT_SYNC_COMMAND){0};
    d3dkmt_unlock(flags);
    if (event) {
        if (signal) events->signal(event,lease->context.pid);
        events->release(event);
    }
    for (DWORD i=0;i<count;i++) (void)d3dkmt_collect_syncs();
    if (count) retirement_wake();
    /* The context still retains the parent until all object references drop. */
    return sync_contexts_release(lease);
}

NTSTATUS d3dkmt_sync_step(D3DKMT_SYNC_LEASE *lease,unsigned *publication)
{
    if (!lease || lease->self!=lease || !publication || (!lease->command.count && !lease->event) ||
        lease->command.count>D3DKMT_MAX_SYNC_OBJECTS) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    NTSTATUS status=sync_contexts_validate(lease);
    KMT_REFERENCE *objects[D3DKMT_MAX_SYNC_OBJECTS];
    const D3DKMT_SYNC_COMMAND *cmd=&lease->command;
    for (DWORD i=0;status==STATUS_SUCCESS && i<cmd->count;i++) {
        KMT_REFERENCE *r=reference_lookup(lease->context.pid,cmd->objects[i]); objects[i]=r;
        uint64_t value=cmd->vector_values ? cmd->values[i] : cmd->value;
        if (!r || !r->synchronization || !r->leases || r->parent!=lease->context.device) status=STATUS_INVALID_PARAMETER;
        else if (!cmd->signal && (r->sync_type==OKMT_SYNC_MONITORED ?
            __atomic_load_n(r->monitored_value,__ATOMIC_ACQUIRE)<value :
            r->sync_type==3 ? r->sync_value<value : !r->sync_value)) status=D3DKMT_STATUS_DEVICE_BUSY;
        else if (cmd->signal && !cmd->allow_rewind && r->sync_type==OKMT_SYNC_MONITORED &&
            value<__atomic_load_n(r->monitored_value,__ATOMIC_ACQUIRE)) status=STATUS_INVALID_PARAMETER;
        else if (cmd->signal && r->sync_type==2 && r->sync_value==r->sync_maximum) status=STATUS_INVALID_PARAMETER;
    }
    unsigned expected=D3DKMT_WORK_QUEUED;
    if (status==STATUS_SUCCESS && !__atomic_compare_exchange_n(publication,&expected,D3DKMT_WORK_SUBMITTED,false,
            __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) status=STATUS_CANCELLED;
    if (status==STATUS_SUCCESS) for (DWORD i=0;i<cmd->count;i++) {
        KMT_REFERENCE *r=objects[i];
        uint64_t value=cmd->vector_values ? cmd->values[i] : cmd->value;
        if (r->sync_type==OKMT_SYNC_MONITORED) {
            if (cmd->signal) __atomic_store_n(r->monitored_value,value,__ATOMIC_RELEASE);
        } else if (r->sync_type==3) {
            if (cmd->signal && (cmd->allow_rewind || value>r->sync_value)) r->sync_value=value;
        } else if (!cmd->signal) r->sync_value--;
        else if (r->sync_type==1) r->sync_value=1;
        else r->sync_value++;
    }
    if (status==STATUS_SUCCESS && lease->event) lease->event_signal=TRUE;
    BOOL wake=status==STATUS_SUCCESS && cmd->signal && cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    return status;
}

/* Arrays are captured kernel buffers. Validate the entire set before either
 * interpreting the GPU ABI union or changing any fence memory. */
static NTSTATUS sync_set_locked(DWORD pid,OKMT_HANDLE device,DWORD count,
    const OKMT_HANDLE *handles,BOOL *monitored)
{
    BOOL native=FALSE;
    for (DWORD i=0;i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        if (!r || !r->synchronization || r->closing || r->busy || r->parent!=device)
            return STATUS_INVALID_PARAMETER;
        BOOL kind=r->sync_type==OKMT_SYNC_MONITORED;
        if (i && kind!=native) return STATUS_INVALID_PARAMETER;
        native=kind;
        for (DWORD j=0;j<i;j++) if (handles[j]==handles[i]) return STATUS_INVALID_PARAMETER;
    }
    *monitored=native; return STATUS_SUCCESS;
}
NTSTATUS d3dkmt_sync_uses_monitored(const D3DKMT_CONTEXT_LEASE *lease,DWORD count,
    const OKMT_HANDLE *handles,BOOL *monitored)
{
    if (!lease || lease->self!=lease || !count || count>D3DKMT_MAX_SYNC_OBJECTS || !handles || !monitored)
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    NTSTATUS status=context_work_validate(lease,NULL,0);
    if (!status) status=sync_set_locked(lease->pid,lease->device,count,handles,monitored);
    d3dkmt_unlock(flags); return status;
}
NTSTATUS d3dkmt_signal_sync_cpu(DWORD pid,OKMT_HANDLE device,DWORD count,
    const OKMT_HANDLE *handles,const uint64_t *values,BOOL rewind)
{
    if (!count || count>D3DKMT_MAX_SYNC_OBJECTS || !handles || !values || (rewind!=FALSE && rewind!=TRUE))
        return STATUS_INVALID_PARAMETER;
    D3DKMT_DEVICE_LEASE lease={0};
    NTSTATUS status=d3dkmt_acquire_device(pid,device,&lease);
    if (status) return status;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    status=parent->closing ? STATUS_PROCESS_IS_TERMINATING :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
    BOOL monitored=FALSE;
    if (!status) status=sync_set_locked(pid,device,count,handles,&monitored);
    if (!status && !monitored) status=STATUS_INVALID_PARAMETER;
    for (DWORD i=0;!status && i<count;i++)
        if (reference_lookup(pid,handles[i])->group) status=STATUS_ACCESS_DENIED;
    for (DWORD i=0;!status && !parent->error_state && !rewind && i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        if (values[i]<__atomic_load_n(r->monitored_value,__ATOMIC_ACQUIRE)) status=STATUS_INVALID_PARAMETER;
    }
    if (!status && !parent->error_state) for (DWORD i=0;i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        __atomic_store_n(r->monitored_value,values[i],__ATOMIC_RELEASE);
    }
    void (*notify)(void *)=parent->driver->monitored.notify;
    BOOL wake=!status && cpu_waits_update_locked();
    d3dkmt_unlock(flags);
    if (wake) cpu_waits_wake();
    if (!status && notify) notify(lease.provider.context);
    (void)d3dkmt_release_device(&lease);
    return status;
}

static void cpu_waits_wake(void)
{
    void (*wake)(void)=__atomic_load_n(&cpu_wait_waker,__ATOMIC_ACQUIRE);
    if (wake) wake();
}
BOOL d3dkmt_cpu_wait_install_waker(void (*wake)(void))
{
    if (!wake) return FALSE;
    uint64_t flags=d3dkmt_lock();
    BOOL valid=!cpu_wait_waker || cpu_wait_waker==wake;
    if (valid) __atomic_store_n(&cpu_wait_waker,wake,__ATOMIC_RELEASE);
    d3dkmt_unlock(flags);return valid;
}
static NTSTATUS cpu_wait_status_locked(D3DKMT_CPU_WAIT_LEASE *lease)
{
    if (lease->status!=STATUS_PENDING) return lease->status;
    KMT_REFERENCE *parent=reference_lookup(lease->device.pid,lease->device.handle);
    NTSTATUS status=!parent || !parent->device || !parent->leases ? STATUS_INVALID_PARAMETER :
        parent->closing ? STATUS_PROCESS_IS_TERMINATING :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_PENDING;
    if (status==STATUS_PENDING) {
        DWORD ready=0;
        for (DWORD i=0;i<lease->count;i++) {
            KMT_REFERENCE *r=reference_lookup(lease->device.pid,lease->objects[i]);
            if (!r || !r->synchronization || !r->leases || r->parent!=lease->device.handle ||
                r->sync_type!=OKMT_SYNC_MONITORED) { status=STATUS_INVALID_PARAMETER;break; }
            ready+=__atomic_load_n(r->monitored_value,__ATOMIC_ACQUIRE)>=lease->values[i];
        }
        if (status==STATUS_PENDING && (lease->any ? ready!=0 : ready==lease->count)) status=STATUS_SUCCESS;
    }
    lease->status=status;return status;
}
static BOOL cpu_waits_update_locked(void)
{
    BOOL changed=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++)
        if (cpu_waiters[i] && cpu_waiters[i]->status==STATUS_PENDING)
            changed|=cpu_wait_status_locked(cpu_waiters[i])!=STATUS_PENDING;
    return changed;
}
NTSTATUS d3dkmt_cpu_wait_capture(DWORD pid,OKMT_HANDLE device,DWORD count,
    const OKMT_HANDLE *objects,const uint64_t *values,BOOL any,D3DKMT_CPU_WAIT_LEASE *lease)
{
    if (!lease || lease->self || lease->device.self || !count || count>D3DKMT_MAX_SYNC_OBJECTS ||
        !objects || !values || (any!=FALSE && any!=TRUE)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status=d3dkmt_acquire_device(pid,device,&lease->device);
    if (status) return status;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device); BOOL monitored=FALSE;
    status=parent->closing ? STATUS_PROCESS_IS_TERMINATING :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
    DWORD slot=D3DKMT_MAX_CPU_WAITS;
    for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++) if (!cpu_waiters[i]) {slot=i;break;}
    if (!status && slot==D3DKMT_MAX_CPU_WAITS) status=STATUS_INSUFFICIENT_RESOURCES;
    if (!status) status=sync_set_locked(pid,device,count,objects,&monitored);
    if (!status && !monitored) status=STATUS_INVALID_PARAMETER;
    for (DWORD i=0;!status && i<count;i++)
        if (reference_lookup(pid,objects[i])->leases==UINT32_MAX) status=STATUS_INSUFFICIENT_RESOURCES;
    if (!status) {
        lease->self=lease;lease->count=count;lease->any=any;lease->status=STATUS_PENDING;
        for (DWORD i=0;i<count;i++) {
            reference_lookup(pid,objects[i])->leases++;
            lease->objects[i]=objects[i];lease->values[i]=values[i];
        }
        cpu_waiters[slot]=lease;(void)cpu_wait_status_locked(lease);
    }
    d3dkmt_unlock(flags);
    if (status) (void)d3dkmt_release_device(&lease->device);
    return status;
}
NTSTATUS d3dkmt_cpu_wait_status(D3DKMT_CPU_WAIT_LEASE *lease)
{
    if (!lease || lease->self!=lease) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();NTSTATUS status=cpu_wait_status_locked(lease);
    d3dkmt_unlock(flags);return status;
}
NTSTATUS d3dkmt_cpu_wait_cancel(D3DKMT_CPU_WAIT_LEASE *lease)
{
    if (!lease || lease->self!=lease) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    if (lease->status==STATUS_PENDING) lease->status=STATUS_CANCELLED;
    d3dkmt_unlock(flags);cpu_waits_wake();return STATUS_SUCCESS;
}
NTSTATUS d3dkmt_cpu_wait_release(D3DKMT_CPU_WAIT_LEASE *lease)
{
    if (!lease) return STATUS_INVALID_PARAMETER;
    if (lease->self) {
        if (lease->self!=lease || !lease->count || lease->count>D3DKMT_MAX_SYNC_OBJECTS) return STATUS_INVALID_PARAMETER;
        uint64_t flags=d3dkmt_lock();DWORD slot=D3DKMT_MAX_CPU_WAITS;
        for (DWORD i=0;i<D3DKMT_MAX_CPU_WAITS;i++) if (cpu_waiters[i]==lease) {slot=i;break;}
        BOOL valid=slot<D3DKMT_MAX_CPU_WAITS;
        for (DWORD i=0;valid && i<lease->count;i++) {
            KMT_REFERENCE *r=reference_lookup(lease->device.pid,lease->objects[i]);
            valid=r && r->synchronization && r->leases && r->parent==lease->device.handle;
        }
        if (!valid) {d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;}
        DWORD count=lease->count;
        for (DWORD i=0;i<count;i++) reference_lookup(lease->device.pid,lease->objects[i])->leases--;
        cpu_waiters[slot]=NULL;lease->self=NULL;lease->count=0;
        d3dkmt_unlock(flags);
        for (DWORD i=0;i<count;i++) (void)d3dkmt_collect_syncs();
        retirement_wake();
    }
    return lease->device.self ? d3dkmt_release_device(&lease->device) : STATUS_SUCCESS;
}

NTSTATUS d3dkmt_create_resource_allocations(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE resource,
    BOOL create_resource,DWORD count,const D3DKMT_ALLOCATION_DESCRIPTION *input,
    OKMT_HANDLE *resource_output,OKMT_HANDLE *output)
{
    if (!pid || !resource_output || (count && (!input || !output)) ||
        (!count && !create_resource) || count>D3DKMT_MAX_ALLOCATION_BATCH ||
        (create_resource && resource) || (create_resource!=FALSE && create_resource!=TRUE))
        return STATUS_INVALID_PARAMETER;
    for (DWORD i=0;i<count;i++) if (!input[i].bytes) return STATUS_INVALID_PARAMETER;
    OKMT_HANDLE handles[D3DKMT_MAX_ALLOCATION_BATCH];
    uint64_t cookies[D3DKMT_MAX_ALLOCATION_BATCH];
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    KMT_REFERENCE *group=resource ? reference_lookup(pid,resource) : NULL;
    NTSTATUS status=STATUS_SUCCESS;
    if (!parent || !parent->device || parent->busy || parent->closing) status=STATUS_INVALID_PARAMETER;
    else if (resource && (!group || !group->resource || group->parent!=device || group->closing))
        status=STATUS_INVALID_PARAMETER;
    else if (group && group->busy) status=D3DKMT_STATUS_DEVICE_BUSY;
    else if (!parent->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    else if (!parent->driver->memory.allocate) status=STATUS_NOT_SUPPORTED;
    else if (handle_available()<count+(create_resource ? 1u : 0u)) status=STATUS_NO_MEMORY;
    if (status!=STATUS_SUCCESS) { d3dkmt_unlock(flags); return status; }
    KMT_ADAPTER *a=parent->driver;
    uint64_t device_cookie=parent->cookie;
    if (create_resource) {
        resource=handle_allocate(pid,parent->adapter);
        group=reference_lookup(pid,resource);
        group->resource=TRUE; group->parent=device; group->driver=a;
        parent->children++;
    }
    if (group) { group->busy=TRUE; group->children+=count; }
    for (DWORD i=0;i<count;i++) {
        handles[i]=handle_allocate(pid,parent->adapter);
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        r->allocation=r->busy=TRUE; r->parent=device; r->group=resource; r->driver=a;
    }
    parent->children+=count;
    d3dkmt_unlock(flags);
    DWORD created=0;
    for (;created<count;created++) {
        cookies[created]=0;
        status=a->memory.allocate(a->provider.context,pid,device_cookie,&input[created],&cookies[created]);
        if (status!=STATUS_SUCCESS) break;
    }
    flags=d3dkmt_lock();
    /* The pending children retain the parent/provider throughout callbacks.
     * Closing/exiting/removal cannot publish a partially successful batch. */
    if (status==STATUS_SUCCESS && parent->closing) status=STATUS_PROCESS_IS_TERMINATING;
    if (status==STATUS_SUCCESS && group && group->closing) status=STATUS_INVALID_PARAMETER;
    if (status==STATUS_SUCCESS && !a->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    for (DWORD i=0;i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        r->busy=FALSE;
        if (i>=created) { r->owner=0; parent->children--; if (group) group->children--; }
        else { r->cookie=cookies[i]; r->closing=status!=STATUS_SUCCESS; }
    }
    if (group) {
        group->busy=FALSE;
        if (create_resource && status!=STATUS_SUCCESS) group->closing=TRUE;
        resource_retire_locked(group);
    }
    if (status==STATUS_SUCCESS) {
        *resource_output=resource;
        for (DWORD i=0;i<count;i++) output[i]=handles[i];
    }
    BOOL retire=parent->closing && !parent->children && !parent->leases && !parent->busy;
    d3dkmt_unlock(flags);
    if (status!=STATUS_SUCCESS)
        for (DWORD i=0;i<created;i++) (void)allocation_retire(pid,handles[i]);
    if (retire) (void)device_retire(pid,device);
    if (retire || status!=STATUS_SUCCESS) retirement_wake();
    return status;
}

NTSTATUS d3dkmt_create_allocations(DWORD pid,OKMT_HANDLE device,DWORD count,
    const D3DKMT_ALLOCATION_DESCRIPTION *input,OKMT_HANDLE *output)
{
    OKMT_HANDLE resource=0;
    return d3dkmt_create_resource_allocations(pid,device,0,FALSE,count,input,&resource,output);
}

NTSTATUS d3dkmt_create_private_resource_allocations(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE resource,
    BOOL create_resource,DWORD count,const D3DKMT_PRIVATE_ALLOCATION *input,
    OKMT_HANDLE *resource_output,OKMT_HANDLE *output)
{
    if (!resource_output || (count && (!input || !output)) || (!count && !create_resource) ||
        count>D3DKMT_MAX_ALLOCATION_BATCH || (create_resource && resource) ||
        (create_resource!=FALSE && create_resource!=TRUE)) return STATUS_INVALID_PARAMETER;
    for (DWORD i=0;i<count;i++)
        if (!input[i].bytes || input[i].bytes>D3DKMT_MAX_ALLOCATION_PRIVATE_BYTES) return STATUS_INVALID_PARAMETER;
    D3DKMT_DEVICE_LEASE lease={0};
    NTSTATUS status=d3dkmt_acquire_device(pid,device,&lease);
    if (status!=STATUS_SUCCESS) return status;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,device);
    D3DKMT_ALLOCATION_PROVIDER provider=r->driver->memory;
    d3dkmt_unlock(flags);
    D3DKMT_ALLOCATION_DESCRIPTION descriptions[D3DKMT_MAX_ALLOCATION_BATCH];
    if (count && !provider.translate) status=STATUS_NOT_SUPPORTED;
    for (DWORD i=0;status==STATUS_SUCCESS && i<count;i++)
        status=provider.translate(lease.provider.context,input[i].data,input[i].bytes,&descriptions[i]);
    if (status==STATUS_SUCCESS) status=d3dkmt_create_resource_allocations(pid,device,resource,
        create_resource,count,descriptions,resource_output,output);
    (void)d3dkmt_release_device(&lease);
    return status;
}

NTSTATUS d3dkmt_create_private_allocations(DWORD pid,OKMT_HANDLE device,DWORD count,
    const D3DKMT_PRIVATE_ALLOCATION *input,OKMT_HANDLE *output)
{
    OKMT_HANDLE resource=0;
    return d3dkmt_create_private_resource_allocations(pid,device,0,FALSE,count,input,&resource,output);
}

NTSTATUS d3dkmt_destroy_allocations(DWORD pid,OKMT_HANDLE device,DWORD count,const OKMT_HANDLE *handles)
{
    if (!handles || !count || count>D3DKMT_MAX_ALLOCATION_BATCH) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    if (!parent || !parent->device || parent->closing || parent->busy) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    OKMT_HANDLE group=0;
    for (DWORD i=0;i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,handles[i]);
        if (!r || !r->allocation || r->parent!=device || r->busy || r->closing ||
            (i && r->group!=group)) {
            d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
        }
        group=r->group;
        for (DWORD j=0;j<i;j++) if (handles[i]==handles[j]) {
            d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
        }
    }
    for (DWORD i=0;i<count;i++) reference_lookup(pid,handles[i])->closing=TRUE;
    d3dkmt_unlock(flags);
    for (DWORD i=0;i<count;i++) (void)allocation_retire(pid,handles[i]);
    retirement_wake();
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_destroy_resource(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE resource)
{
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device),*r=reference_lookup(pid,resource);
    if (!parent || !parent->device || parent->busy || parent->closing ||
        !r || !r->resource || r->parent!=device || r->closing) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    /* Closing also cancels a pending append. Its reserved children retain the
     * group until publication rolls back; no provider executes under lock. */
    r->closing=TRUE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        KMT_REFERENCE *child=&references[i];
        if (child->owner==pid && child->allocation && child->group==resource) child->closing=TRUE;
    }
    resource_retire_locked(r);
    d3dkmt_unlock(flags);
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        flags=d3dkmt_lock();
        KMT_REFERENCE *child=&references[i];
        OKMT_HANDLE h=child->owner==pid && child->allocation && child->group==resource &&
            !child->busy && !child->leases ? (child->generation<<HANDLE_SLOT_BITS)|i : 0;
        d3dkmt_unlock(flags);
        if (h) (void)allocation_retire(pid,h);
    }
    retirement_wake();
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_acquire_allocation(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE handle,D3DKMT_ALLOCATION_LEASE *lease)
{
    if (!lease || lease->self) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle),*parent=reference_lookup(pid,device);
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (r && r->allocation && !r->busy && !r->closing && r->parent==device &&
        parent && parent->device && !parent->busy && !parent->closing) {
        if (!parent->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
        else if (r->leases==UINT32_MAX) status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            r->leases++;
            *lease=(D3DKMT_ALLOCATION_LEASE){lease,pid,device,handle,parent->cookie,r->cookie,
                parent->driver->provider,parent->driver->memory};
            status=STATUS_SUCCESS;
        }
    }
    d3dkmt_unlock(flags);
    return status;
}

NTSTATUS d3dkmt_release_allocation(D3DKMT_ALLOCATION_LEASE *lease)
{
    if (!lease || lease->self!=lease) return STATUS_INVALID_PARAMETER;
    DWORD pid=lease->pid; OKMT_HANDLE handle=lease->handle;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=reference_lookup(pid,handle);
    if (!r || !r->allocation || !r->leases || r->parent!=lease->device || r->cookie!=lease->cookie) {
        d3dkmt_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    r->leases--;
    BOOL retire=r->closing && !r->leases && !r->busy;
    *lease=(D3DKMT_ALLOCATION_LEASE){0};
    d3dkmt_unlock(flags);
    if (retire) { (void)allocation_retire(pid,handle); retirement_wake(); }
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_allocation_properties(DWORD pid,OKMT_HANDLE device,OKMT_HANDLE resource,
    DWORD count,DWORD operation,D3DKMT_ALLOCATION_PROPERTIES_WORK *work)
{
    if (!work || (!resource && (!count || count>D3DKMT_MAX_HANDLES)) ||
        (resource && count) || (operation!=D3DKMT_PROPERTY_RESIDENCY &&
        operation!=D3DKMT_PROPERTY_SET_PRIORITY)) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    KMT_REFERENCE *group=resource ? reference_lookup(pid,resource) : NULL;
    NTSTATUS status=STATUS_SUCCESS;
    if (!parent || !parent->device || parent->busy || parent->closing)
        status=STATUS_INVALID_PARAMETER;
    else if (resource && (!group || !group->resource || group->parent!=device || group->closing))
        status=STATUS_INVALID_PARAMETER;
    else if (group && group->busy) status=D3DKMT_STATUS_DEVICE_BUSY;
    else if (!parent->driver->present) status=D3DKMT_STATUS_DEVICE_REMOVED;
    else if (!parent->driver->memory.properties) status=STATUS_NOT_SUPPORTED;
    if (status!=STATUS_SUCCESS) {d3dkmt_unlock(flags);return status;}
    if (group) {
        DWORD priority=work->values[0];
        for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
            KMT_REFERENCE *r=&references[i];
            if (r->owner!=pid || !r->allocation || r->group!=resource || r->closing) continue;
            work->handles[count]=(r->generation<<HANDLE_SLOT_BITS)|i;
            if (operation==D3DKMT_PROPERTY_SET_PRIORITY) work->values[count]=priority;
            count++;
        }
    }
    if (!count) status=STATUS_INVALID_PARAMETER;
    for (DWORD i=0;status==STATUS_SUCCESS && i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,work->handles[i]);
        if (!r || !r->allocation || r->busy || r->closing || r->parent!=device)
            status=STATUS_INVALID_PARAMETER;
        /* Conservative bound also covers a list containing repeated handles. */
        else if (r->leases>UINT32_MAX-count) status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if (status!=STATUS_SUCCESS) {d3dkmt_unlock(flags);return status;}
    KMT_ADAPTER *driver=parent->driver;
    uint64_t device_cookie=parent->cookie;
    for (DWORD i=0;i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,work->handles[i]);
        r->leases++;
        work->cookies[i]=r->cookie;
    }
    if (group) group->busy=TRUE;
    d3dkmt_unlock(flags);
    /* Every callback observes captured cookies and owns their backing through
     * these references. A concurrent close may finish logically; the admitted
     * metadata operation may complete before its deferred physical retirement. */
    status=driver->memory.properties(driver->provider.context,pid,device_cookie,
        operation,work->cookies,count,work->values);
    if (status==STATUS_SUCCESS && operation==D3DKMT_PROPERTY_RESIDENCY) {
        DWORD worst=OKMT_RESIDENT_GPU;
        for (DWORD i=0;i<count;i++) {
            if (work->values[i]<OKMT_RESIDENT_GPU || work->values[i]>OKMT_NOT_RESIDENT)
                status=STATUS_UNSUCCESSFUL;
            if (work->values[i]>worst) worst=work->values[i];
        }
        if (resource) work->values[0]=worst;
    }
    flags=d3dkmt_lock();
    if (group) {group->busy=FALSE;resource_retire_locked(group);}
    BOOL retire=FALSE;
    for (DWORD i=0;i<count;i++) {
        /* Retained references cannot be reused, even after logical close.
         * Drop the whole batch before unlocked collection; duplicate handles
         * remain valid through their final decrement under this same lock. */
        KMT_REFERENCE *r=reference_lookup(pid,work->handles[i]);
        r->leases--;retire=retire || r->closing;
    }
    d3dkmt_unlock(flags);
    if (retire) {
        for (DWORD i=0;i<count;i++) (void)allocation_retire(pid,work->handles[i]);
        retirement_wake();
    }
    return status;
}

NTSTATUS d3dkmt_collect_allocations(void)
{
    uint64_t flags=d3dkmt_lock();
    DWORD pid=0; OKMT_HANDLE handle=0; BOOL pending=FALSE;
    for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
        DWORD slot=(allocation_cursor+i)&(D3DKMT_MAX_HANDLES-1);
        KMT_REFERENCE *r=&references[slot];
        if (r->owner && r->allocation && r->closing) {
            pending=TRUE;
            if (r->busy || r->leases) continue;
            pid=r->owner; handle=(r->generation<<HANDLE_SLOT_BITS)|slot;
            allocation_cursor=(slot+1)&(D3DKMT_MAX_HANDLES-1); break;
        }
    }
    d3dkmt_unlock(flags);
    return handle ? allocation_retire(pid,handle) : pending ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

DWORD d3dkmt_query_size(DWORD type)
{
    switch (type) {
    case OKMT_QUERY_DRIVER_VERSION: case OKMT_QUERY_ADAPTER_TYPE: return sizeof(DWORD);
    case OKMT_QUERY_UMD_FILENAME: return sizeof(OKMT_UMD_FILENAME);
    case OKMT_QUERY_DEVICE_IDENTIFIERS: return sizeof(OKMT_QUERY_DEVICE_IDS);
    default: return 0;
    }
}

NTSTATUS d3dkmt_query_adapter(DWORD pid, OKMT_HANDLE handle, DWORD type,
                               void *data, DWORD bytes, BOOL compat32)
{
    DWORD required=d3dkmt_query_size(type);
    BOOL private_query=type==OKMT_QUERY_PRIVATE_DRIVER_DATA;
    if (!data || (private_query ? (!bytes || bytes>D3DKMT_MAX_ADAPTER_PRIVATE_BYTES) :
                  (!required || bytes!=required))) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *r=handle_lookup(pid,handle);
    KMT_ADAPTER *a=r ? adapter_lookup(r->adapter) : NULL;
    NTSTATUS status=STATUS_SUCCESS;
    if (!r) status=STATUS_INVALID_PARAMETER;
    else if (!a) status=D3DKMT_STATUS_DEVICE_REMOVED;
    else {
        const D3DKMT_ADAPTER_DESCRIPTION *d=&a->description;
        switch (type) {
        case OKMT_QUERY_PRIVATE_DRIVER_DATA:
            if (!d->private_bytes) status=STATUS_NOT_SUPPORTED;
            else if (bytes!=d->private_bytes) status=STATUS_INVALID_PARAMETER;
            else for (DWORD i=0;i<bytes;i++) ((BYTE *)data)[i]=d->private_data[i];
            break;
        case OKMT_QUERY_DRIVER_VERSION: *(DWORD *)data=d->driver_version; break;
        case OKMT_QUERY_ADAPTER_TYPE: *(DWORD *)data=d->adapter_type; break;
        case OKMT_QUERY_DEVICE_IDENTIFIERS:
            if (((OKMT_QUERY_DEVICE_IDS *)data)->physical_index)
                status=STATUS_INVALID_PARAMETER;
            else *(OKMT_QUERY_DEVICE_IDS *)data=d->device_ids;
            break;
        case OKMT_QUERY_UMD_FILENAME: {
            OKMT_UMD_FILENAME *name=data;
            const OKMT_WCHAR *path=compat32 ? d->umd32 : d->umd64;
            if (name->version!=OKMT_UMD_DX9 || !path[0]) status=STATUS_NOT_SUPPORTED;
            else for (DWORD i=0;i<260;i++) name->name[i]=path[i];
            break;
        }
        }
    }
    d3dkmt_unlock(flags);
    return status;
}

NTSTATUS d3dkmt_residency_prepare(DWORD pid,OKMT_HANDLE handle,BOOL make,DWORD count,
    D3DKMT_RESIDENCY_WORK *work)
{
    if (!pid || !work || work->self || !count || count>D3DKMT_MAX_HANDLES ||
        (make!=FALSE && make!=TRUE)) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *q=make ? reference_lookup(pid,handle) : NULL;
    if (make && (!q || !q->paging || q->closing || q->busy)) {
        d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;
    }
    OKMT_HANDLE device=make ? q->parent : handle;
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=!parent || !parent->device || parent->closing || parent->busy ? STATUS_INVALID_PARAMETER :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state || (parent->driver->provider.fault_state &&
            __atomic_load_n(parent->driver->provider.fault_state,__ATOMIC_ACQUIRE)) ? D3DKMT_STATUS_DEVICE_ERROR :
        !parent->driver->memory.residency ? STATUS_NOT_SUPPORTED :
        q && q->leases==UINT32_MAX ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    for (DWORD i=0;status==STATUS_SUCCESS && i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,work->handles[i]);
        if (!r || !r->allocation || r->parent!=device || r->closing || r->busy) status=STATUS_INVALID_PARAMETER;
        else if (r->leases>UINT32_MAX-count) status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!status) {
        for (DWORD i=0;i<count;i++) {
            KMT_REFERENCE *r=reference_lookup(pid,work->handles[i]);
            r->leases++;work->cookies[i]=r->cookie;
        }
        if (q) q->leases++;
        work->self=work;work->pid=pid;work->count=count;work->device=device;
        work->queue=make?handle:0;work->make=make;work->attempted=FALSE;
        work->device_cookie=parent->cookie;work->provider_context=parent->driver->provider.context;
        work->commit=parent->driver->memory.residency;
    }
    d3dkmt_unlock(flags);return status;
}

NTSTATUS d3dkmt_residency_commit(D3DKMT_RESIDENCY_WORK *work)
{
    if (!work || work->self!=work || work->attempted || !work->count || work->count>D3DKMT_MAX_HANDLES)
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *parent=reference_lookup(work->pid,work->device);
    KMT_REFERENCE *q=work->queue ? reference_lookup(work->pid,work->queue) : NULL;
    NTSTATUS status=!parent || !parent->device || parent->cookie!=work->device_cookie ||
        parent->driver->provider.context!=work->provider_context || parent->driver->memory.residency!=work->commit ||
        (work->make && (!q || !q->paging || !q->leases || q->parent!=work->device)) ? STATUS_INVALID_PARAMETER :
        parent->closing || (q && q->closing) ? STATUS_PROCESS_IS_TERMINATING :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state || (parent->driver->provider.fault_state &&
            __atomic_load_n(parent->driver->provider.fault_state,__ATOMIC_ACQUIRE)) ? D3DKMT_STATUS_DEVICE_ERROR : STATUS_SUCCESS;
    for (DWORD i=0;!status && i<work->count;i++) {
        KMT_REFERENCE *r=reference_lookup(work->pid,work->handles[i]);
        if (!r || !r->allocation || !r->leases || r->parent!=work->device || r->cookie!=work->cookies[i])
            status=STATUS_INVALID_PARAMETER;
        else if (r->closing) status=STATUS_PROCESS_IS_TERMINATING;
    }
    work->attempted=TRUE;
    d3dkmt_unlock(flags);
    /* All cookies and provider storage remain owned after admission, including
     * concurrent logical close. The immediate provider never frees/maps/pages
     * or calls back. Its native lock makes the whole metadata batch atomic. */
    if (!status) status=work->commit(work->provider_context,work->pid,work->device_cookie,
        work->make,work->cookies,work->count);
    return status>0 ? STATUS_UNSUCCESSFUL : status;
}

NTSTATUS d3dkmt_residency_release(D3DKMT_RESIDENCY_WORK *work)
{
    if (!work || work->self!=work || !work->count || work->count>D3DKMT_MAX_HANDLES)
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();BOOL closing=FALSE;
    KMT_REFERENCE *q=work->queue ? reference_lookup(work->pid,work->queue) : NULL;
    if (work->queue && (!q || !q->paging || !q->leases)) {
        d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;
    }
    for (DWORD i=0;i<work->count;i++) {
        KMT_REFERENCE *r=reference_lookup(work->pid,work->handles[i]);
        if (!r || !r->allocation || !r->leases || r->parent!=work->device || r->cookie!=work->cookies[i]) {
            d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;
        }
    }
    for (DWORD i=0;i<work->count;i++) {
        KMT_REFERENCE *r=reference_lookup(work->pid,work->handles[i]);
        r->leases--;closing|=r->closing;
    }
    if (q) {q->leases--;closing|=q->closing;resource_retire_locked(q);}
    work->self=NULL;
    d3dkmt_unlock(flags);
    if (closing) {
        for (DWORD i=0;i<work->count;i++) (void)allocation_retire(work->pid,work->handles[i]);
        retirement_wake();
    }
    return STATUS_SUCCESS;
}

NTSTATUS d3dkmt_offer_prepare(DWORD pid,OKMT_HANDLE target,BOOL resources,DWORD count,
    DWORD mode,DWORD priority,DWORD options,D3DKMT_OFFER_WORK *w)
{
    if (!pid || !target || !w || w->self || !count || count>D3DKMT_MAX_HANDLES ||
        (resources!=FALSE && resources!=TRUE) || mode>D3DKMT_RECLAIM2 ||
        (mode==D3DKMT_OFFER ? priority<1 || priority>4 || (options&~3u) : priority || options))
        return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    KMT_REFERENCE *q=mode==D3DKMT_RECLAIM2 ? reference_lookup(pid,target) : NULL;
    if (mode==D3DKMT_RECLAIM2 && (!q || !q->paging || q->closing || q->busy)) {
        d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;
    }
    OKMT_HANDLE device=q ? q->parent : target;
    KMT_REFERENCE *parent=reference_lookup(pid,device);
    NTSTATUS status=!parent || !parent->device || parent->closing || parent->busy ? STATUS_INVALID_PARAMETER :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state || (parent->driver->provider.fault_state &&
            __atomic_load_n(parent->driver->provider.fault_state,__ATOMIC_ACQUIRE)) ? D3DKMT_STATUS_DEVICE_ERROR :
        !parent->driver->memory.offer || !parent->driver->memory.offer->prepare ||
        !parent->driver->memory.offer->commit || !parent->driver->memory.offer->finish ? STATUS_NOT_SUPPORTED :
        q && q->leases==UINT32_MAX ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    w->allocation_count=w->group_count=0;
    /* Resolve all groups before taking ownership. Busy prevents an append from
     * changing membership while the provider is preparing replacement pages. */
    for (DWORD i=0;!status && i<count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,w->inputs[i]);
        if (!r || r->parent!=device || r->closing || (resources ? !r->resource : !r->allocation))
            status=STATUS_INVALID_PARAMETER;
        else if (r->busy) status=D3DKMT_STATUS_DEVICE_BUSY;
        else if (resources) {
            BOOL seen=FALSE;
            for (DWORD j=0;j<w->group_count;j++) if (w->groups[j]==w->inputs[i]) seen=TRUE;
            if (!seen) w->groups[w->group_count++]=w->inputs[i];
        } else w->handles[w->allocation_count++]=w->inputs[i];
    }
    for (DWORD g=0;!status && g<w->group_count;g++) {
        DWORD start=w->allocation_count;
        for (DWORD i=0;i<D3DKMT_MAX_HANDLES;i++) {
            KMT_REFERENCE *r=&references[i];
            if (r->owner!=pid || !r->allocation || r->group!=w->groups[g]) continue;
            if (r->closing || r->busy || r->parent!=device) {status=D3DKMT_STATUS_DEVICE_BUSY;break;}
            if (w->allocation_count==D3DKMT_MAX_HANDLES) {status=STATUS_INSUFFICIENT_RESOURCES;break;}
            DWORD next=w->allocation_count++;
            w->handles[next]=(r->generation<<HANDLE_SLOT_BITS)|i;
            w->membership[next]=w->groups[g];
        }
        if (!status && start==w->allocation_count) status=STATUS_INVALID_PARAMETER;
    }
    for (DWORD i=0;!status && i<w->allocation_count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,w->handles[i]);
        if (r->leases>UINT32_MAX-w->allocation_count) status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if (status) {d3dkmt_unlock(flags);return status;}
    for (DWORD i=0;i<w->allocation_count;i++) {
        KMT_REFERENCE *r=reference_lookup(pid,w->handles[i]);
        r->leases++;w->cookies[i]=r->cookie;
    }
    for (DWORD i=0;i<w->group_count;i++) reference_lookup(pid,w->groups[i])->busy=TRUE;
    if (q) q->leases++;
    w->self=w;w->pid=pid;w->count=count;w->mode=mode;w->priority=priority;w->options=options;
    w->device=device;w->queue=q ? target : 0;w->resources=resources;w->attempted=w->prepared=FALSE;
    w->device_cookie=parent->cookie;w->provider_context=parent->driver->provider.context;
    w->provider=parent->driver->memory.offer;
    d3dkmt_unlock(flags);
    status=w->provider->prepare(w->provider_context,pid,w->device_cookie,mode,priority,options,
        w->cookies,w->allocation_count,w->storage,w->values);
    if (!status) {
        w->prepared=TRUE;
        if (mode!=D3DKMT_OFFER) {
            for (DWORD i=0;i<w->allocation_count;i++) if (w->values[i]>1) status=STATUS_UNSUCCESSFUL;
            for (DWORD i=0;!status && i<count;i++) {
                DWORD lost=0;
                if (resources) {
                    for (DWORD j=0;j<w->allocation_count;j++)
                        if (w->membership[j]==w->inputs[i]) lost|=w->values[j];
                } else lost=w->values[i];
                w->results[i]=lost;
            }
        }
    }
    if (status) (void)d3dkmt_offer_release(w);
    return status>0 ? STATUS_UNSUCCESSFUL : status;
}

NTSTATUS d3dkmt_offer_commit(D3DKMT_OFFER_WORK *w)
{
    if (!w || w->self!=w || !w->prepared) return STATUS_INVALID_PARAMETER;
    uint64_t flags=d3dkmt_lock();
    if (w->attempted) {d3dkmt_unlock(flags);return STATUS_INVALID_PARAMETER;}
    w->attempted=TRUE;
    KMT_REFERENCE *parent=reference_lookup(w->pid,w->device);
    KMT_REFERENCE *q=w->queue ? reference_lookup(w->pid,w->queue) : NULL;
    NTSTATUS status=!parent || !parent->device || parent->cookie!=w->device_cookie ||
        parent->driver->provider.context!=w->provider_context || parent->driver->memory.offer!=w->provider ||
        (w->queue && (!q || !q->paging || !q->leases || q->parent!=w->device)) ? STATUS_INVALID_PARAMETER :
        parent->closing || (q && q->closing) ? STATUS_PROCESS_IS_TERMINATING :
        !parent->driver->present ? D3DKMT_STATUS_DEVICE_REMOVED :
        parent->error_state || (parent->driver->provider.fault_state &&
            __atomic_load_n(parent->driver->provider.fault_state,__ATOMIC_ACQUIRE)) ? D3DKMT_STATUS_DEVICE_ERROR : STATUS_SUCCESS;
    for (DWORD i=0;!status && i<w->allocation_count;i++) {
        KMT_REFERENCE *r=reference_lookup(w->pid,w->handles[i]);
        if (!r || !r->allocation || !r->leases || r->parent!=w->device || r->cookie!=w->cookies[i])
            status=STATUS_INVALID_PARAMETER;
        else if (r->closing) status=STATUS_PROCESS_IS_TERMINATING;
    }
    for (DWORD i=0;!status && i<w->group_count;i++) {
        KMT_REFERENCE *r=reference_lookup(w->pid,w->groups[i]);
        if (!r || !r->resource || !r->busy || r->parent!=w->device) status=STATUS_INVALID_PARAMETER;
        else if (r->closing) status=STATUS_PROCESS_IS_TERMINATING;
    }
    d3dkmt_unlock(flags);
    /* Native close/fault admission is independently rechecked by the provider;
     * all registry ownership persists even if logical teardown races here. */
    if (!status) status=w->provider->commit(w->storage);
    return status>0 ? STATUS_UNSUCCESSFUL : status;
}

NTSTATUS d3dkmt_offer_release(D3DKMT_OFFER_WORK *w)
{
    if (!w || w->self!=w) return STATUS_INVALID_PARAMETER;
    /* Caller has quiesced commit. Provider finish may release native gates,
     * trim offered pages or wake a collector; registry references still live. */
    if (w->prepared) w->provider->finish(w->storage);
    uint64_t flags=d3dkmt_lock();BOOL closing=FALSE;
    for (DWORD i=0;i<w->allocation_count;i++) {
        KMT_REFERENCE *r=reference_lookup(w->pid,w->handles[i]);
        r->leases--;closing|=r->closing;
    }
    for (DWORD i=0;i<w->group_count;i++) {
        KMT_REFERENCE *r=reference_lookup(w->pid,w->groups[i]);
        r->busy=FALSE;closing|=r->closing;resource_retire_locked(r);
    }
    if (w->queue) {
        KMT_REFERENCE *q=reference_lookup(w->pid,w->queue);
        q->leases--;closing|=q->closing;resource_retire_locked(q);
    }
    w->self=NULL;
    d3dkmt_unlock(flags);
    if (closing) {
        for (DWORD i=0;i<w->allocation_count;i++) (void)allocation_retire(w->pid,w->handles[i]);
        retirement_wake();
    }
    return STATUS_SUCCESS;
}
