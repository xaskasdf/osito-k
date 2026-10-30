#include "intel_bcs_context.h"
#include "intel_rcs_sampled.h"

bool intel_bcs_context_create(intel_bcs_context_t *c, intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a, void *allocator_context, uint64_t owner,
    uint32_t hardware_id, intel_gpu_buffer_t *source, uint64_t source_gpu,
    intel_gpu_buffer_t *destination, uint64_t destination_gpu, uint64_t ggtt_limit)
{
    if (!c || c->started || !space || !a || !owner || !hardware_id || hardware_id>=(1u<<21) ||
        !source || !destination || source==destination) return false;
    c->started=true; c->owner=owner; c->space=space; c->hardware_id=hardware_id;
    c->source=source; c->destination=destination;
    c->source_gpu=source_gpu; c->destination_gpu=destination_gpu;
    if (intel_gpu_buffer_reside(source,owner)!=INTEL_GPU_BUFFER_OK) return false;
    c->source_pinned=true;
    if (intel_gpu_buffer_reside(destination,owner)!=INTEL_GPU_BUFFER_OK) return false;
    c->destination_pinned=true;
    uint64_t source_bytes=(uint64_t)source->pages*4096;
    uint64_t destination_bytes=(uint64_t)destination->pages*4096;
    if (source->dma<destination->dma+destination_bytes && destination->dma<source->dma+source_bytes)
        return false;
    intel_ppgtt_range_t ranges[2]={
        {source_gpu,source->dma,source_bytes,false},
        {destination_gpu,destination->dma,destination_bytes,true}
    };
    if (ranges[0].gpu>ranges[1].gpu) {
        intel_ppgtt_range_t temp=ranges[0]; ranges[0]=ranges[1]; ranges[1]=temp;
    }
    if (!intel_ppgtt_range_pages(ranges,2,&c->table_pages) ||
        intel_gpu_buffer_create(&c->tables,a,allocator_context,owner,
            (uint64_t)c->table_pages*4096)!=INTEL_GPU_BUFFER_OK ||
        !intel_ppgtt_build_ranges(c->tables.cpu,c->tables.bytes,c->tables.dma,ranges,2,&c->table_pages) ||
        intel_gpu_buffer_sync(&c->tables,owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_create(&c->state,a,allocator_context,owner,3*4096)!=INTEL_GPU_BUFFER_OK)
        return false;
    uint64_t pages[3]={c->state.dma,c->state.dma+4096,c->state.dma+8192};
    if (intel_ggtt_bind(space,&c->binding,pages,3,1,ggtt_limit,c->originals)!=INTEL_GGTT_BIND_OK)
        return false;
    if (intel_ggtt_pin(space,&c->binding,c->binding.cookie)!=INTEL_GGTT_BIND_OK) return false;
    c->ggtt_pinned=true; c->ready=true;
    return true;
}

bool intel_bcs_context_attach(intel_bcs_context_t *c, intel_bcs_queue_t *q, void *backing)
{
    if (!c || !c->ready || c->queue || !q) return false;
    if (intel_bcs_queue_attach_epoch(q,c->owner,c->epoch,backing?backing:c,&c->queue_id)!=INTEL_BCS_OK)
        return false;
    c->queue=q; c->queue_owner=c->owner;
    return true;
}

static bool bcs_context_drop_retained(intel_bcs_context_t *c)
{
    uint64_t owner=c->retained_owner ? c->retained_owner : c->owner;
    if (c->retained_source) {
        if (intel_gpu_buffer_unreside(c->retained_source,owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_source=NULL;
    }
    if (c->retained_destination) {
        if (intel_gpu_buffer_unreside(c->retained_destination,owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_destination=NULL;
    }
    if (c->retained_program) {
        if (intel_gpu_buffer_unreside(c->retained_program,owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_program=NULL;
    }
    if (c->retained_vertex_program) {
        if (intel_gpu_buffer_unreside(c->retained_vertex_program,owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_vertex_program=NULL;
    }
    if (c->retained_indices) {
        if (intel_gpu_buffer_unreside(c->retained_indices,owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_indices=NULL;
    }
    for (unsigned i=0;i<c->retained_texture_count;i++) if (c->retained_textures[i]) {
        if (intel_gpu_buffer_unreside(c->retained_textures[i],owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->retained_textures[i]=NULL;
    }
    c->retained_texture_count=0;
    c->retained_owner=0;
    return true;
}

static bool bcs_context_reserve_engine(intel_bcs_context_t *c,intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a,void *allocator_context,uint64_t storage_owner,
    uint32_t hardware_id,uint64_t source_gpu,uint64_t destination_gpu,
    uint32_t table_pages,uint64_t ggtt_limit,uint64_t batch_gpu,uint32_t batch_bytes)
{
    if (!c || c->started || !space || !a || !storage_owner || !hardware_id ||
        hardware_id>=(1u<<21) || !source_gpu || !destination_gpu ||
        source_gpu==destination_gpu || ((source_gpu|destination_gpu)&4095) ||
        source_gpu>=INTEL_PPGTT_VA_LIMIT || destination_gpu>=INTEL_PPGTT_VA_LIMIT ||
        !table_pages || table_pages>512 ||
        (batch_gpu && (batch_bytes!=INTEL_RCS_DRAW_BYTES && batch_bytes!=INTEL_RCS_SAMPLED_BYTES)) ||
        (batch_gpu && ((batch_gpu&4095) || batch_gpu>INTEL_PPGTT_VA_LIMIT-batch_bytes ||
         (source_gpu>=batch_gpu && source_gpu<batch_gpu+batch_bytes) ||
         (destination_gpu>=batch_gpu && destination_gpu<batch_gpu+batch_bytes)))) return false;
    c->started=true; c->space=space; c->hardware_id=hardware_id;
    c->source_gpu=source_gpu; c->destination_gpu=destination_gpu;
    c->render=batch_gpu!=0; c->batch_gpu=batch_gpu;
    if (c->render && intel_gpu_buffer_create(&c->batch,a,allocator_context,storage_owner,
        batch_bytes)!=INTEL_GPU_BUFFER_OK) return false;
    intel_ppgtt_range_t batch_range={batch_gpu,c->batch.dma,c->batch.bytes,false};
    if (intel_gpu_buffer_create(&c->tables,a,allocator_context,storage_owner,
            (uint64_t)table_pages*4096)!=INTEL_GPU_BUFFER_OK ||
        !intel_ppgtt_build_ranges(c->tables.cpu,c->tables.bytes,c->tables.dma,
            c->render ? &batch_range : NULL,c->render ? 1 : 0,&c->table_pages) ||
        intel_gpu_buffer_sync(&c->tables,storage_owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_create(&c->state,a,allocator_context,storage_owner,(c->render ? 23 : 3)*4096)!=INTEL_GPU_BUFFER_OK)
        return false;
    uint64_t pages[23]; unsigned count=c->render ? 23 : 3;
    for (unsigned i=0;i<count;i++) pages[i]=c->state.dma+(uint64_t)i*4096;
    if (intel_ggtt_bind(space,&c->binding,pages,count,1,ggtt_limit,c->originals)!=INTEL_GGTT_BIND_OK ||
        intel_ggtt_pin(space,&c->binding,c->binding.cookie)!=INTEL_GGTT_BIND_OK) return false;
    c->ggtt_pinned=true; c->reserved=true;
    return true;
}

bool intel_bcs_context_reserve(intel_bcs_context_t *c,intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a,void *allocator_context,uint64_t storage_owner,
    uint32_t hardware_id,uint64_t source_gpu,uint64_t destination_gpu,
    uint32_t table_pages,uint64_t ggtt_limit)
{
    return bcs_context_reserve_engine(c,space,a,allocator_context,storage_owner,hardware_id,
        source_gpu,destination_gpu,table_pages,ggtt_limit,0,0);
}
bool intel_bcs_context_reserve_render(intel_bcs_context_t *c,intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a,void *allocator_context,uint64_t storage_owner,
    uint32_t hardware_id,uint64_t source_gpu,uint64_t destination_gpu,
    uint32_t table_pages,uint64_t ggtt_limit,uint64_t batch_gpu)
{
    if (!batch_gpu) return false;
    return bcs_context_reserve_engine(c,space,a,allocator_context,storage_owner,hardware_id,
        source_gpu,destination_gpu,table_pages,ggtt_limit,batch_gpu,INTEL_RCS_DRAW_BYTES);
}
bool intel_bcs_context_reserve_draw(intel_bcs_context_t *c,intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a,void *allocator_context,uint64_t storage_owner,
    uint32_t hardware_id,uint64_t source_gpu,uint64_t destination_gpu,
    uint32_t table_pages,uint64_t ggtt_limit,uint64_t batch_gpu,uint64_t program_gpu)
{
    if (!program_gpu || (program_gpu&4095) || program_gpu>=INTEL_PPGTT_VA_LIMIT ||
        program_gpu==source_gpu || program_gpu==destination_gpu ||
        (program_gpu>=batch_gpu && program_gpu-batch_gpu<INTEL_RCS_DRAW_BYTES)) return false;
    if (!intel_bcs_context_reserve_render(c,space,a,allocator_context,storage_owner,
        hardware_id,source_gpu,destination_gpu,table_pages,ggtt_limit,batch_gpu)) return false;
    c->program_gpu=program_gpu;
    return true;
}
bool intel_bcs_context_reserve_sampled(intel_bcs_context_t *c,intel_ggtt_space_t *space,
    const intel_gpu_allocator_t *a,void *allocator_context,uint64_t storage_owner,
    uint32_t hardware_id,uint64_t source_gpu,uint64_t destination_gpu,
    uint32_t table_pages,uint64_t ggtt_limit,uint64_t batch_gpu,uint64_t program_gpu)
{
    if (!batch_gpu || !program_gpu || (program_gpu&4095) || program_gpu>=INTEL_PPGTT_VA_LIMIT ||
        program_gpu==source_gpu || program_gpu==destination_gpu ||
        (program_gpu>=batch_gpu && program_gpu-batch_gpu<INTEL_RCS_SAMPLED_BYTES)) return false;
    if (!bcs_context_reserve_engine(c,space,a,allocator_context,storage_owner,hardware_id,
        source_gpu,destination_gpu,table_pages,ggtt_limit,batch_gpu,INTEL_RCS_SAMPLED_BYTES)) return false;
    c->program_gpu=program_gpu;
    return true;
}
static bool bcs_context_drop_coherent(intel_bcs_coherent_range_t *mapping)
{
    if (mapping->resident) {
        intel_gpu_coherent_access_t *access=mapping->reference.access;
        if (intel_gpu_buffer_unreside(access->buffer,access->owner)!=INTEL_GPU_BUFFER_OK) return false;
        mapping->resident=false;
    }
    if (mapping->reference.self &&
        intel_gpu_buffer_drop_coherent(&mapping->reference)!=INTEL_GPU_BUFFER_OK) return false;
    *mapping=(intel_bcs_coherent_range_t){0};
    return true;
}

static bool bcs_context_vertex_valid(const intel_bcs_context_t *c,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!vertex.buffer) return !vertex.gpu;
    intel_gpu_buffer_t *b=vertex.buffer;
    if (!program || !c->render || c->batch.bytes!=INTEL_RCS_SAMPLED_BYTES ||
        b==src || b==dst || b==program || b==&c->state || b==&c->tables || b==&c->batch ||
        !vertex.gpu || (vertex.gpu&4095) || vertex.gpu>=INTEL_PPGTT_VA_LIMIT ||
        count>INTEL_BCS_TEXTURES || (count && !textures)) return false;
    for (unsigned i=0;i<count;i++) if (textures[i].buffer==b) return false;
    return true;
}

static bool bcs_context_indices_valid(const intel_bcs_context_t *c,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!indices.buffer) return !indices.gpu;
    if (indices.buffer==vertex.buffer) return false;
    return bcs_context_vertex_valid(c,src,dst,program,
        (intel_bcs_program_binding_t){indices.buffer,indices.gpu},textures,count);
}

static bool bcs_context_textures_valid(const intel_bcs_context_t *c,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (count>INTEL_BCS_TEXTURES || (count && (!textures || !program || !c->render ||
        c->batch.bytes!=INTEL_RCS_SAMPLED_BYTES))) return false;
    for (unsigned i=0;i<count;i++) {
        intel_gpu_buffer_t *b=textures[i].buffer;
        if (!b || b==src || b==dst || b==program || b==&c->state || b==&c->tables || b==&c->batch ||
            !textures[i].gpu || (textures[i].gpu&4095) || textures[i].gpu>=INTEL_PPGTT_VA_LIMIT) return false;
        for (unsigned j=0;j<i;j++) if (b==textures[j].buffer) return false;
    }
    return true;
}
static enum intel_bcs_status bcs_context_pin_textures(intel_bcs_context_t *c,uint64_t owner,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    for (unsigned i=0;i<count;i++) {
        enum intel_gpu_buffer_status pin=intel_gpu_buffer_reside(textures[i].buffer,owner);
        if (pin!=INTEL_GPU_BUFFER_OK) return pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        c->retained_textures[c->retained_texture_count++]=textures[i].buffer;
    }
    return INTEL_BCS_OK;
}
/* All new pins already exist. After publication, retained[] takes the old
 * pins until retirement succeeds; even identical replacements balance both. */
static void bcs_context_install_textures(intel_bcs_context_t *c,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    for (unsigned i=0;i<INTEL_BCS_TEXTURES;i++) {
        c->retained_textures[i]=i<c->texture_count ? c->textures[i].buffer : NULL;
        c->textures[i]=i<count ? textures[i] : (intel_bcs_texture_binding_t){0};
    }
    c->retained_texture_count=c->texture_count; c->texture_count=count;
}
static bool bcs_context_map_surfaces(intel_bcs_context_t *c,intel_gpu_buffer_t *src,
    intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned texture_count,intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices,bool keep_coherent)
{
    _Static_assert(6+INTEL_BCS_TEXTURES+INTEL_BCS_COHERENT_RANGES<=INTEL_PPGTT_MAX_RANGES,"native mapping capacity");
    intel_ppgtt_range_t ranges[INTEL_PPGTT_MAX_RANGES]={{c->source_gpu,src->dma,(uint64_t)src->pages*4096,false},
        {c->destination_gpu,dst->dma,(uint64_t)dst->pages*4096,true},
        {c->batch_gpu,c->batch.dma,c->batch.bytes,false}};
    unsigned count=c->render ? 3 : 2;
    if (program) {
        if (!c->render || !c->program_gpu) return false;
        ranges[count++]=(intel_ppgtt_range_t){c->program_gpu,program->dma,(uint64_t)program->pages*4096,false};
    }
    if (vertex.buffer)
        ranges[count++]=(intel_ppgtt_range_t){vertex.gpu,vertex.buffer->dma,(uint64_t)vertex.buffer->pages*4096,false};
    if (indices.buffer)
        ranges[count++]=(intel_ppgtt_range_t){indices.gpu,indices.buffer->dma,(uint64_t)indices.buffer->pages*4096,false};
    for (unsigned i=0;i<texture_count;i++) {
        const intel_gpu_buffer_t *b=textures[i].buffer;
        ranges[count++]=(intel_ppgtt_range_t){textures[i].gpu,b->dma,(uint64_t)b->pages*4096,false};
    }
    uint64_t coherent_mask=0;
    if (keep_coherent) for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        intel_bcs_coherent_range_t *mapping=&c->coherent[i];
        if (!mapping->mapped) continue;
        intel_gpu_coherent_access_t *access=mapping->reference.access;
        if (mapping->reference.self!=&mapping->reference || !mapping->resident ||
            !access || access->self!=access || access->owner!=c->owner) return false;
        coherent_mask|=1ULL<<count;
        ranges[count++]=(intel_ppgtt_range_t){mapping->gpu,access->dma,access->bytes,true};
    }
    for (unsigned i=0;i<count;i++) for (unsigned j=0;j<i;j++) {
        if (ranges[i].dma<ranges[j].dma+ranges[j].bytes &&
            ranges[j].dma<ranges[i].dma+ranges[i].bytes) return false;
    }
    for (unsigned i=0;i<count;i++) for (unsigned j=0;j<i;j++) {
        if (ranges[i].gpu<ranges[j].gpu) {
            intel_ppgtt_range_t temp=ranges[i]; ranges[i]=ranges[j]; ranges[j]=temp;
            if (((coherent_mask>>i)^(coherent_mask>>j))&1u)
                coherent_mask^=(1ULL<<i)|(1ULL<<j);
        }
    }
    return intel_ppgtt_build_ranges_coherent(c->tables.cpu,c->tables.bytes,c->tables.dma,
        ranges,count,coherent_mask,&c->table_pages);
}

typedef struct {
    intel_bcs_context_t *context;
    intel_gpu_coherent_access_t *access;
    uint64_t gpu;
} bcs_context_coherent_update_t;

static enum intel_bcs_status bcs_context_coherent_update(void *backing,void *argument,
    const intel_gt_io_t *io,intel_bcs_state_t *engine)
{
    bcs_context_coherent_update_t *update=argument;
    intel_bcs_context_t *c=update->context;
    intel_gpu_coherent_access_t *access=update->access;
    if (!c->ready || c->access_token || c->retained_source || c->retained_destination ||
        c->retained_program || c->retained_vertex_program || c->retained_indices || c->retained_texture_count || c->rebindings==UINT64_MAX) return INTEL_BCS_INVALID;
    intel_bcs_coherent_range_t *found=NULL,*empty=NULL;
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++) {
        intel_bcs_coherent_range_t *m=&c->coherent[i];
        if (m->reference.self && m->gpu==update->gpu) found=m;
        if (!m->reference.self && !m->resident && !empty) empty=m;
    }
    intel_bcs_coherent_range_t *mapping=access ? empty : found;
    if (!mapping || (access && found) || (!access && !mapping->mapped)) return INTEL_BCS_INVALID;
    if (access) {
        if (access->self!=access || access->owner!=c->owner) return INTEL_BCS_INVALID;
        enum intel_gpu_buffer_status result=intel_gpu_buffer_retain_coherent(access,&mapping->reference);
        if (result!=INTEL_GPU_BUFFER_OK)
            return result==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        result=intel_gpu_buffer_reside(access->buffer,access->owner);
        if (result!=INTEL_GPU_BUFFER_OK) {
            if (!bcs_context_drop_coherent(mapping)) goto quarantine;
            return result==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        }
        mapping->resident=true; mapping->gpu=update->gpu;
    }
    mapping->mapped=access!=NULL;
    if (!bcs_context_map_surfaces(c,c->source,c->destination,c->program,c->textures,c->texture_count,c->vertex_program,c->indices,true)) {
        if (access) {
            if (!bcs_context_drop_coherent(mapping)) goto quarantine;
        } else mapping->mapped=true;
        return INTEL_BCS_INVALID;
    }
    /* Queue update admission proves there is no outstanding work. Release no
     * old backing until BOTH cache publication and hardware TLB ack succeed. */
    if (intel_gpu_buffer_sync(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK ||
        intel_cs_invalidate_tlb(io,backing,engine)!=INTEL_CS_OK) goto quarantine;
    c->rebindings++;
    if (!access && !bcs_context_drop_coherent(mapping)) goto quarantine;
    return INTEL_BCS_OK;
quarantine:
    c->ready=false;
    return INTEL_BCS_QUARANTINED;
}

enum intel_bcs_status intel_bcs_context_bind_coherent(intel_bcs_context_t *c,
    intel_gpu_coherent_access_t *access,uint64_t gpu,const intel_ppgtt_pat_t *policy)
{
    if (!c || !c->queue || !access || !policy || !policy->started || !policy->ready ||
        !gpu || (gpu&4095) || gpu>=INTEL_PPGTT_VA_LIMIT) return INTEL_BCS_INVALID;
    bcs_context_coherent_update_t update={c,access,gpu};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_coherent_update,&update);
}

enum intel_bcs_status intel_bcs_context_unbind_coherent(intel_bcs_context_t *c,uint64_t gpu)
{
    if (!c || !c->queue || !gpu || (gpu&4095)) return INTEL_BCS_INVALID;
    bcs_context_coherent_update_t update={c,NULL,gpu};
    return intel_bcs_queue_remove_context_mappings(c->queue,c->owner,c->queue_id,bcs_context_coherent_update,&update);
}

static enum intel_bcs_status bcs_context_activate(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned texture_count,intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices)
{
    if (!c || !c->reserved || !c->started || c->ready || c->owner || c->queue ||
        !c->ggtt_pinned || !owner || !src || !dst || src==dst ||
        src==&c->tables || dst==&c->tables || src==&c->state || dst==&c->state || src==&c->batch || dst==&c->batch ||
        (program && (!c->render || !c->program_gpu || program==src || program==dst ||
         program==&c->tables || program==&c->state || program==&c->batch)) ||
        !bcs_context_textures_valid(c,src,dst,program,textures,texture_count) ||
        !bcs_context_vertex_valid(c,src,dst,program,vertex,textures,texture_count) ||
        !bcs_context_indices_valid(c,src,dst,program,vertex,indices,textures,texture_count))
        return INTEL_BCS_INVALID;
    c->owner=owner;
    enum intel_gpu_buffer_status pin=intel_gpu_buffer_reside(src,owner);
    enum intel_bcs_status result=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    if (pin!=INTEL_GPU_BUFFER_OK) { c->owner=0; return result; }
    c->retained_source=src;
    pin=intel_gpu_buffer_reside(dst,owner);
    result=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
    c->retained_destination=dst;
    if (program) {
        pin=intel_gpu_buffer_reside(program,owner);
        result=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_program=program;
    }
    if (vertex.buffer) {
        pin=intel_gpu_buffer_reside(vertex.buffer,owner);
        result=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_vertex_program=vertex.buffer;
    }
    if (indices.buffer) {
        pin=intel_gpu_buffer_reside(indices.buffer,owner);
        result=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_indices=indices.buffer;
    }
    result=bcs_context_pin_textures(c,owner,textures,texture_count);
    if (result!=INTEL_BCS_OK) goto reject;
    result=INTEL_BCS_INVALID;
    uint64_t sb=(uint64_t)src->pages*4096,db=(uint64_t)dst->pages*4096;
    if (src->dma<dst->dma+db && dst->dma<src->dma+sb) goto reject;
    /* The hardware has never received this descriptor. This operation changes
     * only its private PPGTT arena; all GGTT addresses remain fixed. */
    if (!bcs_context_map_surfaces(c,src,dst,program,textures,texture_count,vertex,indices,true)) goto reject;
    if (intel_gpu_buffer_sync(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK) goto quarantine;
    c->source=src; c->destination=dst; c->source_pinned=c->destination_pinned=true;
    c->program=program; c->program_pinned=program!=NULL; c->retained_program=NULL;
    c->vertex_program=vertex;c->vertex_program_pinned=vertex.buffer!=NULL;c->retained_vertex_program=NULL;
    c->indices=indices;c->indices_pinned=indices.buffer!=NULL;c->retained_indices=NULL;
    bcs_context_install_textures(c,textures,texture_count);
    c->retained_source=c->retained_destination=NULL;
    c->reserved=false; c->ready=true;
    return INTEL_BCS_OK;
reject:
    if (bcs_context_drop_retained(c)) { c->owner=0; return result; }
quarantine:
    c->reserved=false;
    return INTEL_BCS_QUARANTINED;
}
enum intel_bcs_status intel_bcs_context_activate(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst)
{ return bcs_context_activate(c,owner,src,dst,NULL,NULL,0,(intel_bcs_program_binding_t){0},(intel_bcs_index_binding_t){0}); }
enum intel_bcs_status intel_bcs_context_activate_draw(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program)
{ return program ? bcs_context_activate(c,owner,src,dst,program,NULL,0,(intel_bcs_program_binding_t){0},(intel_bcs_index_binding_t){0}) : INTEL_BCS_INVALID; }

enum intel_bcs_status intel_bcs_context_activate_sampled(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *src,intel_gpu_buffer_t *dst,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{ return program ? bcs_context_activate(c,owner,src,dst,program,textures,count,(intel_bcs_program_binding_t){0},(intel_bcs_index_binding_t){0}) : INTEL_BCS_INVALID; }

typedef struct {
    intel_bcs_context_t *context;
    intel_gpu_buffer_t *source, *destination, *program;
    const intel_bcs_texture_binding_t *textures;
    unsigned texture_count;
    intel_bcs_program_binding_t vertex;
    intel_bcs_index_binding_t indices;
} bcs_context_update_t;

static enum intel_bcs_status bcs_context_update(void *backing, void *argument,
    const intel_gt_io_t *io, intel_bcs_state_t *engine)
{
    bcs_context_update_t *u=argument;
    intel_bcs_context_t *c=u->context;
    intel_gpu_buffer_t *src=u->source,*dst=u->destination,*program=u->program;
    if (!c->ready || c->access_token || c->retained_source || c->retained_destination || c->retained_program || c->retained_vertex_program || c->retained_indices || c->retained_texture_count ||
        !src || !dst || src==dst || src==&c->state || src==&c->tables ||
        dst==&c->state || dst==&c->tables || src==&c->batch || dst==&c->batch || c->rebindings==UINT64_MAX ||
        (program && (!c->render || !c->program_gpu || program==src || program==dst ||
         program==&c->tables || program==&c->state || program==&c->batch)) ||
        !bcs_context_textures_valid(c,src,dst,program,u->textures,u->texture_count) ||
        !bcs_context_vertex_valid(c,src,dst,program,u->vertex,u->textures,u->texture_count) ||
        !bcs_context_indices_valid(c,src,dst,program,u->vertex,u->indices,u->textures,u->texture_count))
        return INTEL_BCS_INVALID;
    enum intel_gpu_buffer_status pin=intel_gpu_buffer_reside(src,c->owner);
    if (pin!=INTEL_GPU_BUFFER_OK)
        return pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    c->retained_source=src;
    pin=intel_gpu_buffer_reside(dst,c->owner);
    enum intel_bcs_status rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
    c->retained_destination=dst;
    if (program) {
        pin=intel_gpu_buffer_reside(program,c->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_program=program;
    }
    if (u->vertex.buffer) {
        pin=intel_gpu_buffer_reside(u->vertex.buffer,c->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_vertex_program=u->vertex.buffer;
    }
    if (u->indices.buffer) {
        pin=intel_gpu_buffer_reside(u->indices.buffer,c->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_indices=u->indices.buffer;
    }
    rejected=bcs_context_pin_textures(c,c->owner,u->textures,u->texture_count);
    if (rejected!=INTEL_BCS_OK) goto reject;
    rejected=INTEL_BCS_INVALID;
    uint64_t sb=(uint64_t)src->pages*4096,db=(uint64_t)dst->pages*4096;
    if (src->dma<dst->dma+db && dst->dma<src->dma+sb) goto reject;
    /* The builder validates the entire input before touching the arena. No
     * engine can run under this gate. Context/ring GGTT mappings stay fixed. */
    if (!bcs_context_map_surfaces(c,src,dst,program,u->textures,u->texture_count,u->vertex,u->indices,true)) goto reject;
    if (intel_gpu_buffer_sync(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK ||
        intel_cs_invalidate_tlb(io,backing,engine)!=INTEL_CS_OK) goto quarantine;
    c->retained_source=c->source; c->retained_destination=c->destination;
    c->retained_program=c->program;
    c->program=program; c->program_pinned=program!=NULL;
    c->retained_vertex_program=c->vertex_program.buffer;
    c->vertex_program=u->vertex;c->vertex_program_pinned=u->vertex.buffer!=NULL;
    c->retained_indices=c->indices.buffer;c->indices=u->indices;c->indices_pinned=u->indices.buffer!=NULL;
    bcs_context_install_textures(c,u->textures,u->texture_count);
    c->source=src; c->destination=dst; c->rebindings++;
    if (!bcs_context_drop_retained(c)) goto quarantine;
    return INTEL_BCS_OK;
reject:
    if (bcs_context_drop_retained(c)) return rejected;
quarantine:
    c->ready=false;
    return INTEL_BCS_QUARANTINED;
}

enum intel_bcs_status intel_bcs_context_rebind(intel_bcs_context_t *c,
    intel_gpu_buffer_t *source, intel_gpu_buffer_t *destination)
{
    if (!c || !c->queue) return INTEL_BCS_INVALID;
    bcs_context_update_t update={c,source,destination,NULL,NULL,0,{0},{0}};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_update,&update);
}
enum intel_bcs_status intel_bcs_context_rebind_draw(intel_bcs_context_t *c,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program)
{
    if (!c || !c->queue || !program) return INTEL_BCS_INVALID;
    bcs_context_update_t update={c,source,destination,program,NULL,0,{0},{0}};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_update,&update);
}

enum intel_bcs_status intel_bcs_context_rebind_sampled(intel_bcs_context_t *c,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !program) return INTEL_BCS_INVALID;
    bcs_context_update_t update={c,source,destination,program,textures,count,{0},{0}};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_update,&update);
}

typedef struct {
    intel_bcs_context_t *context;
    intel_gpu_buffer_t *source,*destination,*program;
    uint64_t owner;
    const intel_bcs_texture_binding_t *textures;
    unsigned texture_count;
    intel_bcs_program_binding_t vertex;
    intel_bcs_index_binding_t indices;
} bcs_context_retarget_t;

static enum intel_bcs_status bcs_context_retarget(void *backing,void *argument,
    const intel_gt_io_t *io,intel_bcs_state_t *engine)
{
    bcs_context_retarget_t *update=argument;
    intel_bcs_context_t *c=update->context;
    intel_gpu_buffer_t *src=update->source,*dst=update->destination,*program=update->program;
    if (!c->ready || c->access_token || c->retained_source || c->retained_destination || c->retained_program || c->retained_vertex_program || c->retained_indices || c->retained_texture_count ||
        (!c->bindings_closed && (!c->source_pinned || !c->destination_pinned)) || !src || !dst || src==dst ||
        src==&c->state || src==&c->tables || dst==&c->state || dst==&c->tables || src==&c->batch || dst==&c->batch ||
        c->rebindings==UINT64_MAX ||
        (program && (!c->render || !c->program_gpu || program==src || program==dst ||
         program==&c->tables || program==&c->state || program==&c->batch)) ||
        !bcs_context_textures_valid(c,src,dst,program,update->textures,update->texture_count) ||
        !bcs_context_vertex_valid(c,src,dst,program,update->vertex,update->textures,update->texture_count) ||
        !bcs_context_indices_valid(c,src,dst,program,update->vertex,update->indices,update->textures,update->texture_count)) return INTEL_BCS_INVALID;
    enum intel_gpu_buffer_status pin=intel_gpu_buffer_reside(src,update->owner);
    if (pin!=INTEL_GPU_BUFFER_OK) return pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    c->retained_owner=update->owner; c->retained_source=src;
    pin=intel_gpu_buffer_reside(dst,update->owner);
    enum intel_bcs_status rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
    if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
    c->retained_destination=dst;
    if (program) {
        pin=intel_gpu_buffer_reside(program,update->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_program=program;
    }
    if (update->vertex.buffer) {
        pin=intel_gpu_buffer_reside(update->vertex.buffer,update->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_vertex_program=update->vertex.buffer;
    }
    if (update->indices.buffer) {
        pin=intel_gpu_buffer_reside(update->indices.buffer,update->owner);
        rejected=pin==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
        if (pin!=INTEL_GPU_BUFFER_OK) goto reject;
        c->retained_indices=update->indices.buffer;
    }
    rejected=bcs_context_pin_textures(c,update->owner,update->textures,update->texture_count);
    if (rejected!=INTEL_BCS_OK) goto reject;
    rejected=INTEL_BCS_INVALID;
    uint64_t sb=(uint64_t)src->pages*4096,db=(uint64_t)dst->pages*4096;
    if (src->dma<dst->dma+db && dst->dma<src->dma+sb) goto reject;
    if (!bcs_context_map_surfaces(c,src,dst,program,update->textures,update->texture_count,update->vertex,update->indices,false)) goto reject;
    if (intel_gpu_buffer_sync(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK ||
        intel_cs_invalidate_tlb(io,backing,engine)!=INTEL_CS_OK) goto quarantine;
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++)
        if (!bcs_context_drop_coherent(&c->coherent[i])) goto quarantine;
    c->retained_source=c->source; c->retained_destination=c->destination;
    c->retained_program=c->program;
    c->program=program; c->program_pinned=program!=NULL;
    c->retained_vertex_program=c->vertex_program.buffer;
    c->vertex_program=update->vertex;c->vertex_program_pinned=update->vertex.buffer!=NULL;
    c->retained_indices=c->indices.buffer;c->indices=update->indices;c->indices_pinned=update->indices.buffer!=NULL;
    bcs_context_install_textures(c,update->textures,update->texture_count);
    c->retained_owner=c->owner;
    c->source=src; c->destination=dst; c->owner=update->owner; c->rebindings++;
    c->source_pinned=c->destination_pinned=true;c->bindings_closed=false;
    if (!bcs_context_drop_retained(c)) goto quarantine;
    return INTEL_BCS_OK;
reject:
    if (bcs_context_drop_retained(c)) return rejected;
quarantine:
    c->ready=false;
    return INTEL_BCS_QUARANTINED;
}

enum intel_bcs_status intel_bcs_context_retarget(intel_bcs_context_t *c,uint64_t owner,
    uint64_t epoch,intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination)
{
    if (!c || !c->queue || !owner) return INTEL_BCS_INVALID;
    bcs_context_retarget_t update={c,source,destination,NULL,owner,NULL,0,{0},{0}};
    uint64_t context=0;
    enum intel_bcs_status result=intel_bcs_queue_retarget_context(c->queue,c->queue_owner,
        c->queue_id,owner,epoch,bcs_context_retarget,&update,&context);
    if (result==INTEL_BCS_OK) { c->queue_owner=owner; c->queue_id=context; c->epoch=epoch; }
    return result;
}
enum intel_bcs_status intel_bcs_context_retarget_draw(intel_bcs_context_t *c,uint64_t owner,
    uint64_t epoch,intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program)
{
    if (!c || !c->queue || !owner || !program) return INTEL_BCS_INVALID;
    bcs_context_retarget_t update={c,source,destination,program,owner,NULL,0,{0},{0}};
    uint64_t context=0;
    enum intel_bcs_status result=intel_bcs_queue_retarget_context(c->queue,c->queue_owner,
        c->queue_id,owner,epoch,bcs_context_retarget,&update,&context);
    if (result==INTEL_BCS_OK) { c->queue_owner=owner; c->queue_id=context; c->epoch=epoch; }
    return result;
}

enum intel_bcs_status intel_bcs_context_retarget_sampled(intel_bcs_context_t *c,uint64_t owner,
    uint64_t epoch,intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !owner || !program) return INTEL_BCS_INVALID;
    bcs_context_retarget_t update={c,source,destination,program,owner,textures,count,{0},{0}};
    uint64_t context=0;
    enum intel_bcs_status result=intel_bcs_queue_retarget_context(c->queue,c->queue_owner,
        c->queue_id,owner,epoch,bcs_context_retarget,&update,&context);
    if (result==INTEL_BCS_OK) { c->queue_owner=owner; c->queue_id=context; c->epoch=epoch; }
    return result;
}

static enum intel_bcs_status bcs_context_close_bindings(void *backing,void *argument,
    const intel_gt_io_t *io,intel_bcs_state_t *engine)
{
    intel_bcs_context_t *c=argument;
    if (!c->ready || c->access_token || c->retained_source || c->retained_destination ||
        c->retained_program || c->retained_vertex_program || c->retained_indices || c->retained_texture_count ||
        !c->source_pinned || !c->destination_pinned || c->rebindings==UINT64_MAX) return INTEL_BCS_INVALID;
    intel_ppgtt_range_t batch={c->batch_gpu,c->batch.dma,c->batch.bytes,false};
    if (!intel_ppgtt_build_ranges(c->tables.cpu,c->tables.bytes,c->tables.dma,
            c->render ? &batch : NULL,c->render ? 1 : 0,&c->table_pages) ||
        intel_gpu_buffer_sync(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK ||
        intel_cs_invalidate_tlb(io,backing,engine)!=INTEL_CS_OK) goto quarantine;
    /* Cached translations can no longer reach client backing. The queue gate
     * excludes active work, new submissions and concurrent mapping updates. */
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++)
        if (!bcs_context_drop_coherent(&c->coherent[i])) goto quarantine;
    c->retained_source=c->source;c->retained_destination=c->destination;
    c->retained_program=c->program;c->retained_vertex_program=c->vertex_program.buffer;c->retained_indices=c->indices.buffer;
    c->retained_owner=c->owner;
    bcs_context_install_textures(c,NULL,0);
    c->source=c->destination=c->program=NULL;c->vertex_program=(intel_bcs_program_binding_t){0};
    c->indices=(intel_bcs_index_binding_t){0};
    c->source_pinned=c->destination_pinned=c->program_pinned=c->vertex_program_pinned=c->indices_pinned=false;
    c->bindings_closed=true;c->rebindings++;
    if (!bcs_context_drop_retained(c)) goto quarantine;
    return INTEL_BCS_OK;
quarantine:
    c->ready=false;
    return INTEL_BCS_QUARANTINED;
}
enum intel_bcs_status intel_bcs_context_close_bindings(intel_bcs_context_t *c)
{
    if (!c || !c->queue || !c->ready) return INTEL_BCS_INVALID;
    enum intel_bcs_status status=intel_bcs_queue_close_context(c->queue,c->queue_owner,c->queue_id);
    if (status!=INTEL_BCS_OK || c->bindings_closed) return status;
    return intel_bcs_queue_remove_context_mappings(c->queue,c->queue_owner,c->queue_id,
        bcs_context_close_bindings,c);
}
bool intel_bcs_context_destroy(intel_bcs_context_t *c)
{
    if (!c) return false;
    if (!c->started) return true;
    if (c->access_token) return false;
    if (c->queue) {
        if (intel_bcs_queue_release_context(c->queue,c->queue_owner,c->queue_id)!=INTEL_BCS_OK)
            return false;
        c->queue=NULL;
    }
    c->ready=false; c->reserved=false;
    if (c->ggtt_pinned) {
        if (intel_ggtt_unpin(c->space,&c->binding,c->binding.cookie)!=INTEL_GGTT_BIND_OK)
            return false;
        c->ggtt_pinned=false;
    }
    if (c->binding.phase!=INTEL_GGTT_EMPTY &&
        intel_ggtt_unbind(c->space,&c->binding,c->binding.cookie)!=INTEL_GGTT_BIND_OK) return false;
    if ((c->state.started && intel_gpu_buffer_close(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK) ||
        (c->tables.started && intel_gpu_buffer_close(&c->tables,c->tables.owner)!=INTEL_GPU_BUFFER_OK) ||
        (c->batch.started && intel_gpu_buffer_close(&c->batch,c->batch.owner)!=INTEL_GPU_BUFFER_OK))
        return false;
    if (!bcs_context_drop_retained(c)) return false;
    for (unsigned i=0;i<c->texture_count;i++) if (c->textures[i].buffer) {
        if (intel_gpu_buffer_unreside(c->textures[i].buffer,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->textures[i]=(intel_bcs_texture_binding_t){0};
    }
    c->texture_count=0;
    for (unsigned i=0;i<INTEL_BCS_COHERENT_RANGES;i++)
        if (!bcs_context_drop_coherent(&c->coherent[i])) return false;
    if (c->source_pinned) {
        if (intel_gpu_buffer_unreside(c->source,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->source_pinned=false;
    }
    if (c->destination_pinned) {
        if (intel_gpu_buffer_unreside(c->destination,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->destination_pinned=false;
    }
    if (c->indices_pinned) {
        if (intel_gpu_buffer_unreside(c->indices.buffer,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->indices_pinned=false;
    }
    if (c->vertex_program_pinned) {
        if (intel_gpu_buffer_unreside(c->vertex_program.buffer,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->vertex_program_pinned=false;
    }
    if (c->program_pinned) {
        if (intel_gpu_buffer_unreside(c->program,c->owner)!=INTEL_GPU_BUFFER_OK) return false;
        c->program_pinned=false;
    }
    return true;
}

static bool bcs_context_render_finish(intel_bcs_context_t *c,uint64_t token)
{
    uint32_t *state=c->state.cpu,*ring=state+INTEL_RCS_CONTEXT_BYTES/4;
    if (intel_gpu_buffer_sync(&c->batch,c->batch.owner)!=INTEL_GPU_BUFFER_OK) return false;
    if (c->prepared) {
        if (intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK ||
            !intel_rcs_append_batch(state,INTEL_RCS_CONTEXT_BYTES,ring,4096,c->tail,c->batch_gpu,token,&c->tail)) return false;
    } else {
        if (!intel_rcs_build_batch(state,INTEL_RCS_CONTEXT_BYTES,ring,4096,c->binding.gpu_address,
            c->binding.gpu_address+INTEL_RCS_CONTEXT_BYTES,c->tables.dma,c->hardware_id,c->batch_gpu,
            token,&c->descriptor,&c->tail)) return false;
        c->prepared=true;
    }
    return true;
}

bool intel_bcs_context_prepare(intel_bcs_context_t *c, const intel_bcs_copy_t *copy,
    uint64_t token, uint64_t *descriptor, uint32_t *tail)
{
    if (!c || !c->ready || !copy || !descriptor || !tail || c->access_token!=token ||
        c->source_access.self!=&c->source_access || c->destination_access.self!=&c->destination_access ||
        copy->source!=c->source_gpu || copy->destination!=c->destination_gpu ||
        copy->source_bytes>c->source->bytes || copy->destination_bytes>c->destination->bytes)
        return false;
    /* Check before adding origins to extents. The builder also checks the
     * complete owned range; no subrectangle can rebase a pinned GPU mapping. */
    if (c->render && (!copy->width || !copy->height || copy->width>16384 || copy->height>16384 ||
        copy->source_x>16384-copy->width || copy->destination_x>16384-copy->width ||
        copy->source_y>16384-copy->height || copy->destination_y>16384-copy->height)) return false;
    uint32_t *state=c->state.cpu, *ring=state+(c->render ? INTEL_RCS_CONTEXT_BYTES/4 : 2048);
    if (c->render) {
        intel_rcs_draw_t draw={.batch_gpu=c->batch_gpu,.source_gpu=copy->source,.destination_gpu=copy->destination,
            .source_bytes=copy->source_bytes,.destination_bytes=copy->destination_bytes,
            .source_width=copy->source_x+copy->width,.source_height=copy->source_y+copy->height,.source_pitch=copy->source_pitch,
            .destination_width=copy->destination_x+copy->width,.destination_height=copy->destination_y+copy->height,
            .destination_pitch=copy->destination_pitch,.source_x=copy->source_x,.source_y=copy->source_y,
            .destination_x=copy->destination_x,.destination_y=copy->destination_y,
            .width=copy->width,.height=copy->height};
        if (!intel_rcs_draw_build(c->batch.cpu,c->batch.bytes,&draw) ||
            !bcs_context_render_finish(c,token)) return false;
    } else if (c->prepared) {
        /* Fetch the hardware-saved state before the CPU changes its tail. */
        if (intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK ||
            !intel_bcs_append_copy(state,8192,ring,4096,c->tail,token,copy,&c->tail)) return false;
    } else {
        if (!intel_bcs_build_copy(state,8192,ring,4096,c->binding.gpu_address,
            c->binding.gpu_address+8192,c->tables.dma,c->hardware_id,token,copy,&c->descriptor))
            return false;
        c->tail=INTEL_BCS_COPY_TAIL; c->prepared=true;
    }
    if (intel_gpu_buffer_sync(&c->source[0],c->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(&c->destination[0],c->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK) return false;
    *descriptor=c->descriptor; *tail=c->tail;
    return true;
}

bool intel_bcs_context_prepare_draw(intel_bcs_context_t *c,const intel_rcs_queue_draw_t *work,
    uint64_t token,uint64_t *descriptor,uint32_t *tail)
{
    if (!c || !c->ready || !c->render || !work || !descriptor || !tail ||
        !c->program || !c->program_pinned || c->access_token!=token ||
        c->program_access.self!=&c->program_access || c->source_access.self!=&c->source_access ||
        c->destination_access.self!=&c->destination_access) return false;
    const intel_rcs_primitive_draw_t *draw=&work->primitive;
    const struct intel_d3d9_program *program=&work->program;
    if (program->allocation_bytes>c->program->bytes || (program->code_offset&4095) ||
        program->code_offset>=c->program->bytes ||
        draw->pixel.program_gpu!=c->program_gpu+program->code_offset ||
        draw->pixel.batch_gpu!=c->batch_gpu || draw->vertices_gpu!=c->source_gpu ||
        draw->vertices_bytes>c->source->bytes || draw->pixel.destination_gpu!=c->destination_gpu ||
        draw->pixel.destination_bytes>c->destination->bytes) return false;
    bool vertex=work->vertex_program.base.abi!=0;
    if (vertex!=!!c->vertex_program.buffer) return false;
    if (vertex && (!c->vertex_program_pinned ||
        c->vertex_program_access.self!=&c->vertex_program_access ||
        c->vertex_program_access.buffer!=c->vertex_program.buffer ||
        work->vertex_program.base.allocation_bytes>c->vertex_program.buffer->bytes ||
        (work->vertex_program.base.code_offset&4095) ||
        work->vertex_program.base.code_offset>=c->vertex_program.buffer->bytes ||
        work->vertex.program_gpu!=c->vertex_program.gpu+work->vertex_program.base.code_offset ||
        program->abi!=INTEL_D3D9_COMPILER_RESOURCE_ABI)) return false;
    bool indexed=work->indices.gpu!=0;
    if (indexed!=!!c->indices.buffer) return false;
    if (indexed && (!c->indices_pinned || c->index_access.self!=&c->index_access ||
        c->index_access.buffer!=c->indices.buffer || c->index_access.owner!=c->owner ||
        work->indices.gpu!=c->indices.gpu || work->indices.bytes>c->indices.buffer->bytes ||
        program->abi!=INTEL_D3D9_COMPILER_RESOURCE_ABI)) return false;
    bool built;
    if (program->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI) {
        const intel_rcs_sampled_draw_t *sampled=&work->sampled;
        unsigned used=0;
        for (unsigned i=0;i<16;i++) if (sampled->bound_mask&(1u<<i)) {
            const intel_rcs_texture_binding_t *texture=&sampled->textures[i];
            bool found=false;
            for (unsigned j=0;j<c->texture_count;j++) {
                const intel_bcs_texture_binding_t *binding=&c->textures[j];
                const intel_gpu_access_t *access=&c->texture_access[j];
                if (texture->gpu==binding->gpu && texture->bytes<=binding->buffer->bytes &&
                    access->self==access && access->buffer==binding->buffer && access->owner==c->owner) {
                    found=true;used|=1u<<j;break;
                }
            }
            if (!found) return false;
        }
        if (used!=((1u<<c->texture_count)-1u)) return false;
        built=indexed ? intel_rcs_indexed_build(c->batch.cpu,c->batch.bytes,sampled,program,program->allocation_bytes,
            vertex ? &work->vertex : NULL,vertex ? &work->vertex_program : NULL,
            vertex ? work->vertex_program.base.allocation_bytes : 0,&work->indices,
            c->indices.buffer->cpu,c->indices.buffer->bytes,work->scissors,work->scissor_count) :
            vertex ? intel_rcs_vertex_build(c->batch.cpu,c->batch.bytes,sampled,program,program->allocation_bytes,
            &work->vertex,&work->vertex_program,work->vertex_program.base.allocation_bytes,work->scissors,work->scissor_count) :
            intel_rcs_sampled_build(c->batch.cpu,c->batch.bytes,sampled,program,program->allocation_bytes,
                work->scissors,work->scissor_count);
    } else built=intel_rcs_primitives_build(c->batch.cpu,c->batch.bytes,draw,program,program->allocation_bytes,
                                          work->scissors,work->scissor_count);
    if (!built ||
        !bcs_context_render_finish(c,token) ||
        intel_gpu_buffer_sync(c->program,c->owner)!=INTEL_GPU_BUFFER_OK ||
        (vertex && intel_gpu_buffer_sync(c->vertex_program.buffer,c->owner)!=INTEL_GPU_BUFFER_OK) ||
        (indexed && intel_gpu_buffer_sync(c->indices.buffer,c->owner)!=INTEL_GPU_BUFFER_OK) ||
        intel_gpu_buffer_sync(c->source,c->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(c->destination,c->owner)!=INTEL_GPU_BUFFER_OK ||
        intel_gpu_buffer_sync(&c->state,c->state.owner)!=INTEL_GPU_BUFFER_OK) return false;
    *descriptor=c->descriptor; *tail=c->tail;
    return true;
}

enum intel_bcs_status intel_bcs_context_acquire(intel_bcs_context_t *c,uint64_t token)
{
    if (!c || !c->ready || c->bindings_closed || !token || c->access_token) return INTEL_BCS_INVALID;
    c->access_token=token;
    enum intel_gpu_buffer_status r=intel_gpu_buffer_acquire_gpu(c->source,c->owner,&c->source_access);
    if (r==INTEL_GPU_BUFFER_OK)
        r=intel_gpu_buffer_acquire_gpu(c->destination,c->owner,&c->destination_access);
    if (r==INTEL_GPU_BUFFER_OK && c->program)
        r=intel_gpu_buffer_acquire_gpu(c->program,c->owner,&c->program_access);
    if (r==INTEL_GPU_BUFFER_OK && c->vertex_program.buffer)
        r=intel_gpu_buffer_acquire_gpu(c->vertex_program.buffer,c->owner,&c->vertex_program_access);
    if (r==INTEL_GPU_BUFFER_OK && c->indices.buffer)
        r=intel_gpu_buffer_acquire_gpu(c->indices.buffer,c->owner,&c->index_access);
    for (unsigned i=0;r==INTEL_GPU_BUFFER_OK && i<c->texture_count;i++)
        r=intel_gpu_buffer_acquire_gpu(c->textures[i].buffer,c->owner,&c->texture_access[i]);
    return r==INTEL_GPU_BUFFER_OK ? INTEL_BCS_OK :
        r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_INVALID;
}

enum intel_bcs_status intel_bcs_context_retire(intel_bcs_context_t *c,uint64_t token)
{
    if (!c || !token || (c->access_token && c->access_token!=token)) return INTEL_BCS_INVALID;
    if (!c->access_token) return INTEL_BCS_OK;
    for (unsigned i=0;i<c->texture_count;i++) if (c->texture_access[i].self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->texture_access[i]);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    if (c->index_access.self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->index_access);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    if (c->vertex_program_access.self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->vertex_program_access);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    if (c->program_access.self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->program_access);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    if (c->destination_access.self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->destination_access);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    if (c->source_access.self) {
        enum intel_gpu_buffer_status r=intel_gpu_buffer_release_gpu(&c->source_access);
        if (r!=INTEL_GPU_BUFFER_OK) return r==INTEL_GPU_BUFFER_BUSY ? INTEL_BCS_BUSY : INTEL_BCS_QUARANTINED;
    }
    c->access_token=0;
    return INTEL_BCS_OK;
}

enum intel_bcs_status intel_bcs_context_activate_vertex(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,const intel_bcs_texture_binding_t *textures,unsigned count)
{
    return vertex.buffer ? bcs_context_activate(c,owner,source,destination,program,textures,count,vertex,(intel_bcs_index_binding_t){0}) : INTEL_BCS_INVALID;
}

enum intel_bcs_status intel_bcs_context_rebind_vertex(intel_bcs_context_t *c,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !vertex.buffer) return INTEL_BCS_INVALID;
    bcs_context_update_t update={c,source,destination,program,textures,count,vertex,{0}};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_update,&update);
}

enum intel_bcs_status intel_bcs_context_retarget_vertex(intel_bcs_context_t *c,uint64_t owner,uint64_t epoch,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !owner || !vertex.buffer) return INTEL_BCS_INVALID;
    bcs_context_retarget_t update={c,source,destination,program,owner,textures,count,vertex,{0}};
    uint64_t context=0;
    enum intel_bcs_status result=intel_bcs_queue_retarget_context(c->queue,c->queue_owner,
        c->queue_id,owner,epoch,bcs_context_retarget,&update,&context);
    if (result==INTEL_BCS_OK) {c->queue_owner=owner;c->queue_id=context;c->epoch=epoch;}
    return result;
}

enum intel_bcs_status intel_bcs_context_activate_indexed(intel_bcs_context_t *c,uint64_t owner,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    return indices.buffer ? bcs_context_activate(c,owner,source,destination,program,textures,count,vertex,indices) : INTEL_BCS_INVALID;
}
enum intel_bcs_status intel_bcs_context_rebind_indexed(intel_bcs_context_t *c,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !indices.buffer) return INTEL_BCS_INVALID;
    bcs_context_update_t update={c,source,destination,program,textures,count,vertex,indices};
    return intel_bcs_queue_update_context(c->queue,c->owner,c->queue_id,bcs_context_update,&update);
}
enum intel_bcs_status intel_bcs_context_retarget_indexed(intel_bcs_context_t *c,uint64_t owner,uint64_t epoch,
    intel_gpu_buffer_t *source,intel_gpu_buffer_t *destination,intel_gpu_buffer_t *program,
    intel_bcs_program_binding_t vertex,intel_bcs_index_binding_t indices,
    const intel_bcs_texture_binding_t *textures,unsigned count)
{
    if (!c || !c->queue || !owner || !indices.buffer) return INTEL_BCS_INVALID;
    bcs_context_retarget_t update={c,source,destination,program,owner,textures,count,vertex,indices};
    uint64_t context=0;
    enum intel_bcs_status result=intel_bcs_queue_retarget_context(c->queue,c->queue_owner,
        c->queue_id,owner,epoch,bcs_context_retarget,&update,&context);
    if (result==INTEL_BCS_OK) {c->queue_owner=owner;c->queue_id=context;c->epoch=epoch;}
    return result;
}
