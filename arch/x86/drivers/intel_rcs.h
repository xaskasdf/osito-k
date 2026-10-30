/* Native Gen9 render-context and pipeline completion commands. */
#ifndef OSITOK_INTEL_RCS_H
#define OSITOK_INTEL_RCS_H
#include "intel_cs.h"
#ifndef __KERNEL_X86__
#include <stddef.h>
#endif
#define INTEL_RCS_CONTEXT_BYTES INTEL_CS_RENDER_CONTEXT_BYTES
#define INTEL_RCS_RING_BYTES 4096u
#define INTEL_RCS_TOKEN_OFFSET 0x800u
#define INTEL_RCS_FLUSH_BYTES 64u
/* Fresh context, exclusive reset RCS. Full 22-page save area and separate
 * GGTT ring are owned/pinned by the caller. PML4 is owned, validated/flushed.
 * Establishes logical-ring state with render restore inhibited on first use.
 * Emits a render PIPE_CONTROL flush/invalidate and PPHWSP completion token;
 * does not yet initialize the shader/rasterization pipeline or draw pixels. */
bool intel_rcs_build(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint64_t context_gpu,uint64_t ring_gpu,uint64_t pml4_dma,
    uint32_t context_id,uint64_t token,uint64_t *descriptor);
/* Fresh context: initial flush, trusted batch, final token and context save.
 * Does not fabricate a completed head to reuse the append API. */
bool intel_rcs_build_batch(uint32_t *,size_t,uint32_t *,size_t,uint64_t context_gpu,
    uint64_t ring_gpu,uint64_t pml4_dma,uint32_t context_id,uint64_t batch_gpu,
    uint64_t token,uint64_t *descriptor,uint32_t *tail);
/* Confirmed matching context save and idle, then CPU cache synchronization
 * required before append. Retains the complete hardware-saved render image.
 * Flush caches before submission. No user command stream is accepted. */
bool intel_rcs_append(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint32_t completed_tail,uint64_t token,uint32_t *next_tail);
/* Append a trusted, pinned PPGTT batch followed by completion. Same saved
 * context contract as append; the batch must return via MI_BATCH_BUFFER_END.
 * No nested batch or unvalidated client stream. Ring may wrap between jobs. */
bool intel_rcs_append_batch(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint32_t completed_tail,uint64_t batch_gpu,
    uint64_t token,uint32_t *next_tail);
typedef struct { uint32_t original[3],changed; bool started,ready; } intel_rcs_cache_t;
/* Exclusively stopped Gen9 GT, with forcewake held. Slots 0/1 UC at both
 * eLLC/LLC and L3; restore only after RCS retirement. Failure retains changes. */
bool intel_rcs_cache_acquire(const intel_gt_io_t *,void *,intel_rcs_cache_t *);
bool intel_rcs_cache_restore(const intel_gt_io_t *,void *,intel_rcs_cache_t *);
#endif
