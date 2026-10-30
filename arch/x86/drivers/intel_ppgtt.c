#include "intel_ppgtt.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

bool intel_ppgtt_build(uint64_t *arena, size_t bytes, uint64_t dma,
    const intel_ppgtt_page_t *pages, uint32_t count, uint32_t *tables_used)
{
    uintptr_t a = (uintptr_t)arena, p = (uintptr_t)pages, u = (uintptr_t)tables_used;
    uint64_t input_bytes = (uint64_t)count * sizeof(*pages);
    if (!arena || (a & 7) || !tables_used || (u & 3) ||
        !bytes || (bytes & 4095) || bytes >= INTEL_GGTT_PHYS_LIMIT ||
        !dma || (dma & 4095) || dma > INTEL_GGTT_PHYS_LIMIT - bytes ||
        count > INTEL_PPGTT_MAX_MAPPINGS || (count && (!pages || (p & 7))) ||
        a > (uintptr_t)-1 - bytes || p > (uintptr_t)-1 - input_bytes || u > (uintptr_t)-1 - 4 ||
        (count && a < p + input_bytes && p < a + bytes) ||
        (a < u + 4 && u < a + bytes) ||
        (count && p < u + 4 && u < p + input_bytes)) return false;
    uint64_t previous = UINT64_MAX;
    uint32_t needed = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t va = pages[i].gpu, pa = pages[i].dma;
        if (!va || (va & 4095) || va >= INTEL_PPGTT_VA_LIMIT ||
            !pa || (pa & 4095) || pa >= INTEL_GGTT_PHYS_LIMIT ||
            (pa >= dma && pa < dma + bytes) || (i && va <= previous)) return false;
        for (unsigned shift = 21; shift <= 39; shift += 9)
            if (!i || (va >> shift) != (previous >> shift)) needed++;
        previous = va;
    }
    if (needed > bytes / 4096) return false;
    memset(arena, 0, bytes);
    uint32_t used = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t va = pages[i].gpu;
        uint64_t *table = arena;
        for (unsigned shift = 39; shift >= 21; shift -= 9) {
            uint64_t *entry = table + ((va >> shift) & 511);
            if (!*entry) *entry = (dma + (uint64_t)used++ * 4096) | 3;
            table = arena + ((*entry & ~4095ULL) - dma) / sizeof(*arena);
        }
        table[(va >> 12) & 511] = pages[i].dma | 0x19 | (pages[i].writable ? 2 : 0);
    }
    *tables_used = used;
    return true;
}

bool intel_ppgtt_range_pages(const intel_ppgtt_range_t *ranges, uint32_t count,
    uint32_t *table_pages)
{
    if (!table_pages || count>INTEL_PPGTT_MAX_RANGES || (count && !ranges)) return false;
    uint64_t previous=0, pages=0;
    uint32_t needed=1;
    for (uint32_t i=0;i<count;i++) {
        const intel_ppgtt_range_t *r=&ranges[i];
        if (!r->gpu || !r->dma || !r->bytes || ((r->gpu|r->dma|r->bytes)&4095) ||
            r->bytes>(uint64_t)INTEL_PPGTT_MAX_MAPPINGS*4096 || r->gpu>INTEL_PPGTT_VA_LIMIT-r->bytes ||
            r->dma>INTEL_GGTT_PHYS_LIMIT-r->bytes || (i && r->gpu<=previous)) return false;
        pages+=r->bytes/4096;
        if (pages>INTEL_PPGTT_MAX_MAPPINGS) return false;
        uint64_t last=r->gpu+r->bytes-4096;
        for (unsigned shift=21;shift<=39;shift+=9) {
            needed+=(uint32_t)((last>>shift)-(r->gpu>>shift));
            if (!i || (r->gpu>>shift)!=(previous>>shift)) needed++;
        }
        previous=last;
    }
    *table_pages=needed;
    return true;
}

bool intel_ppgtt_build_ranges(uint64_t *arena, size_t bytes, uint64_t dma,
    const intel_ppgtt_range_t *ranges, uint32_t count, uint32_t *tables_used)
{
    return intel_ppgtt_build_ranges_coherent(arena,bytes,dma,ranges,count,0,tables_used);
}

bool intel_ppgtt_build_ranges_coherent(uint64_t *arena,size_t bytes,uint64_t dma,
    const intel_ppgtt_range_t *ranges,uint32_t count,uint64_t coherent_mask,uint32_t *tables_used)
{
    if (count>INTEL_PPGTT_MAX_RANGES || (count<64 && (coherent_mask>>count))) return false;
    uintptr_t a=(uintptr_t)arena, r=(uintptr_t)ranges, u=(uintptr_t)tables_used;
    uint64_t range_bytes=(uint64_t)count*sizeof(*ranges);
    uint32_t needed;
    if (!arena || (a&7) || !tables_used || (u&3) || (count && (r&7)) ||
        !bytes || (bytes&4095) || bytes>=INTEL_GGTT_PHYS_LIMIT ||
        !dma || (dma&4095) || dma>INTEL_GGTT_PHYS_LIMIT-bytes ||
        a>(uintptr_t)-1-bytes || r>(uintptr_t)-1-range_bytes || u>(uintptr_t)-1-4 ||
        (count && a<r+range_bytes && r<a+bytes) || (a<u+4 && u<a+bytes) ||
        (count && r<u+4 && u<r+range_bytes) ||
        !intel_ppgtt_range_pages(ranges,count,&needed) || needed>bytes/4096) return false;
    for (uint32_t i=0;i<count;i++)
        if (ranges[i].dma<dma+bytes && dma<ranges[i].dma+ranges[i].bytes) return false;
    for (uint32_t i=0;i<count;i++) for (uint32_t j=0;j<i;j++)
        if (((coherent_mask>>i)^(coherent_mask>>j))&1u)
            if (ranges[i].dma<ranges[j].dma+ranges[j].bytes && ranges[j].dma<ranges[i].dma+ranges[i].bytes)
                return false;
    memset(arena,0,bytes);
    uint32_t used=1;
    for (uint32_t i=0;i<count;i++) for (uint64_t off=0;off<ranges[i].bytes;off+=4096) {
        uint64_t va=ranges[i].gpu+off, *table=arena;
        for (unsigned shift=39;shift>=21;shift-=9) {
            uint64_t *entry=table+((va>>shift)&511);
            if (!*entry) *entry=(dma+(uint64_t)used++*4096)|3;
            table=arena+((*entry&~4095ULL)-dma)/sizeof(*arena);
        }
        table[(va>>12)&511]=(ranges[i].dma+off)|((coherent_mask&(1ULL<<i))?0x81:0x19)|(ranges[i].writable?2:0);
    }
    *tables_used=used;
    return true;
}

bool intel_ppgtt_pat_acquire(const intel_gt_io_t *io, void *ctx, intel_ppgtt_pat_t *s)
{
    if (!io || !io->read || !io->write || !s || s->started) return false;
    s->started = true;
    if (!io->read(ctx, 0x40e0, &s->original) || s->original == UINT32_MAX) return false;
    uint32_t desired = s->original & 0x00ffffff;
    s->last_value = s->original;
    if (desired != s->original) {
        s->changed = true; /* before the first possibly applied MMIO write */
        if (!io->write(ctx, 0x40e0, desired) ||
            !io->read(ctx, 0x40e0, &s->last_value) || s->last_value != desired) return false;
    }
    s->ready = true;
    return true;
}

bool intel_ppgtt_pat_restore(const intel_gt_io_t *io, void *ctx, intel_ppgtt_pat_t *s)
{
    if (!io || !io->read || !io->write || !s || !s->started) return false;
    s->ready = false;
    if (!s->changed) return true;
    if (!io->read(ctx, 0x40e0, &s->last_value) || s->last_value == UINT32_MAX) return false;
    uint32_t entry = s->last_value & 0xff000000, original = s->original & 0xff000000;
    if (entry == original) { s->changed = false; return true; }
    if (entry) return false; /* slot 3 changed externally; do not overwrite it */
    uint32_t restore = (s->last_value & 0x00ffffff) | original;
    if (!io->write(ctx, 0x40e0, restore) ||
        !io->read(ctx, 0x40e0, &s->last_value) || s->last_value != restore) return false;
    s->changed = false;
    return true;
}

bool intel_ppgtt_coherent_pat_acquire(const intel_gt_io_t *io,void *ctx,intel_ppgtt_pat_t *s)
{
    if (!io || !io->read || !io->write || !s || s->started) return false;
    s->started=true;
    if (!io->read(ctx,0x40e4,&s->original) || s->original==UINT32_MAX) return false;
    uint32_t desired=(s->original&0xffffff00u)|0x07u; /* PAT4: WB, LLC */
    s->last_value=s->original;
    if (desired!=s->original) {
        s->changed=true;
        if (!io->write(ctx,0x40e4,desired) || !io->read(ctx,0x40e4,&s->last_value) || s->last_value!=desired)
            return false;
    }
    s->ready=true; return true;
}
bool intel_ppgtt_coherent_pat_restore(const intel_gt_io_t *io,void *ctx,intel_ppgtt_pat_t *s)
{
    if (!io || !io->read || !io->write || !s || !s->started) return false;
    s->ready=false;
    if (!s->changed) return true;
    if (!io->read(ctx,0x40e4,&s->last_value) || s->last_value==UINT32_MAX) return false;
    uint32_t current=s->last_value&0xffu,original=s->original&0xffu;
    if (current==original) { s->changed=false; return true; }
    if (current!=0x07u) return false;
    uint32_t restore=(s->last_value&0xffffff00u)|original;
    if (!io->write(ctx,0x40e4,restore) || !io->read(ctx,0x40e4,&s->last_value) || s->last_value!=restore)
        return false;
    s->changed=false; return true;
}
