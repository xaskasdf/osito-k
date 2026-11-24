#include "intel_gpu_capture.h"
#include "intel_gen9_shader.h"
#include "intel_gen9_blend.h"

static void gpu_capture_bytes(void *destination,const void *source,size_t bytes)
{
    BYTE *out=destination;
    const volatile BYTE *in=source;
    for (size_t i=0;i<bytes;i++) out[i]=in[i];
}
static NTSTATUS gpu_capture_program(intel_gpu_capture_t *c)
{
    intel_gpu_buffer_t *b=c->resources[2].native.buffer;
    if (c->draw.program_bytes<sizeof(c->program) || c->draw.program_bytes>b->bytes) return STATUS_INVALID_PARAMETER;
    enum intel_gpu_buffer_status access=intel_gpu_buffer_acquire_cpu(b,c->context.native.pid,&c->program_cpu);
    if (access!=INTEL_GPU_BUFFER_OK) return access==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    gpu_capture_bytes(&c->program,c->program_cpu.cpu,sizeof(c->program));
    bool sampled=(c->draw.version==INTEL_UMD_DRAW_SAMPLED_VERSION || c->draw.version==INTEL_UMD_DRAW_VERTEX_VERSION || c->draw.version==INTEL_UMD_DRAW_VERTEX_SHADER_VERSION || intel_gpu_capture_has_indices(c));
    bool valid=!(c->program.code_offset&4095) && !(c->draw.dispatch_mask&~c->program.simd_mask) &&
        c->program.parameter_count==c->constants.parameter_count &&
        (sampled ? intel_gen9_resource_shader_validate(&c->program,c->program_cpu.cpu,c->draw.program_bytes) :
                   intel_gen9_shader_validate(&c->program,c->program_cpu.cpu,c->draw.program_bytes));
    access=intel_gpu_buffer_release_cpu(&c->program_cpu);
    if (access!=INTEL_GPU_BUFFER_OK) return access==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    return valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
static NTSTATUS gpu_capture_vertex_program(intel_gpu_capture_t *c)
{
    intel_gpu_buffer_t *b=c->resources[3].native.buffer;
    if (c->vertex.vertex_program || c->vertex.vertex_program_bytes<sizeof(c->vertex_program) ||
        c->vertex.vertex_program_bytes>b->bytes) return STATUS_INVALID_PARAMETER;
    enum intel_gpu_buffer_status access=intel_gpu_buffer_acquire_cpu(b,c->context.native.pid,&c->vertex_program_cpu);
    if (access!=INTEL_GPU_BUFFER_OK) return access==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    gpu_capture_bytes(&c->vertex_program,c->vertex_program_cpu.cpu,sizeof(c->vertex_program));
    bool valid=!(c->vertex_program.base.code_offset&4095) &&
        c->vertex_program.base.parameter_count==c->vertex.vertex_parameter_count &&
        intel_gen9_vertex_shader_validate(&c->vertex_program,c->vertex_program_cpu.cpu,c->vertex.vertex_program_bytes);
    access=intel_gpu_buffer_release_cpu(&c->vertex_program_cpu);
    if (access!=INTEL_GPU_BUFFER_OK) return access==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    return valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
static bool gpu_capture_vertex_valid(const intel_gpu_capture_t *c,unsigned *extent)
{
    const intel_umd_draw_vertex_shader_t *v=&c->vertex;
    const struct intel_d3d9_vertex_program *p=&c->vertex_program;
    if (v->reserved || !v->viewport_width || !v->viewport_height ||
        v->viewport_width>v->draw.destination_width || v->viewport_height>v->draw.destination_height ||
        v->viewport_x>v->draw.destination_width-v->viewport_width ||
        v->viewport_y>v->draw.destination_height-v->viewport_height) return false;
    for (unsigned i=0;i<8;i++) {
        if (v->texcoord_offset[i]) return false;
        if ((c->program.texcoord_mask&(1u<<i)) && !(p->output_mask&(1u<<(i+3)))) return false;
    }
    *extent=4;
    for (unsigned i=0;i<16;i++) {
        unsigned packed=v->attributes[i],offset=packed&0xffffu;
        if (!(p->input_mask&(1u<<i))) {if (packed) return false;continue;}
        intel_gen9_vertex_format_t f;
        if (!intel_gen9_vertex_format(packed>>16,&f) || f.bytes>v->draw.vertex_stride ||
            (offset&3) || offset>v->draw.vertex_stride-f.bytes) return false;
        if (offset+f.bytes>*extent)*extent=offset+f.bytes;
    }
    return true;
}
static intel_gen9_sampler_t gpu_capture_sampler(const intel_umd_sampler_t *s)
{
    return (intel_gen9_sampler_t){s->min_filter,s->mag_filter,s->mip_filter,
        s->address_u,s->address_v,s->address_w,s->lod_bias,s->min_lod,s->max_lod,s->border_argb};
}
static bool gpu_capture_sampling_valid(const intel_gpu_capture_t *c)
{
    const intel_umd_draw_sampled_t *s=&c->sampled;
    bool vertex=intel_gpu_capture_has_vertex(c);
    bool typed=s->draw.version==INTEL_UMD_DRAW_VERTEX_VERSION || (intel_gpu_capture_has_indices(c) && !vertex);
    if (s->bound_mask!=c->program.texture_mask || (s->bound_mask&~0xffffu) ||
        (!vertex && ((!typed && s->reserved) || (s->position_offset&3) ||
         s->position_offset>s->draw.vertex_stride-16))) return false;
    unsigned vertex_bytes=s->position_offset+16;
    if (vertex && !gpu_capture_vertex_valid(c,&vertex_bytes)) return false;
    for (unsigned i=0;!vertex && i<8;i++) {
        unsigned offset=s->texcoord_offset[i];
        if (!(c->program.texcoord_mask&(1u<<i))) { if (offset) return false;continue; }
        unsigned type=typed ? offset>>16 : 3;
        if (typed) offset&=0xffff;
        intel_gen9_vertex_format_t format;
        if (!intel_gen9_vertex_format(type,&format) || (offset&3) || offset>s->draw.vertex_stride-format.bytes) return false;
        if (offset+format.bytes>vertex_bytes) vertex_bytes=offset+format.bytes;
    }
    if ((uint64_t)s->draw.vertex_offset+(uint64_t)(s->draw.vertex_count-1)*s->draw.vertex_stride+
        vertex_bytes>c->resources[0].native.buffer->bytes) return false;
    for (unsigned i=0;i<INTEL_UMD_MAX_TEXTURES;i++) {
        const intel_umd_texture_t *t=&s->textures[i];
        if (!(s->bound_mask&(1u<<i))) {
            const BYTE *bytes=(const void *)t;
            for (unsigned j=0;j<sizeof(*t);j++) if (bytes[j]) return false;
            continue;
        }
        unsigned resource=c->texture_resource[i];
        intel_gen9_image_t image;
        intel_gen9_sampler_t sampler=gpu_capture_sampler(&t->sampler);
        intel_gen9_sampler_state_t packed;
        if (t->image || resource<(3u+vertex+intel_gpu_capture_has_indices(c)) || resource>=c->resource_count ||
            !intel_gen9_image_layout(t->width,t->height,t->levels,t->format,t->pitch,&image) ||
            image.bytes>c->resources[resource].native.buffer->bytes ||
            t->first>=image.levels || !t->count || t->count>image.levels-t->first || t->srgb>1 ||
            !intel_gen9_sampler_state(&sampler,INTEL_RCS_SAMPLED_BORDERS+i*64,INTEL_RCS_SAMPLED_BYTES,&packed))
            return false;
    }
    return true;
}
static intel_rcs_index_binding_t gpu_capture_index_binding(const intel_gpu_capture_t *c,uint64_t gpu,uint64_t bytes)
{
    const intel_umd_draw_indexed_t *i=&c->indexed;
    return (intel_rcs_index_binding_t){gpu,bytes,i->index_offset,i->index_count,i->index_stride,
        i->base_vertex,i->min_index,i->num_vertices};
}
static NTSTATUS gpu_capture_indices(intel_gpu_capture_t *c)
{
    intel_gpu_buffer_t *b=c->resources[3+intel_gpu_capture_has_vertex(c)].native.buffer;
    if (c->indexed.indices) return STATUS_INVALID_PARAMETER;
    enum intel_gpu_buffer_status status=intel_gpu_buffer_acquire_cpu(b,c->context.native.pid,&c->index_cpu);
    if (status!=INTEL_GPU_BUFFER_OK) return status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    /* The address here only checks range arithmetic; no application VA enters
     * the descriptor. Actual mapping is assigned by the device owner later. */
    intel_rcs_index_binding_t binding=gpu_capture_index_binding(c,4096,b->bytes);
    bool valid=intel_rcs_index_validate(&binding,c->index_cpu.cpu,b->bytes,c->draw.vertex_count,c->draw.topology);
    status=intel_gpu_buffer_release_cpu(&c->index_cpu);
    if (status!=INTEL_GPU_BUFFER_OK) return status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    return valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
NTSTATUS intel_gpu_capture_begin(DWORD pid,OKMT_HANDLE context,DWORD offset,DWORD bytes,
    DWORD allocations,DWORD patches,intel_gpu_capture_t *capture)
{
    if (!capture || capture->self) return STATUS_INVALID_PARAMETER;
    *capture=(intel_gpu_capture_t){.self=capture};
    NTSTATUS status=intel_gpu_kmt_acquire_context(pid,context,&capture->context);
    if (status!=STATUS_SUCCESS) return status;
    intel_gpu_context_lease_t *native=&capture->context.native;
    if (native->node!=INTEL_UMD_NODE_COPY && native->node!=INTEL_UMD_NODE_RENDER) return STATUS_NOT_SUPPORTED;
    bool draw=bytes>=sizeof(capture->draw);
    unsigned count=draw ? 3 : 2;
    if ((offset&7) || (bytes&3) || bytes>sizeof(capture->indexed) ||
        (!draw && bytes!=sizeof(capture->command)) || offset>native->command_bytes ||
        bytes>native->command_bytes-offset || allocations>INTEL_UMD_MAX_RESOURCES || patches>INTEL_UMD_MAX_RESOURCES ||
        allocations>native->allocation_count || patches>native->patch_count) return STATUS_INVALID_PARAMETER;
    if (draw && native->node!=INTEL_UMD_NODE_RENDER) return STATUS_NOT_SUPPORTED;
    gpu_capture_bytes(&capture->indexed,(const BYTE *)native->commands+offset,bytes);
    gpu_capture_bytes(capture->allocations,native->allocations,allocations*sizeof(*capture->allocations));
    gpu_capture_bytes(capture->patches,native->patches,patches*sizeof(*capture->patches));
    const intel_umd_copy32_t *command=&capture->command;
    bool indexed=draw && intel_gpu_capture_has_indices(capture);
    bool vertex=draw && intel_gpu_capture_has_vertex(capture);
    unsigned geometry=count+vertex+indexed;
    count=geometry;
    bool sampled=draw && (command->version==INTEL_UMD_DRAW_SAMPLED_VERSION || command->version==INTEL_UMD_DRAW_VERTEX_VERSION || vertex || indexed);
    bool blending=draw && (command->version==INTEL_UMD_DRAW_BLEND_VERSION || sampled);
    bool rectangles=draw && (command->version==INTEL_UMD_DRAW_RECTANGLES_VERSION ||
        (blending && capture->constants.reserved));
    bool constants=blending || rectangles || (draw && command->version==INTEL_UMD_DRAW_CONSTANTS_VERSION);
    bool copy_rect=!draw && command->version==INTEL_UMD_COPY_RECT_VERSION;
    unsigned payload=indexed ? __builtin_offsetof(intel_umd_draw_indexed_t,values) : vertex ? __builtin_offsetof(intel_umd_draw_vertex_shader_t,values) : sampled ? __builtin_offsetof(intel_umd_draw_sampled_t,values) : 136;
    if (sampled) {
        unsigned textures=(unsigned)__builtin_popcount(capture->sampled.bound_mask);
        if (bytes<payload || (capture->sampled.bound_mask&~0xffffu) || (command->version==INTEL_UMD_DRAW_SAMPLED_VERSION && capture->sampled.reserved) ||
            allocations<geometry || allocations>geometry+textures || patches!=geometry+textures) return STATUS_INVALID_PARAMETER;
    } else if (allocations!=count || patches!=count || bytes>sizeof(capture->batch)) return STATUS_INVALID_PARAMETER;
    if (command->magic!=INTEL_UMD_COMMAND_MAGIC || (!constants && !copy_rect && command->version!=INTEL_UMD_COMMAND_VERSION) ||
        command->opcode!=(draw ? INTEL_UMD_DRAW32 : INTEL_UMD_COPY32)) return STATUS_NOT_SUPPORTED;
    if (indexed && !vertex) {
        const BYTE *prefix=(const void *)&capture->vertex.vertex_program;
        for (unsigned i=0;i<1552-1456;i++) if (prefix[i]) return STATUS_INVALID_PARAMETER;
    }
    unsigned vertex_parameters=vertex ? capture->vertex.vertex_parameter_count : 0;
    if (vertex_parameters>INTEL_UMD_MAX_PARAMETERS) return STATUS_INVALID_PARAMETER;
    if (constants) {
        unsigned rects=rectangles ? capture->constants.reserved : 0;
        if (bytes<payload || (!rectangles && capture->constants.reserved) ||
            (rectangles && (!rects || rects>INTEL_UMD_MAX_RECTANGLES)) || (!blending && !capture->constants.parameter_count) ||
            capture->constants.parameter_count>INTEL_UMD_MAX_PARAMETERS ||
            bytes!=payload+(capture->constants.parameter_count+vertex_parameters)*4+rects*16) return STATUS_INVALID_PARAMETER;
    } else if (draw && bytes!=sizeof(capture->draw)) return STATUS_INVALID_PARAMETER;
    if (!draw && (command->bytes!=sizeof(*command) || command->source || command->destination ||
        (!copy_rect && (command->reserved[0] || command->reserved[1] || command->reserved[2] || command->reserved[3])) ||
        !command->width || !command->height || command->width>32767 || command->height>32767 ||
        (command->source_pitch&3) || (command->destination_pitch&3) ||
        command->source_pitch>32764 || command->destination_pitch>32764 ||
        command->width>command->source_pitch/4 || command->width>command->destination_pitch/4 ||
        command->reserved[0]>command->source_pitch/4-command->width ||
        command->reserved[2]>command->destination_pitch/4-command->width ||
        command->reserved[1]>32767-command->height || command->reserved[3]>32767-command->height))
        return STATUS_INVALID_PARAMETER;
    if (!draw && native->node==INTEL_UMD_NODE_RENDER &&
        (command->reserved[0]+command->width>16384 || command->reserved[2]+command->width>16384 ||
         command->reserved[1]+command->height>16384 || command->reserved[3]+command->height>16384))
        return STATUS_INVALID_PARAMETER;
    if (draw) {
        const intel_umd_draw32_t *d=&capture->draw;
        if (d->bytes!=bytes || d->vertices || d->destination || d->program || d->reserved0 ||
            d->reserved1[0] || d->reserved1[1] || !d->destination_width || d->destination_width>4096 ||
            !d->destination_height || d->destination_height>4096 || (d->destination_pitch&3) ||
            d->destination_pitch<d->destination_width*4 || d->destination_pitch>262144 ||
            !d->width || d->width>d->destination_width || !d->height || d->height>d->destination_height ||
            d->x>d->destination_width-d->width || d->y>d->destination_height-d->height ||
            d->vertex_stride<(vertex ? 4u : 16u) || d->vertex_stride>2048 || (d->vertex_stride&3) || (d->vertex_offset&3) ||
            !d->vertex_count || (!indexed && (d->vertex_count<3 || d->vertex_count>65535 ||
            (d->topology==INTEL_RCS_TRIANGLE_LIST && d->vertex_count%3))) ||
            d->topology<INTEL_RCS_TRIANGLE_LIST || d->topology>INTEL_RCS_TRIANGLE_FAN ||
            !d->dispatch_mask || (d->dispatch_mask&~(sampled ? 7u : 3u))) return STATUS_INVALID_PARAMETER;
        for (unsigned i=blending?4:0;i<6;i++) if (d->reserved2[i]) return STATUS_INVALID_PARAMETER;
        if (blending) {
            intel_umd_blend_t state={d->reserved2[0],d->reserved2[1],d->reserved2[2],d->reserved2[3]};
            intel_gen9_blend_state_t packed;
            if (!(state.flags&INTEL_UMD_BLEND_VALID) || !intel_gen9_blend_pack(&state,&packed))
                return STATUS_INVALID_PARAMETER;
        }
        if (rectangles) {
            const intel_umd_rectangle_t *r=(const void *)((const BYTE *)&capture->sampled+
                payload+(capture->constants.parameter_count+vertex_parameters)*4);
            for (unsigned i=0;i<capture->constants.reserved;i++)
                if (!r[i].width || !r[i].height || r[i].width>d->width || r[i].height>d->height ||
                    r[i].x<d->x || r[i].y<d->y || r[i].x-d->x>d->width-r[i].width ||
                    r[i].y-d->y>d->height-r[i].height) return STATUS_INVALID_PARAMETER;
        }
    }
    DWORD index[INTEL_UMD_MAX_RESOURCES];
    for (unsigned i=0;i<INTEL_UMD_MAX_RESOURCES;i++) index[i]=INTEL_UMD_MAX_RESOURCES;
    for (unsigned i=0;i<patches;i++) {
        const OKMT_PATCH_LOCATION *patch=&capture->patches[i];
        unsigned role=patch->slot;
        bool vertex_role=vertex && role==INTEL_UMD_VERTEX_PROGRAM_ROLE;
        bool index_role=indexed && role==INTEL_UMD_INDEX_ROLE;
        if (role>=(sampled ? INTEL_UMD_MAX_RESOURCES : count) ||
            (!vertex_role && !index_role && role>=3 && (role>=3+INTEL_UMD_MAX_TEXTURES ||
             !(capture->sampled.bound_mask&(1u<<(role-3)))))) return STATUS_INVALID_PARAMETER;
        unsigned expected=offset+(index_role ? __builtin_offsetof(intel_umd_draw_indexed_t,indices) : vertex_role ? __builtin_offsetof(intel_umd_draw_vertex_shader_t,vertex_program) :
            role<3 ? (draw ? 80 : 32)+role*8 :
            __builtin_offsetof(intel_umd_draw_sampled_t,textures)+(role-3)*sizeof(intel_umd_texture_t));
        if (patch->patch_offset!=expected || index[role]!=INTEL_UMD_MAX_RESOURCES || patch->allocation_index>=allocations ||
            patch->driver_id || patch->allocation_offset || patch->split_offset) return STATUS_INVALID_PARAMETER;
        index[role]=patch->allocation_index;
    }
    for (unsigned i=0;i<allocations;i++) {
        if (!capture->allocations[i].allocation || capture->allocations[i].flags!=(i==index[1] ? 1u : 0u))
            return STATUS_INVALID_PARAMETER;
        for (unsigned j=0;j<i;j++) if (capture->allocations[i].allocation==capture->allocations[j].allocation)
            return STATUS_INVALID_PARAMETER;
    }
    DWORD unique[INTEL_UMD_MAX_RESOURCES];
    for (unsigned i=0;i<count;i++) {
        unsigned role=indexed && i==3u+vertex ? INTEL_UMD_INDEX_ROLE : vertex && i==3 ? INTEL_UMD_VERTEX_PROGRAM_ROLE : i;
        unique[i]=index[role];
        if (unique[i]>=allocations) return STATUS_INVALID_PARAMETER;
        for (unsigned j=0;j<i;j++) if (unique[j]==unique[i]) return STATUS_INVALID_PARAMETER;
    }
    if (sampled) for (unsigned slot=0;slot<INTEL_UMD_MAX_TEXTURES;slot++) if (capture->sampled.bound_mask&(1u<<slot)) {
        unsigned allocation=index[3+slot],resource=0;
        if (allocation>=allocations || capture->sampled.textures[slot].image) return STATUS_INVALID_PARAMETER;
        while (resource<count && unique[resource]!=allocation) resource++;
        if (resource<geometry) return STATUS_INVALID_PARAMETER;
        if (resource==count) unique[count++]=allocation;
        capture->texture_resource[slot]=resource;
    }
    if (count!=allocations) return STATUS_INVALID_PARAMETER; /* no unused or duplicate allocation entries */
    capture->resource_count=count;
    for (unsigned role=0;role<count;role++) {
        status=intel_gpu_kmt_acquire_resident(pid,capture->context.reference.device,
            capture->allocations[unique[role]].allocation,&capture->resources[role]);
        if (status!=STATUS_SUCCESS) return status;
        intel_gpu_memory_resident_lease_t *resource=&capture->resources[role].native;
        if (resource->memory!=native->owner->memory || resource->device!=native->device)
            return STATUS_INVALID_PARAMETER;
        uint64_t end;
        if (draw) {
            const intel_umd_draw32_t *d=&capture->draw;
            end=role==0 ? (uint64_t)d->vertex_offset+(uint64_t)(d->vertex_count-1)*d->vertex_stride+(vertex ? 4 : 16) :
                role==1 ? (uint64_t)d->destination_height*d->destination_pitch : role==2 ? d->program_bytes : vertex && role==3 ? capture->vertex.vertex_program_bytes : 0;
        } else {
            uint64_t pitch=role ? command->destination_pitch : command->source_pitch;
            uint64_t x=command->reserved[role*2],y=command->reserved[role*2+1];
            end=native->node==INTEL_UMD_NODE_RENDER ? (y+command->height)*pitch :
                (y+command->height-1)*pitch+(x+command->width)*4;
        }
        if (end>resource->buffer->bytes) return STATUS_INVALID_PARAMETER;
        for (unsigned j=0;j<role;j++)
            if (resource->buffer==capture->resources[j].native.buffer) return STATUS_INVALID_PARAMETER;
    }
    if (draw && (status=gpu_capture_program(capture))!=STATUS_SUCCESS) return status;
    if (vertex && (status=gpu_capture_vertex_program(capture))!=STATUS_SUCCESS) return status;
    if (sampled && !gpu_capture_sampling_valid(capture)) return STATUS_INVALID_PARAMETER;
    if (indexed && (status=gpu_capture_indices(capture))!=STATUS_SUCCESS) return status;
    if (native->owner->fences) {
        status=intel_gpu_fences_capture(native->owner->fences,native->pid,native->device,native->epoch,
            capture->fences,INTEL_BCS_COHERENT_RANGES,&capture->fence_count);
        if (status!=STATUS_SUCCESS) return status;
    }
    capture->valid=true;
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_capture_release(intel_gpu_capture_t *capture)
{
    if (!capture) return STATUS_INVALID_PARAMETER;
    if (!capture->self) return STATUS_SUCCESS;
    if (capture->self!=capture) return STATUS_INVALID_PARAMETER;
    capture->valid=false;
    if (capture->program_cpu.self) {
        enum intel_gpu_buffer_status status=intel_gpu_buffer_release_cpu(&capture->program_cpu);
        if (status!=INTEL_GPU_BUFFER_OK && status!=INTEL_GPU_BUFFER_IO)
            return status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    }
    if (capture->vertex_program_cpu.self) {
        enum intel_gpu_buffer_status status=intel_gpu_buffer_release_cpu(&capture->vertex_program_cpu);
        if (status!=INTEL_GPU_BUFFER_OK && status!=INTEL_GPU_BUFFER_IO)
            return status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    }
    if (capture->index_cpu.self) {
        enum intel_gpu_buffer_status status=intel_gpu_buffer_release_cpu(&capture->index_cpu);
        if (status!=INTEL_GPU_BUFFER_OK && status!=INTEL_GPU_BUFFER_IO)
            return status==INTEL_GPU_BUFFER_BUSY ? D3DKMT_STATUS_DEVICE_BUSY : STATUS_UNSUCCESSFUL;
    }
    NTSTATUS result=STATUS_SUCCESS;
    for (unsigned i=0;i<capture->fence_count;i++) if (capture->fences[i].self) {
        NTSTATUS status=intel_gpu_fence_release(&capture->fences[i]);
        if (status!=STATUS_SUCCESS) result=status;
    }
    for (unsigned i=0;i<INTEL_UMD_MAX_RESOURCES;i++) if (capture->resources[i].reference.self) {
        NTSTATUS status=intel_gpu_kmt_release_resident(&capture->resources[i]);
        if (status!=STATUS_SUCCESS) result=status;
    }
    if (result!=STATUS_SUCCESS) return result;
    if (capture->context.reference.self) result=intel_gpu_kmt_release_context(&capture->context);
    if (result==STATUS_SUCCESS) *capture=(intel_gpu_capture_t){0};
    return result;
}
NTSTATUS intel_gpu_capture_copy(const intel_gpu_capture_t *capture,const intel_bcs_context_t *context,
    intel_bcs_copy_t *copy)
{
    if (!capture || capture->self!=capture || !capture->valid || !context || !context->ready || !copy ||
        capture->command.opcode!=INTEL_UMD_COPY32 || context->program || context->owner!=capture->context.native.pid ||
        context->render!=(capture->context.native.node==INTEL_UMD_NODE_RENDER) ||
        context->source!=capture->resources[0].native.buffer || context->destination!=capture->resources[1].native.buffer)
        return STATUS_INVALID_PARAMETER;
    *copy=(intel_bcs_copy_t){context->source_gpu,context->source->bytes,
        context->destination_gpu,context->destination->bytes,
        capture->command.source_pitch,capture->command.destination_pitch,capture->command.width,capture->command.height,
        capture->command.reserved[0],capture->command.reserved[1],capture->command.reserved[2],capture->command.reserved[3]};
    return STATUS_SUCCESS;
}
NTSTATUS intel_gpu_capture_draw(const intel_gpu_capture_t *capture,const intel_bcs_context_t *context,
    intel_rcs_queue_draw_t *work)
{
    bool vertex=intel_gpu_capture_has_vertex(capture),indexed=intel_gpu_capture_has_indices(capture);
    unsigned geometry=3+vertex+indexed;
    bool sampled=capture && (capture->draw.version==INTEL_UMD_DRAW_SAMPLED_VERSION || capture->draw.version==INTEL_UMD_DRAW_VERTEX_VERSION || capture->draw.version==INTEL_UMD_DRAW_VERTEX_SHADER_VERSION || indexed);
    if (!capture || capture->self!=capture || !capture->valid || !context || !context->ready || !work ||
        capture->command.opcode!=INTEL_UMD_DRAW32 || capture->resource_count<geometry ||
        capture->resource_count>INTEL_UMD_MAX_RESOURCES || (!sampled && capture->resource_count!=3) || !context->render ||
        context->texture_count!=capture->resource_count-geometry ||
        capture->context.native.node!=INTEL_UMD_NODE_RENDER || context->owner!=capture->context.native.pid ||
        context->source!=capture->resources[0].native.buffer || context->destination!=capture->resources[1].native.buffer ||
        !context->program || context->program!=capture->resources[2].native.buffer) return STATUS_INVALID_PARAMETER;
    if (vertex!=!!context->vertex_program.buffer ||
        (vertex && context->vertex_program.buffer!=capture->resources[3].native.buffer)) return STATUS_INVALID_PARAMETER;
    if (indexed!=!!context->indices.buffer ||
        (indexed && context->indices.buffer!=capture->resources[3+vertex].native.buffer)) return STATUS_INVALID_PARAMETER;
    for (unsigned i=geometry;i<capture->resource_count;i++)
        if (context->textures[i-geometry].buffer!=capture->resources[i].native.buffer) return STATUS_INVALID_PARAMETER;
    const intel_umd_draw32_t *d=&capture->draw;
    *work=(intel_rcs_queue_draw_t){.program=capture->program,.primitive={
        {context->batch_gpu,context->program_gpu+capture->program.code_offset,context->destination_gpu,
            context->destination->bytes,d->destination_width,d->destination_height,d->destination_pitch,
            d->x,d->y,d->width,d->height,d->dispatch_mask,{0}},
        context->source_gpu,context->source->bytes,d->vertex_offset,d->vertex_stride,d->vertex_count,d->topology,{0}}};
    if (d->version==INTEL_UMD_DRAW_BLEND_VERSION || sampled)
        work->primitive.blend=(intel_umd_blend_t){d->reserved2[0],d->reserved2[1],d->reserved2[2],d->reserved2[3]};
    work->primitive.pixel.constants.count=capture->constants.parameter_count;
    if (indexed) work->indices=gpu_capture_index_binding(capture,context->indices.gpu,context->indices.buffer->bytes);
    const unsigned *values=indexed ? capture->indexed.values : vertex ? capture->vertex.values : sampled ? capture->sampled.values : capture->constants.values;
    for (unsigned i=0;i<capture->constants.parameter_count;i++)
        work->primitive.pixel.constants.values[i]=values[i];
    if (d->version==INTEL_UMD_DRAW_RECTANGLES_VERSION || d->version==INTEL_UMD_DRAW_BLEND_VERSION || sampled) {
        _Static_assert(INTEL_UMD_MAX_RECTANGLES==INTEL_RCS_MAX_SCISSORS,"owned list bound");
        const intel_umd_rectangle_t *r=(const void *)(values+capture->constants.parameter_count+(vertex ? capture->vertex.vertex_parameter_count : 0));
        work->scissor_count=capture->constants.reserved;
        for (unsigned i=0;i<work->scissor_count;i++)
            work->scissors[i]=(intel_rcs_scissor_t){r[i].x,r[i].y,r[i].width,r[i].height};
    }
    if (sampled) {
        work->sampled.bound_mask=capture->sampled.bound_mask;
        work->sampled.typed_vertices=d->version==INTEL_UMD_DRAW_VERTEX_VERSION || (indexed && !vertex);
        work->sampled.position_offset=capture->sampled.position_offset;
        for (unsigned i=0;i<8;i++) {
            unsigned packed=capture->sampled.texcoord_offset[i];
            work->sampled.texcoord_offset[i]=work->sampled.typed_vertices ? packed&0xffffu : packed;
            work->sampled.texcoord_type[i]=work->sampled.typed_vertices ? packed>>16 : 0;
        }
        for (unsigned i=0;i<INTEL_UMD_MAX_TEXTURES;i++) if (work->sampled.bound_mask&(1u<<i)) {
            const intel_umd_texture_t *t=&capture->sampled.textures[i];
            unsigned resource=capture->texture_resource[i];
            if (resource<geometry || resource>=capture->resource_count) return STATUS_INVALID_PARAMETER;
            intel_rcs_texture_binding_t *out=&work->sampled.textures[i];
            out->gpu=context->textures[resource-geometry].gpu;
            out->bytes=capture->resources[resource].native.buffer->bytes;
            if (!intel_gen9_image_layout(t->width,t->height,t->levels,t->format,t->pitch,&out->image))
                return STATUS_INVALID_PARAMETER;
            out->first=t->first;out->count=t->count;out->srgb=t->srgb;
            out->sampler=gpu_capture_sampler(&t->sampler);
        }
    }
    if (vertex) {
        const intel_umd_draw_vertex_shader_t *v=&capture->vertex;
        work->vertex_program=capture->vertex_program;
        work->vertex=(intel_rcs_vertex_binding_t){.program_gpu=context->vertex_program.gpu+capture->vertex_program.base.code_offset,
            .viewport_x=v->viewport_x,.viewport_y=v->viewport_y,.viewport_width=v->viewport_width,.viewport_height=v->viewport_height,
            .constants={.count=v->vertex_parameter_count}};
        for (unsigned i=0;i<16;i++) {
            work->vertex.attribute_offset[i]=v->attributes[i]&0xffffu;
            work->vertex.attribute_type[i]=v->attributes[i]>>16;
        }
        for (unsigned i=0;i<v->vertex_parameter_count;i++)work->vertex.constants.values[i]=values[v->parameter_count+i];
    }
    return STATUS_SUCCESS;
}
