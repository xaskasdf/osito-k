/* Native Gen9 blitter context/command qualification, internal kernel API. */
#ifndef OSITOK_INTEL_BCS_H
#define OSITOK_INTEL_BCS_H
#include "intel_cs.h"
#ifndef __KERNEL_X86__
#include <stddef.h>
#endif

#define INTEL_BCS_CONTEXT_BYTES 8192u
#define INTEL_BCS_RING_BYTES 4096u
#define INTEL_BCS_TOKEN_OFFSET 0x800u
#define INTEL_BCS_TAIL 32u
#define INTEL_BCS_COPY_TAIL 64u

/* Build a fresh, kernel-only context and one MI_FLUSH_DW sequence. Caller
 * owns separate CPU buffers and pinned GGTT/DMA ranges. PML4 points to owned,
 * flushed page tables; this token-only program uses GGTT/PPHWSP addresses.
 * No rendering capabilities or user command parsing are implied. */
bool intel_bcs_build(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint64_t context_gpu,
    uint64_t ring_gpu, uint64_t pml4_dma, uint32_t context_id,
    uint64_t token, uint64_t *descriptor);
#define INTEL_BCS_FENCE_TAIL 96u
/* Trusted native fence store through a caller-owned writable PPGTT mapping.
 * The cacheable fence page must remain pinned and coherent with CPU WB views.
 * MI_STORE_DATA_IMM initiates a qword write; this does not certify atomicity or
 * replace the flush/CSB/idle evidence required for retiring the engine. */
bool intel_bcs_build_fence(uint32_t *,size_t,uint32_t *,size_t,uint64_t,uint64_t,
    uint64_t,uint32_t,uint64_t token,uint64_t fence_gpu,uint64_t value,uint64_t *);

typedef struct {
    uint64_t source, source_bytes, destination, destination_bytes;
    uint32_t source_pitch, destination_pitch, width, height;
    uint32_t source_x, source_y, destination_x, destination_y;
} intel_bcs_copy_t;
/* Build a 32bpp linear XY_SRC_COPY_BLT followed by the completion token.
 * Surface addresses are owned PPGTT mappings, distinct from GGTT context,
 * ring and HWSP addresses. Pitches are positive bytes; origins are pixels
 * within these mappings, not offsets added to their aligned GPU addresses.
 * Caller verifies mapping permissions and lifetime for every surface page.
 * This internal builder accepts no user command buffer or raw hardware state. */
bool intel_bcs_build_copy(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint64_t context_gpu,
    uint64_t ring_gpu, uint64_t pml4_dma, uint32_t context_id,
    uint64_t token, const intel_bcs_copy_t *, uint64_t *descriptor);
/* After confirmed context-complete/idle and CPU cache synchronization, append
 * to an empty, saved ring. Preserve the hardware-saved engine image and head;
 * clear restore-inhibit for subsequent use. Caller flushes before resubmission.
 * No live context, pending tail or arbitrary user command stream is accepted. */
bool intel_bcs_append_copy(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint32_t completed_tail,
    uint64_t token, const intel_bcs_copy_t *, uint32_t *next_tail);

/* Historical BCS source API, with a shared engine-session representation. */
#define intel_bcs_status intel_cs_status
typedef intel_cs_state_t intel_bcs_state_t;
#define INTEL_BCS_OK INTEL_CS_OK
#define INTEL_BCS_INVALID INTEL_CS_INVALID
#define INTEL_BCS_IO INTEL_CS_IO
#define INTEL_BCS_BUSY INTEL_CS_BUSY
#define INTEL_BCS_TIMEOUT INTEL_CS_TIMEOUT
#define INTEL_BCS_ENGINE_ERROR INTEL_CS_ENGINE_ERROR
#define INTEL_BCS_QUARANTINED INTEL_CS_QUARANTINED
#define INTEL_BCS_PENDING INTEL_CS_PENDING

/* Serialized kernel engine session. Enable once; submit/check repeatedly;
 * close once on final shutdown or any hardware error. This layer does not
 * acquire caller locks, allocate, sleep or expose command buffers to users.
 * Check performs one bounded observation; wait is a bring-up polling helper.
 * A completed job permits reuse of its saved context/ring and data, but all
 * context/HWSP/table storage remains owned until close confirms retirement.
 * Any I/O/timeout error prevents further submission until close/recovery. */
enum intel_bcs_status intel_bcs_enable(const intel_gt_io_t *, void *,
    uint64_t hwsp_gpu, intel_bcs_state_t *);
enum intel_bcs_status intel_bcs_submit(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t expected_token, uint32_t expected_tail, intel_bcs_state_t *);
enum intel_bcs_status intel_bcs_check(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), intel_bcs_state_t *);
enum intel_bcs_status intel_bcs_wait(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), intel_bcs_state_t *);
enum intel_bcs_status intel_bcs_close(const intel_gt_io_t *, void *, intel_bcs_state_t *);

/* Exclusive engine owner, after CPU page-table writes have been flushed and
 * all previous BCS work has completed with its flush token and context save.
 * Forcewake remains held and no other engine may use these page tables.
 * A verified BTCR acknowledgement permits dropping OLD surface mappings;
 * context/ring/HWSP/table storage still requires full engine retirement.
 * Failure poisons submission and retains both old and new DMA resources. */
enum intel_bcs_status intel_bcs_invalidate_tlb(const intel_gt_io_t *, void *,
    intel_bcs_state_t *);

/* Caller owns an exclusively acquired, freshly reset CFL GT2 with forcewake
 * held. Context, ring, PML4 and global HWSP must remain pinned, flushed and
 * alive throughout this call. No competing engine or IRQ handler may submit.
 * token_read reads real GPU-visible completion memory; it must not synthesize
 * completion. This is a one-shot polling bring-up path, not a scheduler.
 * Completion requires token + matching CSB completion + idle/head evidence.
 * BCS is reset before retiring references even after successful completion.
 * If retired is false on return, keep all resources pinned/quarantined. */
enum intel_bcs_status intel_bcs_run(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t hwsp_gpu, uint64_t expected_token, uint32_t expected_tail,
    intel_bcs_state_t *);
#endif
