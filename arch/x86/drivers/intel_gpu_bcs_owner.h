/* Device-owned BCS session over captured KMT resources. No runtime substitute. */
#ifndef OSITOK_INTEL_GPU_BCS_OWNER_H
#define OSITOK_INTEL_GPU_BCS_OWNER_H
#include "intel_gpu_executor.h"
/* Kernel VA partition: ordinary windows at 4 GiB, textures at 8..12 GiB,
 * coherent fence windows at 16..20 GiB. Each image owns a whole 256 MiB slot;
 * actual backing remains constrained by the shared page/quota limits. */
#define INTEL_GPU_TEXTURE_VA_BASE 0x200000000ULL
#define INTEL_GPU_TEXTURE_VA_STRIDE 0x10000000ULL
/* Both instruction streams must fit one 256 MiB instruction-base window. */
#define INTEL_GPU_VERTEX_PROGRAM_VA_OFFSET 0x08000000ULL
/* Separate 256 MiB index window, outside shader and image windows. */
#define INTEL_GPU_INDEX_VA_BASE 0x180000000ULL

typedef struct {
    intel_ggtt_space_t *space;
    intel_gpu_allocator_t allocator;
    void *allocator_context;
    intel_gt_io_t io;
    void *hardware_context;
    bool (*token_read)(void *,uint64_t gpu_address,uint64_t *);
    void (*mapping_stopped)(void *,bool);
    uint64_t ggtt_limit,source_gpu,destination_gpu,first_fence;
    uint32_t first_hardware_id;
    bool render;
    uint64_t batch_gpu,program_gpu; /* Nonzero program window enables verified primitive work. */
} intel_gpu_bcs_config_t;
struct intel_gpu_bcs_owner;
typedef struct {
    struct intel_gpu_bcs_owner *owner;
    intel_bcs_context_t native;
    intel_gpu_fence_mapping_t fences[INTEL_BCS_COHERENT_RANGES];
    uint64_t context;
    DWORD pid,device;
} intel_gpu_bcs_slot_t;
typedef struct intel_gpu_bcs_owner {
    struct intel_gpu_bcs_owner *self;
    intel_gpu_bcs_config_t config;
    intel_gpu_jobs_t *jobs;
    intel_gpu_bcs_slot_t slots[INTEL_BCS_QUEUE_CONTEXTS];
    intel_bcs_state_t engine;
    intel_bcs_queue_t queue;
    intel_ppgtt_pat_t pat;
    intel_ppgtt_pat_t coherent_pat;
    intel_rcs_cache_t cache;
    intel_gpu_buffer_t hwsp;
    intel_ggtt_binding_t hwsp_binding;
    uint64_t hwsp_original,executor;
    unsigned count;
    bool hwsp_pinned,starting,closing,closed;
} intel_gpu_bcs_owner_t;

/* Stable, private device storage; never a PE stack object. One permanent BSP
 * device task serializes init/prepare/start/close, independently of app exit.
 * Caller owns a stopped, acquired GT and DMA domain, keeps forcewake/MMIO,
 * allocator, GGTT and cleanup-service-attached jobs alive until close succeeds.
 * mapping_stopped controls the platform's GGTT mutation gate; it is not proof
 * of retirement supplied by a client. No public adapter is registered here. */
NTSTATUS intel_gpu_bcs_owner_init(intel_gpu_bcs_owner_t *,intel_gpu_jobs_t *,const intel_gpu_bcs_config_t *);
/* Reserve fixed hardware slots before start, without an application capture.
 * The joined executor activates each slot on first use of a new typed logical
 * context. No live GGTT mutation or PMM allocation occurs in that callback.
 * Closed logical owners can yield their slot after queue retirement and a
 * bounded TLB-acknowledged surface replacement; hardware storage stays fixed.
 * Exhaustion with all logical owners still live fails work. */
NTSTATUS intel_gpu_bcs_owner_reserve(intel_gpu_bcs_owner_t *,unsigned slots,unsigned table_pages);
/* Prepare bounded native contexts while stopped, using a typed, retained
 * capture. The capture stays alive for this call; native residency then owns
 * the mapped surfaces. Every partial failure requires close, never memset.
 * Context identities are kernel-assigned and never recycled within a session. */
NTSTATUS intel_gpu_bcs_owner_prepare(intel_gpu_bcs_owner_t *,const intel_gpu_capture_t *);
/* Publishes the queue only after all structural mappings exist. Reserved slots
 * activate on first captured work; assigned contexts rebind live surfaces
 * within their allocated table capacity using the acknowledged TLB gate. */
NTSTATUS intel_gpu_bcs_owner_start(intel_gpu_bcs_owner_t *);
/* Nonblocking lifecycle progression. First stops/detaches the executor, then
 * retires engine/queue, restores PAT and returns every GGTT/DMA reference.
 * BUSY or failure retains storage; retry close. Device owner disables IRQ
 * producers first (this session currently uses the executor's timer path).
 * Captured jobs require their independent cleanup service to keep running. */
NTSTATUS intel_gpu_bcs_owner_close(intel_gpu_bcs_owner_t *);
#endif
