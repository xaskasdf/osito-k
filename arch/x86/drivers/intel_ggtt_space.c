#include "intel_ggtt_space.h"

static bool space_enter(intel_ggtt_space_t *s)
{
    return !__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQUIRE);
}

static void space_leave(intel_ggtt_space_t *s)
{
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
}

static bool space_used(const intel_ggtt_space_t *s, uint32_t page)
{
    uint32_t i = page - s->first;
    return (s->used[i / 8] & (1u << (i & 7))) != 0;
}

static void space_mark(intel_ggtt_space_t *s, uint32_t page, bool used)
{
    uint32_t i = page - s->first;
    uint8_t bit = (uint8_t)(1u << (i & 7));
    if (used) s->used[i / 8] |= bit;
    else s->used[i / 8] &= (uint8_t)~bit;
}

enum intel_ggtt_bind_status intel_ggtt_space_init(intel_ggtt_space_t *s,
    const intel_ggtt_layout_t *v, uint32_t first, uint32_t count,
    uint8_t *bitmap, size_t bitmap_bytes, const intel_ggtt_io_t *io, void *ctx)
{
    if (!s || !v || !bitmap || !io || !io->read || !io->write || !io->sync ||
        !first || !count || bitmap_bytes < ((uint64_t)count + 7) / 8 ||
        (v->ggtt_bytes != 2ULL * 1024 * 1024 &&
         v->ggtt_bytes != 4ULL * 1024 * 1024 &&
         v->ggtt_bytes != 8ULL * 1024 * 1024) ||
        v->gpu_va_bytes != v->ggtt_bytes / 8 * 4096 ||
        first >= v->ggtt_bytes / 8 - 1 || count > v->ggtt_bytes / 8 - 1 - first)
        return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    if (s->ready) { space_leave(s); return INTEL_GGTT_BIND_BUSY; }
    s->first = first;
    s->count = count;
    s->used = bitmap;
    s->io = *io;
    s->context = ctx;
    s->next_cookie = 1;
    for (uint32_t i = 0; i < (count + 7) / 8; i++) bitmap[i] = 255;
    for (uint32_t i = first; i < first + count; i++) {
        uint64_t pte;
        if (!s->io.read(ctx, i, &pte) || pte == UINT64_MAX) {
            space_leave(s);
            return INTEL_GGTT_BIND_IO_ERROR;
        }
        space_mark(s, i, (pte & 1) != 0);
    }
    s->ready = true;
    space_leave(s);
    return INTEL_GGTT_BIND_OK;
}

enum intel_ggtt_bind_status intel_ggtt_space_capacity(intel_ggtt_space_t *s,
    uint32_t block_pages,uint32_t spare_pages,uint64_t max_address,uint32_t *blocks)
{
    if (!s || !block_pages || !blocks || (max_address&4095)) return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    if (!s->ready) { space_leave(s); return INTEL_GGTT_BIND_INVALID; }
    uint64_t end=(uint64_t)s->first+s->count;
    if (max_address && (max_address>>12)<end) end=max_address>>12;
    uint32_t run=0,free_pages=0,count=0;
    for (uint32_t page=s->first;page<end;page++) {
        if (space_used(s,page)) run=0;
        else {
            free_pages++;
            if (++run==block_pages) { count++; run=0; }
        }
    }
    uint32_t budget=free_pages>spare_pages ? (free_pages-spare_pages)/block_pages : 0;
    *blocks=count<budget ? count : budget;
    space_leave(s);
    return INTEL_GGTT_BIND_OK;
}

static bool binding_matches(const intel_ggtt_space_t *s,
                             const intel_ggtt_binding_t *b, uint64_t cookie)
{
    return s->ready && b && b->self == b && b->space == s && cookie &&
           b->cookie == cookie && b->phase != INTEL_GGTT_EMPTY;
}

/* Even a failed write may have reached the device. Restore the entire range,
 * synchronize, then read back every entry before releasing any reservation. */
static bool binding_restore(intel_ggtt_space_t *s, intel_ggtt_binding_t *b)
{
    bool ok = true;
    for (uint32_t i = 0; i < b->count; i++)
        if (!s->io.write(s->context, b->first + i, b->originals[i])) ok = false;
    if (!s->io.sync(s->context)) ok = false;
    for (uint32_t i = 0; i < b->count; i++) {
        uint64_t pte;
        if (!s->io.read(s->context, b->first + i, &pte) || pte != b->originals[i])
            ok = false;
    }
    if (!ok) { b->phase = INTEL_GGTT_QUARANTINED; return false; }
    for (uint32_t i = 0; i < b->count; i++) space_mark(s, b->first + i, false);
    *b = (intel_ggtt_binding_t){0};
    return true;
}

enum intel_ggtt_bind_status intel_ggtt_bind(intel_ggtt_space_t *s,
    intel_ggtt_binding_t *b, const uint64_t *dma, uint32_t count,
    uint32_t alignment, uint64_t max_address, uint64_t *originals)
{
    if (!s || !b || !dma || !originals || dma == originals || !count ||
        !alignment || (alignment & (alignment - 1)) || (max_address & 4095))
        return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    enum intel_ggtt_bind_status result = INTEL_GGTT_BIND_INVALID;
    if (!s->ready || b->phase != INTEL_GGTT_EMPTY || count > s->count ||
        alignment > s->count || s->next_cookie == UINT64_MAX) goto out;
    uintptr_t src = (uintptr_t)dma, dst = (uintptr_t)originals;
    uint64_t array_bytes = (uint64_t)count * sizeof(uint64_t);
    /* Capturing old PTEs must not overwrite DMA addresses after validation.
     * Subtraction avoids overflowing an end pointer in the overlap check. */
    if ((src >= dst ? src - dst : dst - src) < array_bytes) goto out;
    for (uint32_t i = 0; i < count; i++)
        if ((dma[i] & 4095) || dma[i] >= INTEL_GGTT_PHYS_LIMIT) goto out;
    uint64_t end = (uint64_t)s->first + s->count;
    if (max_address && (max_address >> 12) < end) end = max_address >> 12;
    result = INTEL_GGTT_BIND_NO_SPACE;
    uint64_t page = ((uint64_t)s->first + alignment - 1) & ~((uint64_t)alignment - 1);
    for (; page < end && count <= end - page;) {
        uint32_t i;
        for (i = 0; i < count && !space_used(s, (uint32_t)page + i); i++) {}
        if (i == count) break;
        page = (page + i + 1 + alignment - 1) & ~((uint64_t)alignment - 1);
    }
    if (page >= end || count > end - page) goto out;

    /* Recheck the complete range before any write. A new firmware/external
     * mapping is preserved, made unavailable and reported to the caller. */
    result = INTEL_GGTT_BIND_IO_ERROR;
    for (uint32_t i = 0; i < count; i++) {
        if (!s->io.read(s->context, (uint32_t)page + i, &originals[i]) ||
            originals[i] == UINT64_MAX) goto out;
        if (originals[i] & 1) {
            space_mark(s, (uint32_t)page + i, true);
            result = INTEL_GGTT_BIND_CHANGED;
            goto out;
        }
    }
    *b = (intel_ggtt_binding_t){.self = b, .space = s,
        .cookie = s->next_cookie++, .gpu_address = page * 4096,
        .originals = originals, .first = (uint32_t)page, .count = count,
        .phase = INTEL_GGTT_QUARANTINED};
    for (uint32_t i = 0; i < count; i++) space_mark(s, b->first + i, true);
    for (uint32_t i = 0; i < count; i++)
        if (!s->io.write(s->context, b->first + i, dma[i] | 1)) goto rollback;
    if (!s->io.sync(s->context)) goto rollback;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t pte;
        if (!s->io.read(s->context, b->first + i, &pte) || pte != (dma[i] | 1))
            goto rollback;
    }
    b->phase = INTEL_GGTT_BOUND;
    result = INTEL_GGTT_BIND_OK;
    goto out;
rollback:
    result = binding_restore(s, b) ? INTEL_GGTT_BIND_IO_ERROR :
                                    INTEL_GGTT_BIND_QUARANTINED;
out:
    space_leave(s);
    return result;
}

enum intel_ggtt_bind_status intel_ggtt_unbind(intel_ggtt_space_t *s,
    intel_ggtt_binding_t *b, uint64_t cookie)
{
    if (!s || !b) return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    enum intel_ggtt_bind_status result = INTEL_GGTT_BIND_INVALID;
    if (!binding_matches(s, b, cookie)) goto out;
    if (b->pins) { result = INTEL_GGTT_BIND_BUSY; goto out; }
    result = binding_restore(s, b) ? INTEL_GGTT_BIND_OK : INTEL_GGTT_BIND_QUARANTINED;
out:
    space_leave(s);
    return result;
}

enum intel_ggtt_bind_status intel_ggtt_pin(intel_ggtt_space_t *s,
    intel_ggtt_binding_t *b, uint64_t cookie)
{
    if (!s || !b) return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    enum intel_ggtt_bind_status result = INTEL_GGTT_BIND_INVALID;
    if (binding_matches(s, b, cookie) && b->phase == INTEL_GGTT_BOUND && b->pins != UINT32_MAX) {
        b->pins++;
        result = INTEL_GGTT_BIND_OK;
    }
    space_leave(s);
    return result;
}

enum intel_ggtt_bind_status intel_ggtt_unpin(intel_ggtt_space_t *s,
    intel_ggtt_binding_t *b, uint64_t cookie)
{
    if (!s || !b) return INTEL_GGTT_BIND_INVALID;
    if (!space_enter(s)) return INTEL_GGTT_BIND_BUSY;
    enum intel_ggtt_bind_status result = INTEL_GGTT_BIND_INVALID;
    if (binding_matches(s, b, cookie) && b->phase == INTEL_GGTT_BOUND && b->pins) {
        b->pins--;
        result = INTEL_GGTT_BIND_OK;
    }
    space_leave(s);
    return result;
}
