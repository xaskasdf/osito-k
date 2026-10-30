/* Captured native UMD work and retained process-owned resource references. */
#ifndef OSITOK_INTEL_GPU_CAPTURE_H
#define OSITOK_INTEL_GPU_CAPTURE_H
#include "intel_gpu_kmt.h"
#include "intel_gpu_umd_abi.h"
#include "intel_bcs_context.h"
typedef struct intel_gpu_capture {
    struct intel_gpu_capture *self;
    intel_gpu_kmt_context_lease_t context;
    intel_gpu_kmt_resident_lease_t resources[INTEL_UMD_MAX_RESOURCES]; /* unique: vertices, target, code, images */
    union { intel_umd_copy32_t command; intel_umd_draw32_t draw; intel_umd_draw_constants_t constants;
            intel_umd_draw_rectangles_t batch; intel_umd_draw_sampled_t sampled;
            intel_umd_draw_vertex_shader_t vertex; intel_umd_draw_indexed_t indexed; };
    struct intel_d3d9_program program;
    intel_gpu_cpu_access_t program_cpu;
    struct intel_d3d9_vertex_program vertex_program;
    intel_gpu_cpu_access_t vertex_program_cpu;
    intel_gpu_cpu_access_t index_cpu;
    OKMT_ALLOCATION_LIST allocations[INTEL_UMD_MAX_RESOURCES];
    OKMT_PATCH_LOCATION patches[INTEL_UMD_MAX_RESOURCES];
    unsigned texture_resource[INTEL_UMD_MAX_TEXTURES]; /* sampler slot -> unique lease */
    unsigned resource_count;
    intel_gpu_fence_lease_t fences[INTEL_BCS_COHERENT_RANGES];
    unsigned fence_count;
    bool valid;
} intel_gpu_capture_t;
static inline bool intel_gpu_capture_has_indices(const intel_gpu_capture_t *c)
{ return c && c->draw.version==INTEL_UMD_DRAW_INDEXED_VERSION; }
static inline bool intel_gpu_capture_has_vertex(const intel_gpu_capture_t *c)
{ return c && (c->draw.version==INTEL_UMD_DRAW_VERTEX_SHADER_VERSION ||
    (intel_gpu_capture_has_indices(c) && c->vertex.vertex_program_bytes)); }
/* Driver-owned zeroed stable storage, NEVER a cancellable client stack object.
 * offset/bytes describe a bounded staging subrange (not the public Render ABI).
 * Only this call reads the mutable process staging bytes; all validation and
 * later encoding use the private captured copy. Malicious concurrent writes
 * can change what gets captured, not bypass validation or inject GPU addresses.
 * Every accepted begin, including failure, must release until success. Failed
 * capture can retain partial leases. No submission or execution admission is
 * granted: the enqueue transaction must recheck close/exit/removal atomically. */
NTSTATUS intel_gpu_capture_begin(DWORD pid,OKMT_HANDLE context,DWORD offset,DWORD bytes,
    DWORD allocations,DWORD patches,intel_gpu_capture_t *);
/* Idempotent after success. BUSY retains ownership for the persistent driver's
 * retirement work; never abandon/reuse the capture on a failed release. */
NTSTATUS intel_gpu_capture_release(intel_gpu_capture_t *);
/* Worker owns stable native engine mappings for the whole call. Bind the
 * captured resources to that context before encoding; only its kernel-owned
 * PPGTT addresses enter the resulting copy. No GPU access/fence is acquired;
 * the queue's native acquire/prepare/retire path still controls execution. */
NTSTATUS intel_gpu_capture_copy(const intel_gpu_capture_t *,const intel_bcs_context_t *,intel_bcs_copy_t *);
NTSTATUS intel_gpu_capture_draw(const intel_gpu_capture_t *,const intel_bcs_context_t *,intel_rcs_queue_draw_t *);
#endif
