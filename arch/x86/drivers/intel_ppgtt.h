/* Gen9 legacy 48-bit per-context page tables. Kernel-owned, offline build. */
#ifndef OSITOK_INTEL_PPGTT_H
#define OSITOK_INTEL_PPGTT_H
#include "intel_ggtt.h"
#include "intel_gt.h"
#ifndef __KERNEL_X86__
#include <stddef.h>
#endif
#define INTEL_PPGTT_VA_LIMIT (1ULL << 47)
#define INTEL_PPGTT_MAX_MAPPINGS 65536u
#define INTEL_PPGTT_MAX_RANGES 64u
typedef struct {
    uint64_t gpu, dma;
    bool writable;
} intel_ppgtt_page_t;

/* Build four-level, 4 KiB-page tables in a caller-owned contiguous DMA arena.
 * Entries are sorted by increasing, unique GPU VA in the low canonical half;
 * neither address zero nor mappings of the table arena itself are accepted.
 * Data PTEs select private PAT index 3 (PWT|PCD): caller must establish that
 * index as uncached on the owned adapter. Intermediate tables use index 0.
 * The complete arena, inputs and output must be distinct CPU buffers. On
 * failure nothing is changed. On success all unused entries are non-present.
 * No MMIO, allocation, cache flush or publication is performed here.
 * Caller must validate its DMA domain/ownership, flush before publishing and
 * retain every table/data page until engine retirement, or replace data
 * mappings under an exclusive engine gate after confirmed completion and
 * before an acknowledged hardware TLB invalidation. The offline builder
 * itself supplies none of that synchronization or user-mode isolation. */
bool intel_ppgtt_build(uint64_t *arena, size_t arena_bytes, uint64_t arena_dma,
    const intel_ppgtt_page_t *pages, uint32_t count, uint32_t *tables_used);

typedef struct { uint64_t gpu, dma, bytes; bool writable; } intel_ppgtt_range_t;
/* Owned contiguous DMA ranges, sorted/nonoverlapping in GPU VA. The planner
 * validates the ranges and reports the exact number of table pages without
 * allocating a per-page metadata vector. Same offline/cache/lifetime contract
 * as build above; count is limited to 64 ranges and 65536 mapped pages. */
bool intel_ppgtt_range_pages(const intel_ppgtt_range_t *, uint32_t count,
    uint32_t *table_pages);
bool intel_ppgtt_build_ranges(uint64_t *arena, size_t bytes, uint64_t dma,
    const intel_ppgtt_range_t *, uint32_t count, uint32_t *tables_used);
/* Same offline transaction, with selected ranges using private PAT4 (WB LLC).
 * Bit i selects range i; all other ranges retain PAT3 UC. Mixed-cache aliases
 * of the same physical pages are rejected. Caller must hold the coherent PAT
 * policy and establish CPU WB/IO-coherent DMA provenance before execution.
 * This builds mappings only, and does not grant simultaneous buffer access. */
bool intel_ppgtt_build_ranges_coherent(uint64_t *,size_t,uint64_t,
    const intel_ppgtt_range_t *,uint32_t,uint64_t coherent_mask,uint32_t *);

typedef struct {
    uint32_t original, last_value;
    bool started, changed, ready;
} intel_ppgtt_pat_t;
/* Own a stopped CFL GT before changing the shared private PAT. Only slot 3
 * becomes UC; all other slots are preserved. Keep this zero-initialized state
 * until restoration, including a write that applied but reported failure.
 * Never restore while an engine can still use a context built with slot 3.
 * A failed restore retains changed=true and may be retried after retirement. */
bool intel_ppgtt_pat_acquire(const intel_gt_io_t *, void *, intel_ppgtt_pat_t *);
bool intel_ppgtt_pat_restore(const intel_gt_io_t *, void *, intel_ppgtt_pat_t *);
/* Stopped, exclusively owned CFL GT: own only PAT4 (40E4[7:0]), selecting WB
 * and LLC. Preserve every other entry. Keep state/backing until DMA retirement
 * and restoration, including writes that applied but reported an error. */
bool intel_ppgtt_coherent_pat_acquire(const intel_gt_io_t *,void *,intel_ppgtt_pat_t *);
bool intel_ppgtt_coherent_pat_restore(const intel_gt_io_t *,void *,intel_ppgtt_pat_t *);
#endif
