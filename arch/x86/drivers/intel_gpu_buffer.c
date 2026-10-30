#include "intel_gpu_buffer.h"
#ifndef __KERNEL_X86__
#include <string.h>
#endif
static bool gpu_buffer_enter(intel_gpu_buffer_t *b)
{ return b && !__atomic_exchange_n(&b->busy,1,__ATOMIC_ACQUIRE); }
static enum intel_gpu_buffer_status gpu_buffer_leave(intel_gpu_buffer_t *b,
    enum intel_gpu_buffer_status result)
{ __atomic_store_n(&b->busy,0,__ATOMIC_RELEASE); return result; }
static bool gpu_buffer_valid(intel_gpu_buffer_t *b, uint64_t owner)
{ return b->self==b && b->started && owner && b->owner==owner; }

/* Called with the nonblocking buffer gate held. Allocator callbacks execute
 * without an IRQ-safe metadata lock, and may reenter only to receive BUSY. */
static enum intel_gpu_buffer_status gpu_buffer_allocate_backing(intel_gpu_buffer_t *b)
{
    const intel_gpu_allocator_t *a=&b->allocator;
    void *ctx=b->allocator_context;
    if (!a->allocate(ctx,b->pages,&b->dma,&b->cpu)) return INTEL_GPU_BUFFER_NO_MEMORY;
    b->live=true;
    uint64_t rounded=(uint64_t)b->pages*4096;
    if (!b->dma || (b->dma&4095) || b->dma>INTEL_GGTT_PHYS_LIMIT-rounded ||
        !b->cpu || ((uintptr_t)b->cpu&4095) || (uintptr_t)b->cpu>(uintptr_t)-1-rounded ||
        !a->validate(ctx,b->dma,b->cpu,b->pages))
        return INTEL_GPU_BUFFER_QUARANTINED;
    b->validated=true;
    memset(b->cpu,0,(size_t)rounded);
    b->ready=a->sync(ctx,b->dma,b->cpu,b->pages);
    return b->ready ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_IO;
}

enum intel_gpu_buffer_status intel_gpu_buffer_create(intel_gpu_buffer_t *b,
    const intel_gpu_allocator_t *a, void *ctx, uint64_t owner, uint64_t bytes)
{
    if (!b || !a || !a->allocate || !a->validate || !a->release || !a->sync ||
        !owner || !bytes || bytes>INTEL_GPU_BUFFER_MAX_BYTES) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (b->started) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    b->self=b; b->started=true; b->owner=owner; b->bytes=bytes;
    b->pages=(uint32_t)((bytes+4095)/4096); b->allocator=*a; b->allocator_context=ctx;
    return gpu_buffer_leave(b,gpu_buffer_allocate_backing(b));
}

enum intel_gpu_buffer_status intel_gpu_buffer_discard(intel_gpu_buffer_t *b,uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || b->closing)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->pins || b->residents || b->cpu_access || b->gpu_access || b->coherent_access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    if (!b->live) return gpu_buffer_leave(b,
        b->discarded ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_INVALID);
    if (!b->validated) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_QUARANTINED);
    if (!b->allocator.release(b->allocator_context,b->dma,b->cpu,b->pages))
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    b->live=b->ready=b->validated=false;
    b->cpu=NULL; b->dma=0; b->discarded=true;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_recommit(intel_gpu_buffer_t *b,uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || b->closing || !b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->pins || b->residents || b->cpu_access || b->gpu_access || b->coherent_access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    enum intel_gpu_buffer_status result;
    if (b->live) {
        if (!b->validated) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_QUARANTINED);
        /* Retry cache preparation without leaking/replacing already owned RAM. */
        b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
        result=b->ready ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_IO;
    } else result=gpu_buffer_allocate_backing(b);
    if (result==INTEL_GPU_BUFFER_OK) b->discarded=false;
    return gpu_buffer_leave(b,result);
}

enum intel_gpu_buffer_status intel_gpu_buffer_pin(intel_gpu_buffer_t *b, uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->ready || b->closing || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->cpu_access || b->gpu_access || b->coherent_access || b->pins==UINT32_MAX) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->pins++;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_unpin(intel_gpu_buffer_t *b, uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->pins)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    b->pins--;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_close(intel_gpu_buffer_t *b, uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner)) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    b->closing=true;
    if (b->pins || b->residents || b->cpu_access || b->gpu_access || b->coherent_access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    if (!b->live) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
    if (!b->validated) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_QUARANTINED);
    if (!b->allocator.release(b->allocator_context,b->dma,b->cpu,b->pages))
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    b->live=false; b->cpu=NULL; b->dma=0;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_sync(intel_gpu_buffer_t *b, uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->validated || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->cpu_access || b->coherent_access) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    return gpu_buffer_leave(b,b->ready ?
        INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_IO);
}

enum intel_gpu_buffer_status intel_gpu_buffer_acquire_cpu(intel_gpu_buffer_t *b,
    uint64_t owner,intel_gpu_cpu_access_t *access)
{
    if (!b || !access || access->self) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->validated || b->closing || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->pins || b->cpu_access || b->gpu_access || b->coherent_access) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    if (!b->ready) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    *access=(intel_gpu_cpu_access_t){access,b,owner,b->cpu,b->bytes};
    b->cpu_access=access;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_release_cpu(intel_gpu_cpu_access_t *access)
{
    if (!access || access->self!=access || !access->buffer) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || b->cpu_access!=access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    /* All CPU views are already revoked by the owner. Even if flushing fails,
     * dropping access is safe: the buffer stays unready and cannot be pinned. */
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    b->cpu_access=NULL; memset(access,0,sizeof(*access));
    return gpu_buffer_leave(b,b->ready ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_IO);
}

enum intel_gpu_buffer_status intel_gpu_buffer_cpu_physical(intel_gpu_cpu_access_t *access,
    uint64_t *physical,uint64_t *rounded_bytes)
{
    if (!access || access->self!=access || !access->buffer || !physical || !rounded_bytes)
        return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || !b->validated || b->cpu_access!=access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (!b->allocator.cpu_physical) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_UNSUPPORTED);
    uint64_t pa=0,bytes=(uint64_t)b->pages*4096;
    if (!b->allocator.cpu_physical(b->allocator_context,b->dma,b->cpu,b->pages,&pa) ||
        !pa || (pa&4095) || pa>0x000ffffffffff000ULL ||
        bytes-4096>0x000ffffffffff000ULL-pa)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    *physical=pa; *rounded_bytes=bytes;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_reside(intel_gpu_buffer_t *b,uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->validated || b->closing || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->coherent_access && b->coherent_access->closing)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->residents==UINT32_MAX) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->residents++;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_unreside(intel_gpu_buffer_t *b,uint64_t owner)
{
    if (!b) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->residents)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    b->residents--;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_acquire_gpu(intel_gpu_buffer_t *b,
    uint64_t owner,intel_gpu_access_t *access)
{
    if (!b || !access || access->self) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->validated || !b->ready || b->closing || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->pins || b->cpu_access || b->gpu_access || b->coherent_access) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    if (!b->ready) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    *access=(intel_gpu_access_t){access,b,owner}; b->gpu_access=access;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_release_gpu(intel_gpu_access_t *access)
{
    if (!access || access->self!=access || !access->buffer) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || b->gpu_access!=access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    b->gpu_access=NULL; memset(access,0,sizeof(*access));
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_acquire_coherent(intel_gpu_buffer_t *b,
    uint64_t owner,intel_gpu_coherent_access_t *access)
{
    if (!b || !access || access->self) return INTEL_GPU_BUFFER_INVALID;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,owner) || !b->live || !b->validated || b->closing || b->discarded)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (b->pins || b->residents || b->cpu_access || b->gpu_access || b->coherent_access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    if (!b->allocator.cpu_physical) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_UNSUPPORTED);
    uint64_t physical=0,bytes=(uint64_t)b->pages*4096;
    if (!b->allocator.cpu_physical(b->allocator_context,b->dma,b->cpu,b->pages,&physical) ||
        !physical || (physical&4095) || physical>0x000ffffffffff000ULL ||
        bytes-4096>0x000ffffffffff000ULL-physical)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    if (!b->ready) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_IO);
    *access=(intel_gpu_coherent_access_t){.self=access,.buffer=b,.cpu=b->cpu,
        .owner=owner,.physical=physical,.dma=b->dma,.bytes=bytes};
    b->coherent_access=access;
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_retain_coherent(intel_gpu_coherent_access_t *access,
    intel_gpu_coherent_reference_t *reference)
{
    if (!access || access->self!=access || !access->buffer || !reference || reference->self)
        return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || b->coherent_access!=access ||
        b->closing || access->closing) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    if (access->references==UINT32_MAX) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    access->references++;
    *reference=(intel_gpu_coherent_reference_t){reference,access};
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_drop_coherent(intel_gpu_coherent_reference_t *reference)
{
    if (!reference || reference->self!=reference || !reference->access)
        return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_coherent_access_t *access=reference->access;
    if (access->self!=access || !access->buffer) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || b->coherent_access!=access || !access->references)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    access->references--; memset(reference,0,sizeof(*reference));
    return gpu_buffer_leave(b,INTEL_GPU_BUFFER_OK);
}

enum intel_gpu_buffer_status intel_gpu_buffer_release_coherent(intel_gpu_coherent_access_t *access)
{
    if (!access || access->self!=access || !access->buffer) return INTEL_GPU_BUFFER_INVALID;
    intel_gpu_buffer_t *b=access->buffer;
    if (!gpu_buffer_enter(b)) return INTEL_GPU_BUFFER_BUSY;
    if (!gpu_buffer_valid(b,access->owner) || !b->live || b->coherent_access!=access)
        return gpu_buffer_leave(b,INTEL_GPU_BUFFER_INVALID);
    access->closing=true;
    if (access->references || b->residents) return gpu_buffer_leave(b,INTEL_GPU_BUFFER_BUSY);
    b->ready=b->allocator.sync(b->allocator_context,b->dma,b->cpu,b->pages);
    b->coherent_access=NULL; memset(access,0,sizeof(*access));
    return gpu_buffer_leave(b,b->ready ? INTEL_GPU_BUFFER_OK : INTEL_GPU_BUFFER_IO);
}
