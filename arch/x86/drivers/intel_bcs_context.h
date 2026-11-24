/* Native BCS/RCS context with separately owned surfaces, state and tables. */
#ifndef OSITOK_INTEL_BCS_CONTEXT_H
#define OSITOK_INTEL_BCS_CONTEXT_H
#include "intel_gpu_buffer.h"
#include "intel_ppgtt.h"
#include "intel_ggtt_space.h"
#include "intel_bcs_queue.h"
#include "intel_rcs_draw.h"
/* Six ordinary ranges, sixteen textures and 28 coherent ranges fit the
 * bounded native table builder. Backing stays owned independently of CPU VM. */
#define INTEL_BCS_COHERENT_RANGES 28u
#define INTEL_BCS_TEXTURES 16u
typedef struct {
    intel_gpu_buffer_t *buffer;
    uint64_t gpu;
} intel_bcs_texture_binding_t;
typedef struct {
    intel_gpu_buffer_t *buffer;
    uint64_t gpu;
} intel_bcs_program_binding_t;

typedef struct {
    intel_gpu_buffer_t *buffer;
    uint64_t gpu;
} intel_bcs_index_binding_t;
typedef struct {
    intel_gpu_coherent_reference_t reference;
    uint64_t gpu;
    bool mapped,resident;
} intel_bcs_coherent_range_t;

typedef struct {
    intel_gpu_buffer_t state, tables, batch;
    intel_gpu_buffer_t *source, *destination;
    intel_gpu_buffer_t *program,*retained_program;
    intel_bcs_program_binding_t vertex_program;
    intel_gpu_buffer_t *retained_vertex_program;
    intel_bcs_index_binding_t indices;
    intel_gpu_buffer_t *retained_indices;
    intel_gpu_buffer_t *retained_source, *retained_destination;
    intel_gpu_access_t source_access,destination_access;
    intel_gpu_access_t program_access;
    intel_gpu_access_t vertex_program_access;
    intel_gpu_access_t index_access;
    intel_bcs_texture_binding_t textures[INTEL_BCS_TEXTURES];
    intel_gpu_buffer_t *retained_textures[INTEL_BCS_TEXTURES];
    intel_gpu_access_t texture_access[INTEL_BCS_TEXTURES];
    unsigned texture_count,retained_texture_count;
    uint64_t access_token, epoch;
    uint64_t retained_owner, queue_owner;
    intel_ggtt_binding_t binding;
    intel_ggtt_space_t *space;
    intel_bcs_queue_t *queue;
    uint64_t originals[23], owner, source_gpu, destination_gpu, batch_gpu, descriptor, queue_id, rebindings;
    uint64_t program_gpu;
    uint32_t hardware_id, table_pages, tail;
    bool started, ready, source_pinned, destination_pinned, ggtt_pinned, prepared, reserved, render;
    bool program_pinned;
    bool vertex_program_pinned;
    bool indices_pinned;
    bool bindings_closed;
    intel_bcs_coherent_range_t coherent[INTEL_BCS_COHERENT_RANGES];
} intel_bcs_context_t;
/* Device-worker operation on private zeroed context storage. The caller owns
 * stopped GT/GGTT and supplies two distinct live buffers of the same owner.
 * VA windows and table arena are fixed; rebind can replace surface backing
 * under the queue's exclusive update gate. Private PAT3
 * must be UC before executing it. hardware_id is device-assigned, unique for
 * the engine session. Global HWSP is separately owned by the engine.
 * On any failure, retain this object and retry destroy until it succeeds;
 * quarantined mappings/allocator outputs never authorize freeing DMA RAM. */
bool intel_bcs_context_create(intel_bcs_context_t *, intel_ggtt_space_t *,
    const intel_gpu_allocator_t *, void *, uint64_t owner, uint32_t hardware_id,
    intel_gpu_buffer_t *source, uint64_t source_gpu,
    intel_gpu_buffer_t *destination, uint64_t destination_gpu, uint64_t ggtt_limit);
/* Reserve device-owned state and an empty page-table arena while GT is
 * stopped. No application, surface or logical context is needed. Once the
 * engine is live, its sole executor can activate this previously unused slot
 * without allocating RAM or mutating GGTT. Each slot is assigned once per
 * engine session. retarget can replace a closed logical owner later, assigning
 * a fresh queue identity while preserving this hardware state/ring/ID.
 * Partial reserve/activate failures retain storage until destroy after engine
 * retirement. table_pages bounds later surface rebinding capacity. */
bool intel_bcs_context_reserve(intel_bcs_context_t *,intel_ggtt_space_t *,
    const intel_gpu_allocator_t *,void *,uint64_t storage_owner,uint32_t hardware_id,
    uint64_t source_gpu,uint64_t destination_gpu,uint32_t table_pages,uint64_t ggtt_limit);
/* Same ownership rules, with full RCS save area and a private read-only batch
 * mapping. Only the trusted textured rectangle encoder can populate it. */
bool intel_bcs_context_reserve_render(intel_bcs_context_t *,intel_ggtt_space_t *,
    const intel_gpu_allocator_t *,void *,uint64_t storage_owner,uint32_t hardware_id,
    uint64_t source_gpu,uint64_t destination_gpu,uint32_t table_pages,uint64_t ggtt_limit,uint64_t batch_gpu);
/* Render slot with a third read-only mapping window for owned native code.
 * source is the vertex allocation for primitive work. The same slot can bind
 * copy surfaces with no program through the existing two-resource APIs. */
bool intel_bcs_context_reserve_draw(intel_bcs_context_t *,intel_ggtt_space_t *,
    const intel_gpu_allocator_t *,void *,uint64_t,uint32_t,uint64_t,uint64_t,
    uint32_t,uint64_t,uint64_t,uint64_t program_gpu);
enum intel_bcs_status intel_bcs_context_activate_draw(intel_bcs_context_t *,uint64_t,
    intel_gpu_buffer_t *vertices,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program);
enum intel_bcs_status intel_bcs_context_rebind_draw(intel_bcs_context_t *,
    intel_gpu_buffer_t *vertices,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program);
enum intel_bcs_status intel_bcs_context_retarget_draw(intel_bcs_context_t *,uint64_t,uint64_t,
    intel_gpu_buffer_t *vertices,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program);
enum intel_bcs_status intel_bcs_context_activate(intel_bcs_context_t *,uint64_t owner,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination);
/* backing is the immutable kernel callback payload, or NULL for this context.
 * It remains alive until successful destruction, including deferred close. */
bool intel_bcs_context_attach(intel_bcs_context_t *, intel_bcs_queue_t *, void *backing);
bool intel_bcs_context_destroy(intel_bcs_context_t *);
/* Close admission, wait for queue references, remove all application mappings
 * and acknowledge the GPU TLB before releasing their pins. Device-owned state
 * stays reserved for retargeting. Failure retains backing until retirement. */
enum intel_bcs_status intel_bcs_context_close_bindings(intel_bcs_context_t *);
/* Paired queue-worker operations. A failed/partial acquire still requires
 * retire; it may retain one surface. Only call retire after no submission,
 * proven hardware completion or successful engine retirement. Idle context
 * residency alone permits CPU access. No buffer access may race prepare. */
enum intel_bcs_status intel_bcs_context_acquire(intel_bcs_context_t *,uint64_t token);
enum intel_bcs_status intel_bcs_context_retire(intel_bcs_context_t *,uint64_t token);
/* Replace the two surface mappings of an attached BCS or RCS context without
 * restarting the engine or moving its table/context/ring allocations. New
 * owned buffers must fit the existing table arena and fixed VA windows.
 * Both inputs stay alive for the call; the caller serializes destruction.
 * BUSY/INVALID make no mapping change. QUARANTINED retains old and candidate
 * pins until destroy after engine retirement. Success releases old pins only
 * after acknowledged engine TLB invalidation, allowing their immediate collection.
 * Close/exit racing a successful transaction still prevents future enqueue. */
enum intel_bcs_status intel_bcs_context_rebind(intel_bcs_context_t *,
    intel_gpu_buffer_t *source, intel_gpu_buffer_t *destination);
/* Sole device executor, attached idle context. Caller retains access for this
 * call, owns the matching device/incarnation, and holds PAT4 WB/LLC until all
 * coherent mappings and engines retire. No guest pointer is accepted.
 * Bind retains backing/residency before publishing page tables. Ordinary
 * surface replacement preserves these addresses. Unbind releases only after
 * cache publication and acknowledged TLB invalidation. A failed publication
 * quarantines the context, retaining backing until engine teardown. Retarget
 * to another logical context removes ALL previous coherent mappings.
 * This internal operation does not advertise a public monitored-fence ABI. */
enum intel_bcs_status intel_bcs_context_bind_coherent(intel_bcs_context_t *,
    intel_gpu_coherent_access_t *,uint64_t gpu,const intel_ppgtt_pat_t *);
enum intel_bcs_status intel_bcs_context_unbind_coherent(intel_bcs_context_t *,uint64_t gpu);
/* Closed logical owner only; preserves hardware state/ring/ID and GGTT.
 * New surfaces may belong to another process incarnation. Old and candidate
 * owners remain distinguishable through partial cleanup and quarantine. */
enum intel_bcs_status intel_bcs_context_retarget(intel_bcs_context_t *,uint64_t owner,
    uint64_t epoch,intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination);
/* Queue worker only: validates against owned mappings, builds/appends the ring
 * and synchronizes caches. No CPU/GPU writes may race these buffer accesses.
 * The queue must confirm the prior job's hardware completion before reuse. */
bool intel_bcs_context_prepare(intel_bcs_context_t *, const intel_bcs_copy_t *,
    uint64_t token, uint64_t *descriptor, uint32_t *tail);
/* Acquired, compiler-validated kernel snapshot only. program_gpu identifies
 * the code region within the bound allocation, at program.code_offset. */
bool intel_bcs_context_prepare_draw(intel_bcs_context_t *,const intel_rcs_queue_draw_t *,
    uint64_t,uint64_t *,uint32_t *);
/* Sampled contexts reserve the larger state batch. Bindings list UNIQUE
 * retained buffers, not shader slots; several samplers can refer to one
 * binding. GPU ranges are kernel-assigned, aligned and read-only. Texture
 * maps, geometry and program replace as one acknowledged TLB transaction.
 * The legacy activate/rebind/retarget APIs install an empty texture set.
 * Caller keeps the binding array stable for the call and serializes access.
 * These operations map/retain resources, not sampled application commands. */
bool intel_bcs_context_reserve_sampled(intel_bcs_context_t *,intel_ggtt_space_t *,
    const intel_gpu_allocator_t *,void *,uint64_t,uint32_t,uint64_t,uint64_t,
    uint32_t,uint64_t,uint64_t,uint64_t);
enum intel_bcs_status intel_bcs_context_activate_sampled(intel_bcs_context_t *,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,
    const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_rebind_sampled(intel_bcs_context_t *,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,
    const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_retarget_sampled(intel_bcs_context_t *,uint64_t,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,
    const intel_bcs_texture_binding_t *,unsigned);
/* Same transaction with an independent read-only vertex program. The kernel
 * assigns its GPU window. Legacy operations remove this mapping atomically.
 * Both shader allocations stay resident through TLB acknowledgment; execution
 * acquires both exclusively and retains them through confirmed retirement. */
enum intel_bcs_status intel_bcs_context_activate_vertex(intel_bcs_context_t *,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_rebind_vertex(intel_bcs_context_t *,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_retarget_vertex(intel_bcs_context_t *,uint64_t,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    const intel_bcs_texture_binding_t *,unsigned);
/* Independent read-only index allocation. Optional VS; the same transaction
 * pins, publishes and retires indices with all geometry/code/image mappings.
 * Legacy binding APIs atomically remove the index mapping. */
enum intel_bcs_status intel_bcs_context_activate_indexed(intel_bcs_context_t *,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    intel_bcs_index_binding_t,const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_rebind_indexed(intel_bcs_context_t *,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    intel_bcs_index_binding_t,const intel_bcs_texture_binding_t *,unsigned);
enum intel_bcs_status intel_bcs_context_retarget_indexed(intel_bcs_context_t *,uint64_t,uint64_t,
    intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_gpu_buffer_t *,intel_bcs_program_binding_t,
    intel_bcs_index_binding_t,const intel_bcs_texture_binding_t *,unsigned);
#endif
