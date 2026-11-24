#include "intel_gpu_memory.h"
#ifndef __KERNEL_X86__
#include <string.h>
#endif
static intel_gpu_memory_t *memory_registry[8];
static unsigned memory_registry_lock;
static uint64_t gpu_memory_lock(unsigned *lock)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_exchange_n(lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    return flags;
}
static void gpu_memory_unlock(unsigned *lock, uint64_t flags)
{
    __atomic_store_n(lock,0,__ATOMIC_RELEASE);
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
#define MEMORY_VALID(m) ((m) && (m)->self==(m) && (m)->initialized)
#define MEMORY_RETURN(m,f,v) do { enum intel_gpu_buffer_status r_=(v); \
    gpu_memory_unlock(&(m)->lock,f); return r_; } while (0)

static intel_gpu_memory_device_t *gpu_memory_device(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device)
{
    if (!pid || !device) return NULL;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
        if (m->devices[i].id==device && m->devices[i].pid==pid) return &m->devices[i];
    return NULL;
}
static intel_gpu_memory_resource_t *gpu_memory_resource(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device, uint32_t resource)
{
    if (!pid || !device || !resource) return NULL;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (m->resources[i].id==resource && m->resources[i].pid==pid &&
            m->resources[i].device==device) return &m->resources[i];
    return NULL;
}
static void gpu_memory_close_device(intel_gpu_memory_t *m, intel_gpu_memory_device_t *d)
{
    d->closing=true;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (m->resources[i].id && m->resources[i].device==d->id) m->resources[i].closing=true;
}
static bool gpu_memory_reap_devices(intel_gpu_memory_t *m)
{
    bool retained=false;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) {
        intel_gpu_memory_device_t *d=&m->devices[i];
        if (!d->closing) continue;
        if (!d->allocations && !d->references) memset(d,0,sizeof(*d));
        else retained=true;
    }
    return retained;
}

bool intel_gpu_memory_init(intel_gpu_memory_t *m, const intel_gpu_allocator_t *a,
    void *ctx, uint32_t total, uint32_t process)
{
    if (!m || m->initialized || !a || !a->allocate || !a->validate || !a->sync || !a->release ||
        !total || !process || process>total) return false;
    m->self=m; m->allocator=*a; m->allocator_context=ctx;
    m->page_limit=total; m->process_page_limit=process; m->next_id=1; m->initialized=true;
    return true;
}

static enum intel_gpu_buffer_status gpu_memory_open_device(intel_gpu_memory_t *m,
    uint32_t pid, uint64_t epoch, uint32_t *device, bool retained)
{
    if (!MEMORY_VALID(m) || !pid || !device) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    if (m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (m->next_id==UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NO_MEMORY);
    /* Exit closes this incarnation's admission while its records remain. An
     * ordinary device close must permit a live owner to create another one;
     * outstanding resources, including old epochs, share the PID quota. */
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
        if (m->devices[i].pid==pid && m->devices[i].epoch==epoch && m->devices[i].exiting)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) if (!m->devices[i].id) {
        m->devices[i]=(intel_gpu_memory_device_t){.id=m->next_id++,.pid=pid,.epoch=epoch,.references=retained ? 1u : 0u};
        *device=m->devices[i].id;
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
    }
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NO_MEMORY);
}

enum intel_gpu_buffer_status intel_gpu_memory_open_device(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t *device)
{ return gpu_memory_open_device(m,pid,0,device,false); }

enum intel_gpu_buffer_status intel_gpu_memory_open_device_retained(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t *device)
{ return gpu_memory_open_device(m,pid,0,device,true); }

enum intel_gpu_buffer_status intel_gpu_memory_open_device_epoch(intel_gpu_memory_t *m,
    uint32_t pid,uint64_t epoch,uint32_t *device,bool retained)
{
    if (!epoch) return INTEL_GPU_BUFFER_INVALID;
    return gpu_memory_open_device(m,pid,epoch,device,retained);
}

enum intel_gpu_buffer_status intel_gpu_memory_device_epoch(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint64_t *epoch)
{
    if (!MEMORY_VALID(m) || !epoch) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    *epoch=d->epoch;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_release_device(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || !d->closing || !d->references) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    d->references--;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

static bool gpu_memory_collectable(const intel_gpu_memory_resource_t *r)
{
    return r->id && r->closing && !r->busy && !r->references &&
        !r->cpu_holder && !r->coherent_holder;
}

static bool gpu_memory_offered(const intel_gpu_memory_resource_t *r)
{
    return r->id && r->offer_priority && !r->closing && !r->busy && r->pages &&
        !r->cpu_holder && !r->coherent_holder && !r->pending_work;
}

enum intel_gpu_buffer_status intel_gpu_memory_offer(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,const uint64_t *cookies,uint32_t count,uint32_t priority)
{
    if (!MEMORY_VALID(m) || !cookies || !count || count>1024 || priority<1 || priority>4)
        return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock),selected=0;
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (uint32_t i=0;i<count;i++) {
        if (!cookies[i] || cookies[i]>UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        if (!r || r->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        if (r->busy || r->cpu_holder || r->coherent_holder || r->cpu_access.self || r->coherent_access.self)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
        if ((!r->buffer.live || !r->buffer.validated) && !r->discarded)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        selected|=1ULL<<(r-m->resources);
    }
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (selected&(1ULL<<i)) m->resources[i].offer_priority=priority;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

/* AUTO uses the allocation's current eviction hint. It remains a policy hint,
 * not a promise to evict, and updates to that hint take effect before trim. */
static uint32_t gpu_memory_offer_rank(const intel_gpu_memory_resource_t *r)
{
    if (r->offer_priority!=4) return r->offer_priority;
    return r->priority<0x78000000u ? 1 : r->priority>0x78000000u ? 3 : 2;
}

static enum intel_gpu_buffer_status gpu_memory_trim(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t target,uint32_t *released,bool deferred)
{
    if (!MEMORY_VALID(m) || !target || !released) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock),visited=0;
    if (m->trimming || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    m->trimming=true;
    uint32_t total=0;bool notify=false;
    for (unsigned round=0;round<INTEL_GPU_MEMORY_RESOURCES && total<target;round++) {
        intel_gpu_memory_resource_t *r=NULL;
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
            intel_gpu_memory_resource_t *candidate=&m->resources[i];
            if (!(visited&(1ULL<<i)) && (!pid || candidate->pid==pid) &&
                candidate->id && candidate->offer_priority && !candidate->closing && candidate->pages &&
                (!deferred || candidate->trim_requested) &&
                (!r || gpu_memory_offer_rank(candidate)<gpu_memory_offer_rank(r)))
                r=candidate;
        }
        if (!r || m->closing) break;
        visited|=1ULL<<(r-m->resources);
        if (!deferred && !r->trim_requested) { r->trim_requested=true;notify=true; }
        if (!gpu_memory_offered(r)) continue;
        r->busy=true;
        /* The occupied, busy resource retains its device/manager while unlocked.
         * Only the buffer gate can prove all physical references have retired. */
        gpu_memory_unlock(&m->lock,flags);
        enum intel_gpu_buffer_status result=intel_gpu_buffer_discard(&r->buffer,r->pid);
        flags=gpu_memory_lock(&m->lock);
        if (result==INTEL_GPU_BUFFER_OK) {
            intel_gpu_memory_device_t *d=gpu_memory_device(m,r->pid,r->device);
            total+=r->pages;d->pages-=r->pages;m->pages-=r->pages;r->pages=0;
            r->discarded=true;r->trim_requested=false;
        }
        r->busy=false;
    }
    m->trimming=false;*released=total;
    void (*wake)(void *)=notify ? m->mapping_wake : NULL;
    gpu_memory_unlock(&m->lock,flags);
    if (wake) wake(NULL);
    return INTEL_GPU_BUFFER_OK;
}

enum intel_gpu_buffer_status intel_gpu_memory_trim(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t target,uint32_t *released)
{
    return gpu_memory_trim(m,pid,target,released,false);
}
enum intel_gpu_buffer_status intel_gpu_memory_trim_deferred(intel_gpu_memory_t *m,uint32_t *released)
{
    return gpu_memory_trim(m,0,UINT32_MAX,released,true);
}
bool intel_gpu_memory_watch_mappings(intel_gpu_memory_t *m,void (*wake)(void *))
{
    if (!MEMORY_VALID(m) || !wake) return false;
    uint64_t flags=gpu_memory_lock(&m->lock);
    bool ready=!m->closing && (!m->mapping_wake || m->mapping_wake==wake);
    if (ready) m->mapping_wake=wake;
    gpu_memory_unlock(&m->lock,flags);
    return ready;
}
bool intel_gpu_memory_mapping_unwanted(intel_gpu_memory_t *m,const intel_gpu_buffer_t *buffer)
{
    if (!MEMORY_VALID(m) || !buffer) return false;
    uint64_t flags=gpu_memory_lock(&m->lock);bool unwanted=false;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
        const intel_gpu_memory_resource_t *r=&m->resources[i];
        if (&r->buffer==buffer) { unwanted=r->id && r->offer_priority && r->trim_requested;break; }
    }
    gpu_memory_unlock(&m->lock,flags);
    return unwanted;
}

enum intel_gpu_buffer_status intel_gpu_memory_reclaim_release(intel_gpu_memory_reclaim_work_t *w)
{
    if (!w || w->self!=w || !MEMORY_VALID(w->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=w->memory;uint64_t flags=gpu_memory_lock(&m->lock);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (w->selected&(1ULL<<i)) {
        intel_gpu_memory_resource_t *r=&m->resources[i];
        if (r->id!=w->ids[i] || r->reclaim_owner!=w || !r->busy)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    }
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (w->selected&(1ULL<<i)) {
        m->resources[i].busy=false;m->resources[i].reclaim_owner=NULL;
    }
    memset(w,0,sizeof(*w));
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_reclaim_prepare(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,const uint64_t *cookies,uint32_t count,intel_gpu_memory_reclaim_work_t *w)
{
    if (!MEMORY_VALID(m) || !cookies || !count || count>1024 || !w || w->self)
        return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock),selected=0;
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (uint32_t i=0;i<count;i++) {
        if (!cookies[i] || cookies[i]>UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        if (!r || r->closing || !r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
        selected|=1ULL<<(r-m->resources);
    }
    *w=(intel_gpu_memory_reclaim_work_t){.self=w,.memory=m,.pid=pid,.device=device,
        .count=count,.selected=selected};
    for (uint32_t i=0;i<count;i++) {
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        unsigned slot=(unsigned)(r-m->resources);w->slots[i]=(uint8_t)slot;w->ids[slot]=r->id;
        r->busy=true;r->reclaim_owner=w;
    }
    enum intel_gpu_buffer_status status=INTEL_GPU_BUFFER_OK;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (selected&(1ULL<<i)) {
        intel_gpu_memory_resource_t *r=&m->resources[i];
        if (r->closing || d->closing || m->closing) {status=INTEL_GPU_BUFFER_INVALID;break;}
        if (!r->discarded || !r->buffer.discarded) continue;
        if (!r->pages) {
            uint64_t process_pages=0;
            for (unsigned j=0;j<INTEL_GPU_MEMORY_DEVICES;j++)
                if (m->devices[j].pid==pid) process_pages+=m->devices[j].pages;
            uint32_t pages=r->buffer.pages;
            if (pages>m->page_limit-m->pages || process_pages+pages>m->process_page_limit) {
                uint32_t trim_pid=process_pages+pages>m->process_page_limit ? pid : 0;
                uint64_t global_need=(uint64_t)m->pages+pages>m->page_limit ?
                    (uint64_t)m->pages+pages-m->page_limit : 0;
                uint64_t owner_need=process_pages+pages>m->process_page_limit ?
                    process_pages+pages-m->process_page_limit : 0;
                uint32_t target=(uint32_t)(global_need>owner_need ? global_need : owner_need),released=0;
                gpu_memory_unlock(&m->lock,flags);
                (void)intel_gpu_memory_trim(m,trim_pid,target,&released);
                flags=gpu_memory_lock(&m->lock);
                if (r->closing || d->closing || m->closing) {status=INTEL_GPU_BUFFER_INVALID;break;}
                process_pages=0;
                for (unsigned j=0;j<INTEL_GPU_MEMORY_DEVICES;j++)
                    if (m->devices[j].pid==pid) process_pages+=m->devices[j].pages;
                if (pages>m->page_limit-m->pages || process_pages+pages>m->process_page_limit) {
                    status=INTEL_GPU_BUFFER_NO_MEMORY;break;
                }
            }
            r->pages=pages;m->pages+=pages;d->pages+=pages;
        }
        gpu_memory_unlock(&m->lock,flags);
        status=intel_gpu_buffer_recommit(&r->buffer,pid);
        flags=gpu_memory_lock(&m->lock);
        if (!r->buffer.live) {m->pages-=r->pages;d->pages-=r->pages;r->pages=0;}
        if (status!=INTEL_GPU_BUFFER_OK) break;
    }
    if (status==INTEL_GPU_BUFFER_OK) {
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (selected&(1ULL<<i))
            if (m->resources[i].closing || d->closing || m->closing) status=INTEL_GPU_BUFFER_INVALID;
    }
    if (status==INTEL_GPU_BUFFER_OK)
        for (uint32_t i=0;i<count;i++) w->discarded[i]=m->resources[w->slots[i]].discarded ? 1u : 0u;
    gpu_memory_unlock(&m->lock,flags);
    if (status!=INTEL_GPU_BUFFER_OK) (void)intel_gpu_memory_reclaim_release(w);
    return status;
}

enum intel_gpu_buffer_status intel_gpu_memory_reclaim_commit(intel_gpu_memory_reclaim_work_t *w)
{
    if (!w || w->self!=w || !MEMORY_VALID(w->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=w->memory;uint64_t flags=gpu_memory_lock(&m->lock);
    if (w->attempted) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    w->attempted=true;
    intel_gpu_memory_device_t *d=gpu_memory_device(m,w->pid,w->device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (w->selected&(1ULL<<i)) {
        intel_gpu_memory_resource_t *r=&m->resources[i];
        if (r->id!=w->ids[i] || r->reclaim_owner!=w || !r->busy || r->closing)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    }
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (w->selected&(1ULL<<i)) {
        m->resources[i].offer_priority=0;m->resources[i].discarded=false;m->resources[i].trim_requested=false;
    }
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

static enum intel_gpu_buffer_status gpu_memory_create(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device, uint64_t bytes, uint32_t *resource,bool retained)
{
    if (!MEMORY_VALID(m) || !resource || !bytes || bytes>INTEL_GPU_BUFFER_MAX_BYTES)
        return INTEL_GPU_BUFFER_INVALID;
    uint32_t pages=(uint32_t)((bytes+4095)/4096);
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=NULL,*anchor=NULL;
    intel_gpu_memory_resource_t *r=NULL;
    enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_NO_MEMORY;
    unsigned remaining=0;
    for (;;) {
        d=gpu_memory_device(m,pid,device);
        if (!d || d->closing || m->closing) { result=INTEL_GPU_BUFFER_INVALID; break; }
        if (m->next_id==UINT32_MAX || pages>m->page_limit || pages>m->process_page_limit) break;
        uint64_t process_pages=0;
        for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
            if (m->devices[i].pid==pid) process_pages+=m->devices[i].pages;
        r=NULL;
        for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
            if (!m->resources[i].id) { r=&m->resources[i]; break; }
        if (r && pages<=m->page_limit-m->pages && process_pages+pages<=m->process_page_limit) {
            result=INTEL_GPU_BUFFER_OK; break;
        }
        if (!anchor) {
            if (m->reclaiming || m->trimming || d->references==UINT32_MAX) break;
            for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
                remaining+=gpu_memory_collectable(&m->resources[i]) || gpu_memory_offered(&m->resources[i]);
            if (!remaining) break;
            /* Retain the device across unlocked allocator callbacks, including
             * close/exit and the final collected page. This also prevents
             * manager unpublication. The guard bounds reentrant collection. */
            anchor=d; anchor->references++; m->reclaiming=true;
        }
        if (!remaining) break;
        remaining--;
        uint32_t trim_pid=process_pages+pages>m->process_page_limit ? pid : 0;
        gpu_memory_unlock(&m->lock,flags);
        (void)intel_gpu_memory_collect(m);
        flags=gpu_memory_lock(&m->lock);
        /* Prefer ordinary destruction. If quota still blocks the request,
         * discard only explicitly offered backing, with physical references
         * enforced by its buffer gate and a bounded nonrecursive trim pass. */
        process_pages=0;
        for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
            if (m->devices[i].pid==pid) process_pages+=m->devices[i].pages;
        if (pages>m->page_limit-m->pages || process_pages+pages>m->process_page_limit) {
            trim_pid=process_pages+pages>m->process_page_limit ? pid : 0;
            gpu_memory_unlock(&m->lock,flags);
            uint32_t released=0;(void)intel_gpu_memory_trim(m,trim_pid,1,&released);
            flags=gpu_memory_lock(&m->lock);
        }
        /* Recheck identity, admission, quota and slot after every callback.
         * Busy, pinned or quarantined backing remains charged, with no wait
         * for GPU completion and no retry beyond the initial bounded pass. */
    }
    if (anchor) { anchor->references--; m->reclaiming=false; }
    if (result!=INTEL_GPU_BUFFER_OK) {
        if (anchor) gpu_memory_reap_devices(m);
        MEMORY_RETURN(m,flags,result);
    }
    r->id=m->next_id++; r->device=device; r->pid=pid; r->pages=pages; r->busy=true;
    r->priority=0x78000000u; /* D3DDDI_ALLOCATIONPRIORITY_NORMAL */
    r->residency_refs=1;
    r->references=retained ? 1u : 0u;
    d->pages+=pages; d->allocations++; m->pages+=pages;
    gpu_memory_unlock(&m->lock,flags);
    result=intel_gpu_buffer_create(&r->buffer,&m->allocator,
        m->allocator_context,pid,bytes);
    flags=gpu_memory_lock(&m->lock);
    r->busy=false;
    if (result!=INTEL_GPU_BUFFER_OK) r->closing=true;
    else if (r->closing) result=INTEL_GPU_BUFFER_INVALID; /* exit won allocation */
    else *resource=r->id;
    if (result!=INTEL_GPU_BUFFER_OK) r->references=0;
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_create(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint64_t bytes,uint32_t *resource)
{ return gpu_memory_create(m,pid,device,bytes,resource,false); }

enum intel_gpu_buffer_status intel_gpu_memory_properties(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,unsigned operation,const uint64_t *cookies,
    uint32_t count,uint32_t *values)
{
    if (!MEMORY_VALID(m) || !cookies || !values || !count || count>1024 ||
        operation<INTEL_GPU_PROPERTY_RESIDENCY || operation>INTEL_GPU_PROPERTY_GET_PRIORITY)
        return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (uint32_t i=0;i<count;i++) {
        if (!cookies[i] || cookies[i]>UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        if (!r || r->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        /* Ordinary CPU cache transitions do not change backing residency.
         * Only discard/recommit mutate these fields outside the manager lock. */
        if (r->reclaim_owner || (r->offer_priority && r->busy))
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
        if ((!r->buffer.live || !r->buffer.validated) && !r->discarded)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    }
    for (uint32_t i=0;i<count;i++) {
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        if (operation==INTEL_GPU_PROPERTY_SET_PRIORITY) r->priority=values[i];
        else values[i]=operation==INTEL_GPU_PROPERTY_GET_PRIORITY ? r->priority :
            r->buffer.live && r->buffer.validated ? 2u : 3u;
    }
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_create_retained(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint64_t bytes,uint32_t *resource)
{ return gpu_memory_create(m,pid,device,bytes,resource,true); }

enum intel_gpu_buffer_status intel_gpu_memory_residency(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,bool make,const uint64_t *cookies,uint32_t count)
{
    if (!MEMORY_VALID(m) || !cookies || !count || count>1024)
        return INTEL_GPU_BUFFER_INVALID;
    uint32_t occurrences[INTEL_GPU_MEMORY_RESOURCES]={0};
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d || d->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    for (uint32_t i=0;i<count;i++) {
        if (!cookies[i] || cookies[i]>UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,(uint32_t)cookies[i]);
        if (!r || r->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
        if (r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
        if (!r->buffer.live || !r->buffer.validated) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
        occurrences[r-m->resources]++;
    }
    /* Count duplicates before checking overflow: an invalid suffix must never
     * leave a resident prefix. All arithmetic and mutations share this lock. */
    if (make) for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++)
        if (occurrences[i]>UINT32_MAX-m->resources[i].residency_refs)
            MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NO_MEMORY);
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) if (occurrences[i]) {
        intel_gpu_memory_resource_t *r=&m->resources[i];
        if (make) r->residency_refs+=occurrences[i];
        else r->residency_refs=r->residency_refs>occurrences[i] ? r->residency_refs-occurrences[i] : 0;
    }
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_release_resource(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint32_t resource)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r || !r->closing || r->busy || !r->references) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    r->references--;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_acquire(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device, uint32_t resource, intel_gpu_memory_lease_t *lease)
{
    if (!MEMORY_VALID(m) || !lease || lease->self) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r || r->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    if (r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_pin(&r->buffer,pid);
    if (result==INTEL_GPU_BUFFER_OK)
        *lease=(intel_gpu_memory_lease_t){lease,m,&r->buffer,pid,device,resource};
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_release(intel_gpu_memory_lease_t *lease)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || &r->buffer!=lease->buffer) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_unpin(&r->buffer,lease->pid);
    if (result==INTEL_GPU_BUFFER_OK) memset(lease,0,sizeof(*lease));
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_acquire_resident(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint32_t resource,intel_gpu_memory_resident_lease_t *lease)
{
    if (!MEMORY_VALID(m) || !lease || lease->self) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r || r->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    if (!r->residency_refs || r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_reside(&r->buffer,pid);
    if (result==INTEL_GPU_BUFFER_OK)
        *lease=(intel_gpu_memory_resident_lease_t){.self=lease,.memory=m,.buffer=&r->buffer,
            .pid=pid,.device=device,.resource=resource};
    MEMORY_RETURN(m,flags,result);
}
enum intel_gpu_buffer_status intel_gpu_memory_release_resident(intel_gpu_memory_resident_lease_t *lease)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || &r->buffer!=lease->buffer || (lease->pending_work && !r->pending_work))
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_unreside(&r->buffer,lease->pid);
    if (result==INTEL_GPU_BUFFER_OK) {
        if (lease->pending_work) r->pending_work--;
        memset(lease,0,sizeof(*lease));
    }
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_reserve_work(intel_gpu_memory_resident_lease_t *lease)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory) || lease->pending_work)
        return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || &r->buffer!=lease->buffer || r->closing || m->closing)
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy || r->pending_work==UINT32_MAX) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    if (!r->residency_refs || r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
    r->pending_work++; lease->pending_work=true;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_acquire_cpu(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint32_t resource,intel_gpu_memory_cpu_lease_t *lease)
{
    if (!MEMORY_VALID(m) || !lease || lease->self) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r || r->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy || r->pending_work || r->cpu_access.self || r->cpu_holder)
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    if (r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
    r->busy=true;
    gpu_memory_unlock(&m->lock,flags);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_acquire_cpu(&r->buffer,pid,&r->cpu_access);
    flags=gpu_memory_lock(&m->lock);
    if (result==INTEL_GPU_BUFFER_OK && !r->closing && !m->closing) {
        *lease=(intel_gpu_memory_cpu_lease_t){lease,m,&r->buffer,pid,device,resource,
            r->cpu_access.cpu,r->cpu_access.bytes};
        r->cpu_holder=lease; r->busy=false;
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
    }
    /* Close/exit won cache preparation. No caller reference escapes; keep the
     * stable internal access in the resource if a concurrent buffer operation
     * prevents immediate rollback, so its collector can retry it. */
    gpu_memory_unlock(&m->lock,flags);
    if (result==INTEL_GPU_BUFFER_OK) {
        (void)intel_gpu_buffer_release_cpu(&r->cpu_access);
        result=INTEL_GPU_BUFFER_INVALID;
    }
    flags=gpu_memory_lock(&m->lock);
    r->busy=false;
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_release_cpu(intel_gpu_memory_cpu_lease_t *lease)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || r->cpu_holder!=lease || &r->buffer!=lease->buffer)
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    r->busy=true;
    gpu_memory_unlock(&m->lock,flags);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_release_cpu(&r->cpu_access);
    flags=gpu_memory_lock(&m->lock);
    if (result==INTEL_GPU_BUFFER_OK || result==INTEL_GPU_BUFFER_IO) {
        r->cpu_holder=NULL; memset(lease,0,sizeof(*lease));
    }
    r->busy=false;
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_acquire_coherent(intel_gpu_memory_t *m,
    uint32_t pid,uint32_t device,uint32_t resource,intel_gpu_memory_coherent_lease_t *lease)
{
    if (!MEMORY_VALID(m) || !lease || lease->self) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r || r->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy || r->coherent_access.self || r->coherent_holder)
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    if (r->offer_priority) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_NOT_RESIDENT);
    r->busy=true;
    gpu_memory_unlock(&m->lock,flags);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_acquire_coherent(&r->buffer,pid,&r->coherent_access);
    flags=gpu_memory_lock(&m->lock);
    if (result==INTEL_GPU_BUFFER_OK && !r->closing && !m->closing) {
        *lease=(intel_gpu_memory_coherent_lease_t){lease,m,&r->buffer,&r->coherent_access,pid,device,resource};
        r->coherent_holder=lease; r->busy=false;
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
    }
    gpu_memory_unlock(&m->lock,flags);
    if (result==INTEL_GPU_BUFFER_OK) {
        (void)intel_gpu_buffer_release_coherent(&r->coherent_access);
        result=INTEL_GPU_BUFFER_INVALID;
    }
    flags=gpu_memory_lock(&m->lock);
    r->busy=false;
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_retain_coherent(intel_gpu_memory_coherent_lease_t *lease,
    intel_gpu_coherent_reference_t *reference)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory) || !reference || reference->self)
        return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || r->coherent_holder!=lease || &r->buffer!=lease->buffer || &r->coherent_access!=lease->access ||
        r->closing || m->closing) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    /* Nonblocking buffer metadata only; no allocator/cache operation here. */
    enum intel_gpu_buffer_status result=intel_gpu_buffer_retain_coherent(&r->coherent_access,reference);
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_release_coherent(intel_gpu_memory_coherent_lease_t *lease)
{
    if (!lease || lease->self!=lease || !MEMORY_VALID(lease->memory)) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_memory_t *m=lease->memory;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,lease->pid,lease->device,lease->resource);
    if (!r || r->coherent_holder!=lease || &r->buffer!=lease->buffer || &r->coherent_access!=lease->access)
        MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    if (r->busy) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_BUSY);
    r->busy=true;
    gpu_memory_unlock(&m->lock,flags);
    enum intel_gpu_buffer_status result=intel_gpu_buffer_release_coherent(&r->coherent_access);
    flags=gpu_memory_lock(&m->lock);
    if (result==INTEL_GPU_BUFFER_OK || result==INTEL_GPU_BUFFER_IO) {
        r->coherent_holder=NULL; memset(lease,0,sizeof(*lease));
    }
    r->busy=false;
    MEMORY_RETURN(m,flags,result);
}

enum intel_gpu_buffer_status intel_gpu_memory_close(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device, uint32_t resource)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=gpu_memory_resource(m,pid,device,resource);
    if (!r) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    r->closing=true;
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_close_device(intel_gpu_memory_t *m,
    uint32_t pid, uint32_t device)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_device_t *d=gpu_memory_device(m,pid,device);
    if (!d) MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_INVALID);
    gpu_memory_close_device(m,d);
    MEMORY_RETURN(m,flags,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_memory_collect(intel_gpu_memory_t *m)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t flags=gpu_memory_lock(&m->lock);
    intel_gpu_memory_resource_t *r=NULL;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
        unsigned slot=(m->cursor+i)%INTEL_GPU_MEMORY_RESOURCES;
        if (gpu_memory_collectable(&m->resources[slot])) {
            r=&m->resources[slot]; r->busy=true; m->cursor=(slot+1)%INTEL_GPU_MEMORY_RESOURCES;
            break;
        }
    }
    if (!r) {
        bool retained=gpu_memory_reap_devices(m);
        MEMORY_RETURN(m,flags,m->pages || retained ? INTEL_GPU_BUFFER_BUSY : INTEL_GPU_BUFFER_OK);
    }
    gpu_memory_unlock(&m->lock,flags);
    enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_OK;
    if (r->cpu_access.self) result=intel_gpu_buffer_release_cpu(&r->cpu_access);
    if ((result==INTEL_GPU_BUFFER_OK || result==INTEL_GPU_BUFFER_IO) && r->coherent_access.self)
        result=intel_gpu_buffer_release_coherent(&r->coherent_access);
    if (result==INTEL_GPU_BUFFER_OK || result==INTEL_GPU_BUFFER_IO)
        result=intel_gpu_buffer_close(&r->buffer,r->pid);
    flags=gpu_memory_lock(&m->lock);
    if (result==INTEL_GPU_BUFFER_OK) {
        intel_gpu_memory_device_t *d=gpu_memory_device(m,r->pid,r->device);
        d->pages-=r->pages; d->allocations--; m->pages-=r->pages;
        memset(r,0,sizeof(*r));
    } else r->busy=false;
    gpu_memory_reap_devices(m);
    MEMORY_RETURN(m,flags,result);
}

unsigned intel_gpu_memory_collect_budget(intel_gpu_memory_t *m)
{
    if (!MEMORY_VALID(m)) return 0;
    uint64_t flags=gpu_memory_lock(&m->lock);
    unsigned count=0;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_RESOURCES;i++) {
        if (gpu_memory_collectable(&m->resources[i])) count++;
    }
    gpu_memory_unlock(&m->lock,flags);
    return count;
}

void intel_gpu_memory_shutdown(intel_gpu_memory_t *m)
{
    if (!MEMORY_VALID(m)) return;
    uint64_t flags=gpu_memory_lock(&m->lock);
    m->closing=true;
    for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++)
        if (m->devices[i].id) gpu_memory_close_device(m,&m->devices[i]);
    gpu_memory_reap_devices(m);
    gpu_memory_unlock(&m->lock,flags);
}

enum intel_gpu_buffer_status intel_gpu_memory_publish(intel_gpu_memory_t *m)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t registry_flags=gpu_memory_lock(&memory_registry_lock);
    uint64_t flags=gpu_memory_lock(&m->lock);
    enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_INVALID;
    if (!m->closing && !m->registered) {
        result=INTEL_GPU_BUFFER_BUSY;
        for (unsigned i=0;i<8;i++) if (!memory_registry[i]) {
            memory_registry[i]=m; m->registered=true; result=INTEL_GPU_BUFFER_OK; break;
        }
    }
    gpu_memory_unlock(&m->lock,flags); gpu_memory_unlock(&memory_registry_lock,registry_flags);
    return result;
}

enum intel_gpu_buffer_status intel_gpu_memory_unpublish(intel_gpu_memory_t *m)
{
    if (!MEMORY_VALID(m)) return INTEL_GPU_BUFFER_INVALID;
    uint64_t registry_flags=gpu_memory_lock(&memory_registry_lock);
    uint64_t flags=gpu_memory_lock(&m->lock);
    enum intel_gpu_buffer_status result=INTEL_GPU_BUFFER_INVALID;
    if (m->registered) {
        result=INTEL_GPU_BUFFER_BUSY;
        bool devices=false;
        for (unsigned i=0;i<INTEL_GPU_MEMORY_DEVICES;i++) devices |= m->devices[i].id!=0;
        if (m->closing && !m->pages && !devices) {
            for (unsigned i=0;i<8;i++) if (memory_registry[i]==m) memory_registry[i]=NULL;
            m->registered=false; result=INTEL_GPU_BUFFER_OK;
        }
    }
    gpu_memory_unlock(&m->lock,flags); gpu_memory_unlock(&memory_registry_lock,registry_flags);
    return result;
}

void intel_gpu_memory_release_process(uint32_t pid)
{
    if (!pid) return;
    uint64_t registry_flags=gpu_memory_lock(&memory_registry_lock);
    for (unsigned i=0;i<8;i++) if (memory_registry[i]) {
        intel_gpu_memory_t *m=memory_registry[i];
        uint64_t flags=gpu_memory_lock(&m->lock);
        for (unsigned d=0;d<INTEL_GPU_MEMORY_DEVICES;d++)
            if (m->devices[d].pid==pid) {
                m->devices[d].exiting=true;
                gpu_memory_close_device(m,&m->devices[d]);
            }
        gpu_memory_unlock(&m->lock,flags);
    }
    gpu_memory_unlock(&memory_registry_lock,registry_flags);
}
