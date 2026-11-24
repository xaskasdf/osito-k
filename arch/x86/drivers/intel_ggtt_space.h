/* Native Gen9 GGTT mapping ownership. Internal driver API, not a Win32 ABI. */
#ifndef OSITOK_INTEL_GGTT_SPACE_H
#define OSITOK_INTEL_GGTT_SPACE_H
#include "intel_ggtt.h"
#ifndef __KERNEL_X86__
#include <stddef.h>
#endif

typedef struct {
    bool (*read)(void *, uint32_t page, uint64_t *pte);
    bool (*write)(void *, uint32_t page, uint64_t pte);
    /* Complete prior PTE writes and required translation invalidation.
     * Success must mean hardware completion, never just command submission. */
    bool (*sync)(void *);
} intel_ggtt_io_t;

typedef struct {
    unsigned busy;
    bool ready;
    uint32_t first, count;
    uint64_t next_cookie;
    uint8_t *used;
    intel_ggtt_io_t io;
    void *context;
} intel_ggtt_space_t;

enum intel_ggtt_binding_phase { INTEL_GGTT_EMPTY, INTEL_GGTT_BOUND,
                               INTEL_GGTT_QUARANTINED };
typedef struct intel_ggtt_binding {
    /* All fields are driver-private. Keep this object and originals alive
     * until unbind succeeds; copying/moving a live object is unsupported. */
    struct intel_ggtt_binding *self;
    intel_ggtt_space_t *space;
    uint64_t cookie, gpu_address;
    uint64_t *originals;
    uint32_t first, count, pins;
    enum intel_ggtt_binding_phase phase;
} intel_ggtt_binding_t;

enum intel_ggtt_bind_status {
    INTEL_GGTT_BIND_OK,
    INTEL_GGTT_BIND_INVALID,
    INTEL_GGTT_BIND_BUSY,
    INTEL_GGTT_BIND_NO_SPACE,
    INTEL_GGTT_BIND_IO_ERROR,
    INTEL_GGTT_BIND_CHANGED,
    INTEL_GGTT_BIND_QUARANTINED
};

/* Caller must first own the adapter/address window, exclude every firmware
 * reservation (including fences/contexts), and establish its DMA domain.
 * Discovering a non-present PTE alone does NOT establish this ownership.
 * io is a real hardware implementation; none is synthesized by this module.
 * Initialization additionally preserves every present PTE within the window.
 * The first and last global GGTT pages are guards and cannot be in the window.
 * space must start zeroed. bitmap needs ceil(page_count/8) bytes and remains
 * exclusively owned by space. Neither it nor callbacks may move or change.
 * Calls are nonblocking with respect to other callers: contention returns
 * BUSY, including callback reentry. No spin or IRQ-disabled hardware waits. */
enum intel_ggtt_bind_status intel_ggtt_space_init(intel_ggtt_space_t *,
    const intel_ggtt_layout_t *, uint32_t first_page, uint32_t page_count,
    uint8_t *bitmap, size_t bitmap_bytes, const intel_ggtt_io_t *, void *context);

/* Read-only capacity for page-aligned blocks in the owned window, leaving
 * spare_pages free in total (not necessarily contiguous). Use spare_pages=1
 * for an engine HWSP. This is a planning snapshot, not a reservation: bind
 * still rechecks firmware/external changes before publishing any mapping. */
enum intel_ggtt_bind_status intel_ggtt_space_capacity(intel_ggtt_space_t *,
    uint32_t block_pages,uint32_t spare_pages,uint64_t max_address,uint32_t *blocks);

/* dma_pages are already-owned, page-aligned DMA addresses in this IGD's
 * domain, not CPU pointers or unvalidated physical addresses. Caller retains
 * the underlying RAM/DMA mappings through unbind (including quarantine).
 * originals has page_count elements, distinct from dma_pages/bitmap/object.
 * Zero max_address uses the whole window; otherwise it is an exclusive,
 * page-aligned GPU limit (e.g. the CPU aperture size for mappable buffers).
 * On OK or QUARANTINED, binding becomes live and owns a reservation; all other
 * failures leave it EMPTY and impose no lifetime on underlying DMA pages.
 * Binding and its originals must be private while bind is in progress. */
enum intel_ggtt_bind_status intel_ggtt_bind(intel_ggtt_space_t *,
    intel_ggtt_binding_t *, const uint64_t *dma_pages, uint32_t page_count,
    uint32_t alignment_pages, uint64_t max_address, uint64_t *originals);
/* Releasing a pinned mapping fails BUSY. If restore/verification fails, keep
 * RAM and DMA mappings alive: the binding is quarantined and unbind may retry.
 * cookie prevents a stale reference from operating on a recycled object. */
enum intel_ggtt_bind_status intel_ggtt_unbind(intel_ggtt_space_t *,
    intel_ggtt_binding_t *, uint64_t cookie);
enum intel_ggtt_bind_status intel_ggtt_pin(intel_ggtt_space_t *,
    intel_ggtt_binding_t *, uint64_t cookie);
/* Only call after the engine has actually completed the associated use, or
 * after cancelling before submission. This is a reference, not a GPU fence. */
enum intel_ggtt_bind_status intel_ggtt_unpin(intel_ggtt_space_t *,
    intel_ggtt_binding_t *, uint64_t cookie);
#endif
