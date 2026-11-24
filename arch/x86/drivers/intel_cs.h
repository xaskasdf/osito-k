/* Native Gen9 execlist session shared by copy and render engines. */
#ifndef OSITOK_INTEL_CS_H
#define OSITOK_INTEL_CS_H
#include "intel_gt.h"
enum intel_cs_engine { INTEL_CS_COPY, INTEL_CS_RENDER };
enum intel_cs_status {
    INTEL_CS_OK, INTEL_CS_INVALID, INTEL_CS_IO, INTEL_CS_BUSY,
    INTEL_CS_TIMEOUT, INTEL_CS_ENGINE_ERROR, INTEL_CS_QUARANTINED,
    INTEL_CS_PENDING
};
#define INTEL_CS_COPY_CONTEXT_BYTES 8192u
#define INTEL_CS_RENDER_CONTEXT_BYTES (22u*4096u)
typedef struct {
    bool started, referenced, submitted, completed, retired;
    enum intel_cs_status status;
    uint32_t fault_reg, last_value, csb_status, csb_id;
    uint32_t head, tail, execlist_status, engine_error;
    uint64_t observed_token, elapsed_ns;
    bool enabled, inflight;
    uint32_t csb_head, context_id, target_tail, hwsp_gpu;
    uint64_t target_token, job_start_ns, submitted_jobs, completed_jobs, tlb_invalidations;
    enum intel_cs_engine engine;
} intel_cs_state_t;

/* Exclusive, freshly reset CFL engine, forcewake and validated DMA domain
 * retained by caller. Zeroed stable session storage; engine selection never
 * changes after enable. No allocation, user pointers or lock acquisition.
 * The caller keeps context/ring/tables/HWSP pinned through close/retirement.
 * Commands must end in the engine's flush+post-sync token protocol; a token
 * alone is insufficient without matching context save and idle head/tail.
 * Uncertain submission/timeout poisons reuse. Check observes once; wait/run
 * are bounded polling helpers for bring-up, not a production scheduler. */
enum intel_cs_status intel_cs_enable(enum intel_cs_engine, const intel_gt_io_t *,
    void *, uint64_t hwsp_gpu, intel_cs_state_t *);
enum intel_cs_status intel_cs_submit(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t expected_token, uint32_t expected_tail, intel_cs_state_t *);
enum intel_cs_status intel_cs_check(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), intel_cs_state_t *);
enum intel_cs_status intel_cs_wait(const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), intel_cs_state_t *);
enum intel_cs_status intel_cs_close(const intel_gt_io_t *, void *, intel_cs_state_t *);
/* After CPU table publication and confirmed pipeline flush/completion,
 * acknowledge this engine's TLB invalidation before releasing old mappings.
 * No other engine may use the changed mappings without its own barrier. */
enum intel_cs_status intel_cs_invalidate_tlb(const intel_gt_io_t *, void *, intel_cs_state_t *);
enum intel_cs_status intel_cs_run(enum intel_cs_engine, const intel_gt_io_t *, void *,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t hwsp_gpu, uint64_t expected_token, uint32_t expected_tail, intel_cs_state_t *);
#endif
