#include "intel_gpu_bcs_owner.h"
#include "intel_gpu_service.h"
#include "intel_gen9_shader.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif
extern uint32_t smp_current_cpu(void);

static bool bcs_owner_read(void *opaque,uint32_t reg,uint32_t *value)
{
    intel_gpu_bcs_owner_t *o=((intel_gpu_bcs_slot_t *)opaque)->owner;
    return o->config.io.read(o->config.hardware_context,reg,value);
}
static bool bcs_owner_write(void *opaque,uint32_t reg,uint32_t value)
{
    intel_gpu_bcs_owner_t *o=((intel_gpu_bcs_slot_t *)opaque)->owner;
    return o->config.io.write(o->config.hardware_context,reg,value);
}
static uint64_t bcs_owner_now(void *opaque)
{
    intel_gpu_bcs_owner_t *o=((intel_gpu_bcs_slot_t *)opaque)->owner;
    return o->config.io.now_ns(o->config.hardware_context);
}
static void bcs_owner_relax(void *opaque)
{
    intel_gpu_bcs_owner_t *o=((intel_gpu_bcs_slot_t *)opaque)->owner;
    o->config.io.relax(o->config.hardware_context);
}
static const intel_gt_io_t bcs_owner_io={bcs_owner_read,bcs_owner_write,bcs_owner_now,bcs_owner_relax};
static bool bcs_owner_token(void *opaque,uint64_t *value)
{
    intel_gpu_bcs_slot_t *s=opaque;
    if (s->native.binding.phase!=INTEL_GGTT_BOUND || !s->native.ggtt_pinned) return false;
    return s->owner->config.token_read(s->owner->config.hardware_context,
        s->native.binding.gpu_address+INTEL_BCS_TOKEN_OFFSET,value);
}
static bool bcs_owner_prepare(void *opaque,const intel_bcs_copy_t *copy,uint64_t token,
    uint64_t *descriptor,uint32_t *tail)
{
    return intel_bcs_context_prepare(&((intel_gpu_bcs_slot_t *)opaque)->native,copy,token,descriptor,tail);
}
static bool bcs_owner_prepare_draw(void *opaque,const intel_rcs_queue_draw_t *work,uint64_t token,
    uint64_t *descriptor,uint32_t *tail)
{
    intel_bcs_context_t *c=&((intel_gpu_bcs_slot_t *)opaque)->native;
    /* CPU views are excluded by acquisition before inspecting any code.
     * Recheck the complete program: the client may have changed its allocation
     * since capture. Ownership remains held until this queue proves retirement. */
    if (!c->program || c->access_token!=token || c->program_access.self!=&c->program_access ||
        c->program_access.buffer!=c->program || work->program.allocation_bytes>c->program->bytes ||
        !(work->program.abi==INTEL_D3D9_COMPILER_RESOURCE_ABI ?
          intel_gen9_resource_shader_validate(&work->program,c->program->cpu,work->program.allocation_bytes) :
          intel_gen9_shader_validate(&work->program,c->program->cpu,work->program.allocation_bytes))) return false;
    bool vertex=work->vertex_program.base.abi!=0;
    if (vertex!=!!c->vertex_program.buffer) return false;
    if (vertex && (c->vertex_program_access.self!=&c->vertex_program_access ||
        c->vertex_program_access.buffer!=c->vertex_program.buffer ||
        work->vertex_program.base.allocation_bytes>c->vertex_program.buffer->bytes ||
        !intel_gen9_vertex_shader_validate(&work->vertex_program,c->vertex_program.buffer->cpu,
            work->vertex_program.base.allocation_bytes))) return false;
    return intel_bcs_context_prepare_draw(c,work,token,descriptor,tail);
}
static enum intel_bcs_status bcs_owner_acquire(void *opaque,uint64_t token)
{
    return intel_bcs_context_acquire(&((intel_gpu_bcs_slot_t *)opaque)->native,token);
}
static enum intel_bcs_status bcs_owner_retire(void *opaque,uint64_t token)
{
    return intel_bcs_context_retire(&((intel_gpu_bcs_slot_t *)opaque)->native,token);
}
static bool bcs_owner_mapping_unwanted(intel_gpu_bcs_owner_t *o,const intel_bcs_context_t *c)
{
    intel_gpu_memory_t *m=o->jobs->contexts->memory;
    if (intel_gpu_memory_mapping_unwanted(m,c->source) || intel_gpu_memory_mapping_unwanted(m,c->destination) ||
        intel_gpu_memory_mapping_unwanted(m,c->program) || intel_gpu_memory_mapping_unwanted(m,c->vertex_program.buffer) ||
        intel_gpu_memory_mapping_unwanted(m,c->indices.buffer)) return true;
    for (unsigned i=0;i<c->texture_count;i++)
        if (intel_gpu_memory_mapping_unwanted(m,c->textures[i].buffer)) return true;
    return false;
}
static enum intel_bcs_status bcs_owner_maintain(void *opaque)
{
    intel_gpu_bcs_owner_t *o=opaque;
    for (unsigned i=0;i<o->count;i++) {
        intel_gpu_bcs_slot_t *s=&o->slots[i];
        bool closed=s->pid && intel_gpu_context_closed(o->jobs->contexts,s->pid,s->device,s->context);
        bool park=s->pid && !s->native.bindings_closed && bcs_owner_mapping_unwanted(o,&s->native);
        for (unsigned j=0;j<INTEL_BCS_COHERENT_RANGES;j++) {
            intel_gpu_fence_mapping_t *m=&s->fences[j];
            if (!m->self || (!closed && !park && !intel_gpu_fence_closed(&m->lease))) continue;
            enum intel_bcs_status status=intel_gpu_fence_unmap(m);
            if (status!=INTEL_BCS_OK) return status;
        }
        if ((closed || park) && !s->native.bindings_closed) {
            enum intel_bcs_status status=intel_bcs_context_close_bindings(&s->native);
            if (status!=INTEL_BCS_OK) return status;
            intel_gpu_service_notify(NULL);
        }
    }
    return INTEL_BCS_OK;
}
static enum intel_bcs_status bcs_owner_capture_fences(intel_gpu_bcs_slot_t *s,const intel_gpu_capture_t *capture)
{
    intel_gpu_bcs_owner_t *o=s->owner;
    if (capture->fence_count>INTEL_BCS_COHERENT_RANGES) return INTEL_BCS_INVALID;
    for (unsigned i=0;i<capture->fence_count;i++) {
        const intel_gpu_fence_lease_t *f=&capture->fences[i];
        if (f->self!=f || f->owner!=o->jobs->contexts->fences || f->pid!=s->pid ||
            f->device!=s->device || f->epoch!=s->native.epoch) return INTEL_BCS_INVALID;
    }
    /* Drop obsolete hardware-cache entries before assigning new slots. A
     * retained capture may still bind a logically closed fence afterward. */
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        intel_gpu_fence_mapping_t *m=&s->fences[i];
        if (!m->self) continue;
        bool keep=false;
        for (unsigned j=0;j<capture->fence_count;j++)
            if (m->queue_id==s->native.queue_id && m->lease.owner==capture->fences[j].owner &&
                m->lease.id==capture->fences[j].id) keep=true;
        if (!keep) {
            enum intel_bcs_status status=intel_gpu_fence_unmap(m);
            if (status!=INTEL_BCS_OK) return status;
        }
    }
    for (unsigned i=0;i<capture->fence_count;i++) {
        const intel_gpu_fence_lease_t *f=&capture->fences[i];
        intel_gpu_fence_mapping_t *empty=NULL;bool mapped=false;
        for (unsigned j=0;j<INTEL_BCS_COHERENT_RANGES;j++) {
            intel_gpu_fence_mapping_t *m=&s->fences[j];
            if (!m->self && !empty) empty=m;
            if (m->self && m->lease.owner==f->owner && m->lease.id==f->id) mapped=true;
        }
        if (mapped) continue;
        if (!empty) return INTEL_BCS_INVALID;
        enum intel_bcs_status status=intel_gpu_fence_map(f,&s->native,o->jobs->contexts->memory,
            s->device,&o->coherent_pat,empty);
        if (status!=INTEL_BCS_OK) return status;
    }
    return INTEL_BCS_OK;
}
static enum intel_bcs_status bcs_owner_bind(void *opaque,const intel_gpu_capture_t *capture,
    const intel_bcs_context_t **out)
{
    intel_gpu_bcs_owner_t *o=opaque;
    if (!capture || !capture->valid || capture->self!=capture ||
        capture->context.native.owner!=o->jobs->contexts) return INTEL_BCS_INVALID;
    const intel_gpu_context_lease_t *c=&capture->context.native;
    if (c->node!=(o->config.render ? INTEL_UMD_NODE_RENDER : INTEL_UMD_NODE_COPY)) return INTEL_BCS_INVALID;
    intel_gpu_buffer_t *program=capture->command.opcode==INTEL_UMD_DRAW32 ? capture->resources[2].native.buffer : NULL;
    bool vertex=program && intel_gpu_capture_has_vertex(capture);
    bool indexed=program && intel_gpu_capture_has_indices(capture);
    unsigned geometry=3+vertex+indexed;
    if (program && !o->config.program_gpu) return INTEL_BCS_INVALID;
    if (capture->resource_count<(program ? geometry : 2u) || capture->resource_count>INTEL_UMD_MAX_RESOURCES ||
        (!program && capture->resource_count!=2)) return INTEL_BCS_INVALID;
    unsigned texture_count=program ? capture->resource_count-geometry : 0;
    intel_bcs_program_binding_t vertex_program={0};
    if (vertex) {
        if (o->config.program_gpu>INTEL_PPGTT_VA_LIMIT-INTEL_GPU_VERTEX_PROGRAM_VA_OFFSET) return INTEL_BCS_INVALID;
        vertex_program=(intel_bcs_program_binding_t){capture->resources[3].native.buffer,
            o->config.program_gpu+INTEL_GPU_VERTEX_PROGRAM_VA_OFFSET};
    }
    intel_bcs_index_binding_t indices={0};
    if (indexed) indices=(intel_bcs_index_binding_t){capture->resources[3+vertex].native.buffer,INTEL_GPU_INDEX_VA_BASE};
    if (texture_count>INTEL_BCS_TEXTURES) return INTEL_BCS_INVALID;
    intel_bcs_texture_binding_t textures[INTEL_BCS_TEXTURES];
    _Static_assert(INTEL_BCS_TEXTURES==INTEL_UMD_MAX_TEXTURES,"sampled mapping bound");
    for (unsigned i=0;i<texture_count;i++)
        textures[i]=(intel_bcs_texture_binding_t){capture->resources[geometry+i].native.buffer,
            INTEL_GPU_TEXTURE_VA_BASE+(uint64_t)i*INTEL_GPU_TEXTURE_VA_STRIDE};
    for (unsigned i=0;i<o->count;i++) {
        intel_gpu_bcs_slot_t *s=&o->slots[i];
        if (s->pid!=c->pid || s->device!=c->device || s->context!=c->id) continue;
        intel_gpu_buffer_t *src=capture->resources[0].native.buffer,*dst=capture->resources[1].native.buffer;
        enum intel_bcs_status status=INTEL_BCS_OK;
        bool changed=s->native.source!=src || s->native.destination!=dst || s->native.program!=program ||
            s->native.vertex_program.buffer!=vertex_program.buffer || s->native.vertex_program.gpu!=vertex_program.gpu ||
            s->native.indices.buffer!=indices.buffer || s->native.indices.gpu!=indices.gpu ||
            s->native.texture_count!=texture_count;
        for (unsigned j=0;!changed && j<texture_count;j++)
            changed=s->native.textures[j].buffer!=textures[j].buffer || s->native.textures[j].gpu!=textures[j].gpu;
        if (s->native.bindings_closed) {
            /* The logical context survives pressure. Reopen the retired native
             * queue generation only after restoring mappings and acknowledging
             * its TLB update. Captured work retains its own backing throughout. */
            status=indexed ? intel_bcs_context_retarget_indexed(&s->native,c->pid,c->epoch,
                src,dst,program,vertex_program,indices,textures,texture_count) :
                vertex ? intel_bcs_context_retarget_vertex(&s->native,c->pid,c->epoch,
                src,dst,program,vertex_program,textures,texture_count) :
                program ? intel_bcs_context_retarget_sampled(&s->native,c->pid,c->epoch,
                src,dst,program,textures,texture_count) :
                intel_bcs_context_retarget(&s->native,c->pid,c->epoch,src,dst);
        } else if (changed) {
            status=indexed ? intel_bcs_context_rebind_indexed(&s->native,src,dst,program,vertex_program,indices,textures,texture_count) :
                vertex ? intel_bcs_context_rebind_vertex(&s->native,src,dst,program,vertex_program,textures,texture_count) :
                program ? intel_bcs_context_rebind_sampled(&s->native,src,dst,program,textures,texture_count) :
                intel_bcs_context_rebind(&s->native,src,dst);
            intel_gpu_service_notify(NULL); /* dropped old residency may unblock collection */
        }
        if (status==INTEL_BCS_OK) status=bcs_owner_capture_fences(s,capture);
        if (status==INTEL_BCS_OK) *out=&s->native;
        return status;
    }
    bool retry=false;
    for (unsigned i=0;i<o->count;i++) {
        intel_gpu_bcs_slot_t *s=&o->slots[i];
        if (!s->pid || !intel_gpu_context_closed(o->jobs->contexts,s->pid,s->device,s->context)) continue;
        enum intel_bcs_status status=intel_bcs_queue_close_context(&o->queue,s->native.queue_owner,s->native.queue_id);
        if (status!=INTEL_BCS_OK) return status;
        status=indexed ? intel_bcs_context_retarget_indexed(&s->native,c->pid,c->epoch,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,vertex_program,indices,textures,texture_count) :
            vertex ? intel_bcs_context_retarget_vertex(&s->native,c->pid,c->epoch,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,vertex_program,textures,texture_count) :
            program ? intel_bcs_context_retarget_sampled(&s->native,c->pid,c->epoch,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,textures,texture_count) :
            intel_bcs_context_retarget(&s->native,c->pid,c->epoch,capture->resources[0].native.buffer,capture->resources[1].native.buffer);
        if (status==INTEL_BCS_BUSY) { retry=true; continue; }
        if (status!=INTEL_BCS_OK) return status;
        s->pid=c->pid; s->device=c->device; s->context=c->id;
        intel_gpu_service_notify(NULL); /* old residency may now be reclaimed */
        status=bcs_owner_capture_fences(s,capture);
        if (status==INTEL_BCS_OK) *out=&s->native;
        return status;
    }
    for (unsigned i=0;i<o->count;i++) {
        intel_gpu_bcs_slot_t *s=&o->slots[i];
        if (s->pid || !s->native.reserved) continue;
        enum intel_bcs_status status=indexed ? intel_bcs_context_activate_indexed(&s->native,c->pid,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,vertex_program,indices,textures,texture_count) :
            vertex ? intel_bcs_context_activate_vertex(&s->native,c->pid,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,vertex_program,textures,texture_count) :
            program ? intel_bcs_context_activate_sampled(&s->native,c->pid,
            capture->resources[0].native.buffer,capture->resources[1].native.buffer,program,textures,texture_count) :
            intel_bcs_context_activate(&s->native,c->pid,capture->resources[0].native.buffer,capture->resources[1].native.buffer);
        if (status!=INTEL_BCS_OK) return status;
        s->native.epoch=c->epoch;
        if (!intel_bcs_context_attach(&s->native,&o->queue,s)) return INTEL_BCS_QUARANTINED;
        s->pid=c->pid; s->device=c->device; s->context=c->id;
        status=bcs_owner_capture_fences(s,capture);
        if (status==INTEL_BCS_OK) *out=&s->native;
        return status;
    }
    return retry ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
}
NTSTATUS intel_gpu_bcs_owner_init(intel_gpu_bcs_owner_t *o,intel_gpu_jobs_t *jobs,const intel_gpu_bcs_config_t *c)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!o || o->self || !jobs || jobs->self!=jobs || jobs->closing || jobs->ready!=intel_gpu_executor_notify ||
        !c || !c->space || !c->space->ready || !c->allocator.allocate || !c->allocator.validate ||
        !c->allocator.release || !c->allocator.sync || !c->io.read || !c->io.write ||
        !c->io.now_ns || !c->io.relax || !c->token_read || !c->mapping_stopped ||
        !c->first_fence || c->first_fence==UINT64_MAX || !c->first_hardware_id ||
        c->first_hardware_id>(1u<<21)-INTEL_BCS_QUEUE_CONTEXTS || !c->ggtt_limit ||
        (c->render && (!c->batch_gpu || (c->batch_gpu&4095) || c->batch_gpu>=INTEL_PPGTT_VA_LIMIT)) ||
        (c->program_gpu && (!c->render || (c->program_gpu&4095) || c->program_gpu>=INTEL_PPGTT_VA_LIMIT ||
            c->program_gpu==c->source_gpu || c->program_gpu==c->destination_gpu ||
            (c->program_gpu>=c->batch_gpu && c->program_gpu-c->batch_gpu<INTEL_RCS_SAMPLED_BYTES))) ||
        (c->ggtt_limit&4095) || !c->source_gpu || !c->destination_gpu ||
        ((c->source_gpu|c->destination_gpu)&4095) || c->source_gpu==c->destination_gpu)
        return STATUS_INVALID_PARAMETER;
    o->self=o; o->jobs=jobs; o->config=*c;
    for (unsigned i=0;i<INTEL_BCS_QUEUE_CONTEXTS;i++) o->slots[i].owner=o;
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_bcs_owner_reserve(intel_gpu_bcs_owner_t *o,unsigned slots,unsigned table_pages)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!o || o->self!=o || o->count || o->starting || o->closing ||
        !slots || slots>INTEL_BCS_QUEUE_CONTEXTS || !table_pages || table_pages>512)
        return STATUS_INVALID_PARAMETER;
    for (unsigned i=0;i<slots;i++) {
        o->count++; /* close retains every partial reservation */
        bool reserved=o->config.program_gpu ? intel_bcs_context_reserve_sampled(&o->slots[i].native,o->config.space,
            &o->config.allocator,o->config.allocator_context,1,o->config.first_hardware_id+i,
            o->config.source_gpu,o->config.destination_gpu,table_pages,o->config.ggtt_limit,o->config.batch_gpu,o->config.program_gpu) :
            o->config.render ? intel_bcs_context_reserve_render(&o->slots[i].native,o->config.space,
            &o->config.allocator,o->config.allocator_context,1,o->config.first_hardware_id+i,
            o->config.source_gpu,o->config.destination_gpu,table_pages,o->config.ggtt_limit,o->config.batch_gpu) :
            intel_bcs_context_reserve(&o->slots[i].native,o->config.space,
            &o->config.allocator,o->config.allocator_context,1,o->config.first_hardware_id+i,
            o->config.source_gpu,o->config.destination_gpu,table_pages,o->config.ggtt_limit);
        if (!reserved) {
            o->closing=true; return STATUS_UNSUCCESSFUL;
        }
    }
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_bcs_owner_prepare(intel_gpu_bcs_owner_t *o,const intel_gpu_capture_t *capture)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!o || o->self!=o || o->starting || o->closing || o->config.render || !capture || capture->self!=capture || !capture->valid ||
        capture->context.native.owner!=o->jobs->contexts) return STATUS_INVALID_PARAMETER;
    const intel_gpu_context_lease_t *c=&capture->context.native;
    for (unsigned i=0;i<o->count;i++) if (o->slots[i].pid==c->pid &&
        o->slots[i].device==c->device && o->slots[i].context==c->id) return STATUS_INVALID_PARAMETER;
    if (o->count==INTEL_BCS_QUEUE_CONTEXTS) return STATUS_INSUFFICIENT_RESOURCES;
    intel_gpu_bcs_slot_t *s=&o->slots[o->count++];
    s->native.epoch=c->epoch;
    s->pid=c->pid; s->device=c->device; s->context=c->id;
    if (!intel_bcs_context_create(&s->native,o->config.space,&o->config.allocator,
        o->config.allocator_context,c->pid,o->config.first_hardware_id+o->count-1,
        capture->resources[0].native.buffer,o->config.source_gpu,
        capture->resources[1].native.buffer,o->config.destination_gpu,o->config.ggtt_limit)) {
        o->closing=true; return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_bcs_owner_start(intel_gpu_bcs_owner_t *o)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!o || o->self!=o || !o->count || o->starting || o->closing) return STATUS_INVALID_PARAMETER;
    o->starting=true;
    if (!intel_gpu_memory_watch_mappings(o->jobs->contexts->memory,intel_gpu_executor_notify)) goto fail;
    if (!intel_gpu_contexts_watch_mappings(o->jobs->contexts,intel_gpu_executor_notify)) goto fail;
    if (o->jobs->contexts->fences &&
        !intel_gpu_fences_watch_mappings(o->jobs->contexts->fences,intel_gpu_executor_notify)) goto fail;
    if (intel_gpu_buffer_create(&o->hwsp,&o->config.allocator,o->config.allocator_context,1,4096)!=INTEL_GPU_BUFFER_OK)
        goto fail;
    memset((uint8_t *)o->hwsp.cpu+0x370,0xff,6*8);
    if (intel_gpu_buffer_sync(&o->hwsp,1)!=INTEL_GPU_BUFFER_OK ||
        intel_ggtt_bind(o->config.space,&o->hwsp_binding,&o->hwsp.dma,1,1,
            o->config.ggtt_limit,&o->hwsp_original)!=INTEL_GGTT_BIND_OK ||
        intel_ggtt_pin(o->config.space,&o->hwsp_binding,o->hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK) goto fail;
    o->hwsp_pinned=true;
    if (!intel_ppgtt_pat_acquire(&bcs_owner_io,&o->slots[0],&o->pat)) goto fail;
    if (o->jobs->contexts->fences &&
        !intel_ppgtt_coherent_pat_acquire(&bcs_owner_io,&o->slots[0],&o->coherent_pat)) goto fail;
    if (o->config.render && !intel_rcs_cache_acquire(&bcs_owner_io,&o->slots[0],&o->cache)) goto fail;
    o->config.mapping_stopped(o->config.hardware_context,false);
    if (intel_cs_enable(o->config.render ? INTEL_CS_RENDER : INTEL_CS_COPY,
        &bcs_owner_io,&o->slots[0],o->hwsp_binding.gpu_address,&o->engine)!=INTEL_CS_OK) goto fail;
    const intel_bcs_queue_ops_t ops={bcs_owner_prepare,bcs_owner_token,bcs_owner_acquire,bcs_owner_retire};
    enum intel_bcs_status queue=o->config.program_gpu ?
        intel_bcs_queue_init_draw(&o->queue,&o->engine,&bcs_owner_io,&o->slots[0],&ops,bcs_owner_prepare_draw,o->config.first_fence) :
        intel_bcs_queue_init(&o->queue,&o->engine,&bcs_owner_io,&o->slots[0],&ops,o->config.first_fence);
    if (queue!=INTEL_BCS_OK) goto fail;
    for (unsigned i=0;i<o->count;i++) if (!o->slots[i].native.reserved &&
        !intel_bcs_context_attach(&o->slots[i].native,&o->queue,&o->slots[i])) goto fail;
    if (intel_bcs_queue_publish(&o->queue)!=INTEL_BCS_OK ||
        intel_gpu_executor_attach_managed(o->jobs,&o->queue,bcs_owner_bind,
            bcs_owner_maintain,o,false,&o->executor)!=0) goto fail;
    return STATUS_SUCCESS;
fail:
    o->closing=true; return STATUS_UNSUCCESSFUL;
}
NTSTATUS intel_gpu_bcs_owner_close(intel_gpu_bcs_owner_t *o)
{
    if (smp_current_cpu()!=0) return STATUS_NOT_SUPPORTED;
    if (!o || o->self!=o) return STATUS_INVALID_PARAMETER;
    if (o->closed) return STATUS_SUCCESS;
    o->closing=true;
    if (o->executor) {
        NTSTATUS status=intel_gpu_executor_stop(o->executor);
        if (status!=0) return status;
        status=intel_gpu_executor_detach(o->executor);
        if (status!=0) return status;
        o->executor=0;
    }
    if (o->queue.initialized) {
        enum intel_bcs_status status=intel_bcs_queue_shutdown(&o->queue);
        if (status!=INTEL_BCS_OK) return status==INTEL_BCS_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
        if (intel_bcs_queue_unwatch(&o->queue)!=INTEL_BCS_OK) return D3DKMT_STATUS_DEVICE_BUSY;
        if (o->queue.registered && intel_bcs_queue_unpublish(&o->queue)!=INTEL_BCS_OK) return D3DKMT_STATUS_DEVICE_BUSY;
    } else if (o->engine.started && intel_cs_close(&bcs_owner_io,&o->slots[0],&o->engine)!=INTEL_CS_OK)
        return STATUS_UNSUCCESSFUL;
    if (o->engine.referenced && !o->engine.retired) return STATUS_UNSUCCESSFUL;
    o->config.mapping_stopped(o->config.hardware_context,true);
    if (o->cache.started && !intel_rcs_cache_restore(&bcs_owner_io,&o->slots[0],&o->cache)) return STATUS_UNSUCCESSFUL;
    if (o->pat.started && !intel_ppgtt_pat_restore(&bcs_owner_io,&o->slots[0],&o->pat)) return STATUS_UNSUCCESSFUL;
    bool released=true;
    for (unsigned i=0;i<o->count;i++) if (!intel_bcs_context_destroy(&o->slots[i].native)) released=false;
    intel_gpu_service_notify(NULL);
    if (!released) return STATUS_UNSUCCESSFUL;
    for (unsigned i=0;i<o->count;i++) for (unsigned j=0;j<INTEL_BCS_COHERENT_RANGES;j++) {
        enum intel_bcs_status status=intel_gpu_fence_unmap(&o->slots[i].fences[j]);
        if (status!=INTEL_BCS_OK) return status==INTEL_BCS_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    }
    if (o->coherent_pat.started && !intel_ppgtt_coherent_pat_restore(&bcs_owner_io,&o->slots[0],&o->coherent_pat))
        return STATUS_UNSUCCESSFUL;
    if (o->hwsp_pinned) {
        if (intel_ggtt_unpin(o->config.space,&o->hwsp_binding,o->hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK)
            return STATUS_UNSUCCESSFUL;
        o->hwsp_pinned=false;
    }
    if (o->hwsp_binding.phase!=INTEL_GGTT_EMPTY &&
        intel_ggtt_unbind(o->config.space,&o->hwsp_binding,o->hwsp_binding.cookie)!=INTEL_GGTT_BIND_OK) return STATUS_UNSUCCESSFUL;
    if (o->hwsp.started && intel_gpu_buffer_close(&o->hwsp,1)!=INTEL_GPU_BUFFER_OK) return STATUS_UNSUCCESSFUL;
    o->closed=true;
    return STATUS_SUCCESS;
}
