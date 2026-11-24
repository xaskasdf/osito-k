#include "intel_rcs_sampled.h"
#include "intel_gen9_shader.h"
#include "intel_ppgtt.h"
#include "intel_rcs_vertex.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

static bool rcs_index_range(const intel_rcs_index_binding_t *index,uint32_t vertices,uint32_t topology)
{
    if (!index || ((uintptr_t)index&7) || !vertices ||
        (index->stride!=2 && index->stride!=4) || (index->offset&(index->stride-1)) ||
        !index->gpu || (index->gpu&4095) || !index->bytes || index->bytes>(1u<<28) ||
        index->gpu>INTEL_PPGTT_VA_LIMIT-index->bytes ||
        index->count<3 || index->count>65535 ||
        topology<INTEL_RCS_TRIANGLE_LIST || topology>INTEL_RCS_TRIANGLE_FAN ||
        (topology==INTEL_RCS_TRIANGLE_LIST && index->count%3) ||
        (uint64_t)index->offset+(uint64_t)index->count*index->stride>index->bytes ||
        !index->num_vertices || (uint64_t)index->min_index+index->num_vertices>(1ULL<<32)) return false;
    int64_t first=(int64_t)index->base_vertex+index->min_index;
    return first>=0 && (uint64_t)first+index->num_vertices<=vertices;
}
bool intel_rcs_index_validate(const intel_rcs_index_binding_t *index,const void *data,size_t bytes,
    uint32_t vertices,uint32_t topology)
{
    if (!data || !rcs_index_range(index,vertices,topology) || index->bytes>bytes ||
        (uintptr_t)data>((uintptr_t)-1)-bytes) return false;
    const volatile uint8_t *raw=(const volatile uint8_t *)data+index->offset;
    for (uint32_t i=0;i<index->count;i++,raw+=index->stride) {
        uint32_t value=raw[0]|((uint32_t)raw[1]<<8);
        if (index->stride==4) value|=((uint32_t)raw[2]<<16)|((uint32_t)raw[3]<<24);
        if (value<index->min_index || (uint64_t)value-index->min_index>=index->num_vertices) return false;
    }
    return true;
}

bool intel_rcs_sampling_build(uint32_t *batch,size_t bytes,const intel_rcs_sampled_draw_t *input,
    const struct intel_d3d9_program *program,size_t program_bytes,
    const intel_rcs_scissor_t *scissors,unsigned count,const intel_rcs_vertex_state_t *vertex,
    const intel_rcs_index_binding_t *indices)
{
    if (!batch || !input || ((uintptr_t)batch&3) || ((uintptr_t)input&7) ||
        bytes<INTEL_RCS_SAMPLED_BYTES || (uintptr_t)batch>((uintptr_t)-1)-INTEL_RCS_SAMPLED_BYTES ||
        !intel_gen9_resource_program_validate(program,program_bytes) ||
        count>INTEL_RCS_MAX_SCISSORS || (count && !scissors)) return false;
    const intel_rcs_sampled_draw_t all=*input;
    const intel_rcs_primitive_draw_t *d=&all.primitive;
    const intel_rcs_pixel_draw_t *v=&d->pixel;
    const struct intel_d3d9_program p=*program;
    intel_rcs_scissor_t list[INTEL_RCS_MAX_SCISSORS];
    if (count) memcpy(list,scissors,count*sizeof(*scissors));
    uint64_t gpu[]={v->batch_gpu,v->program_gpu,v->destination_gpu,d->vertices_gpu};
    uint64_t extent[]={INTEL_RCS_SAMPLED_BYTES,(p.code_bytes+4095)&~4095u,v->destination_bytes,d->vertices_bytes};
    for (unsigned i=0;i<4;i++) {
        if (!gpu[i] || (gpu[i]&4095) || !extent[i] || extent[i]>(1ULL<<28) ||
            gpu[i]>INTEL_PPGTT_VA_LIMIT-extent[i]) return false;
        for (unsigned j=0;j<i;j++)
            if (gpu[i]<gpu[j]+extent[j] && gpu[j]<gpu[i]+extent[i]) return false;
    }
    intel_rcs_index_binding_t index={0};
    if (indices) {
        if (!rcs_index_range(indices,d->vertex_count,d->topology)) return false;
        index=*indices;
        for (unsigned i=0;i<4;i++)
            if (index.gpu<gpu[i]+extent[i] && gpu[i]<index.gpu+index.bytes) return false;
        if (vertex && index.gpu<vertex->program_gpu+((vertex->program.base.code_bytes+4095u)&~4095u) &&
            vertex->program_gpu<index.gpu+index.bytes) return false;
    }
    if (!v->destination_width || v->destination_width>4096 || !v->destination_height ||
        v->destination_height>4096 || (v->destination_pitch&3) ||
        v->destination_pitch<v->destination_width*4 || v->destination_pitch>262144 ||
        (uint64_t)v->destination_pitch*v->destination_height>v->destination_bytes ||
        !v->width || !v->height || v->width>v->destination_width || v->height>v->destination_height ||
        v->x>v->destination_width-v->width || v->y>v->destination_height-v->height ||
        !v->dispatch_mask || (v->dispatch_mask&~p.simd_mask) || v->constants.count!=p.parameter_count ||
        d->vertex_stride<(vertex ? 4u : 16u) || d->vertex_stride>2048 || (d->vertex_stride&3) || (d->vertex_offset&3) ||
        (!indices && (d->vertex_count<3 || d->vertex_count>65535 ||
            (d->topology==INTEL_RCS_TRIANGLE_LIST && d->vertex_count%3))) ||
        d->topology<INTEL_RCS_TRIANGLE_LIST || d->topology>INTEL_RCS_TRIANGLE_FAN ||
        all.bound_mask!=p.texture_mask) return false;
    for (unsigned i=0;i<count;i++) {
        const intel_rcs_scissor_t *r=&list[i];
        if (!r->width || !r->height || r->width>v->width || r->height>v->height ||
            r->x<v->x || r->y<v->y || r->x-v->x>v->width-r->width || r->y-v->y>v->height-r->height)
            return false;
    }
    intel_rcs_sampling_state_t prepared={0};
    prepared.vertex=vertex;
    prepared.indices=indices ? &index : NULL;
    if (!vertex && (all.typed_vertices>1 || (!all.typed_vertices && all.position_offset) ||
        (all.position_offset&3) || all.position_offset>d->vertex_stride-16)) return false;
    prepared.mask=all.bound_mask;prepared.vertex_bytes=vertex ? vertex->vertex_bytes : all.position_offset+16;
    prepared.position_offset=all.position_offset;
    for (unsigned i=0;i<9;i++) prepared.attribute_offset[i]=UINT32_MAX;
    for (unsigned i=0;i<8;i++) if (p.texcoord_mask&(1u<<i)) {
        uint32_t offset=all.texcoord_offset[i],slot=(p.texcoord_map>>(i*4))&15;
        if (vertex) {prepared.attribute_offset[slot]=vertex->program.output_slots[3+i]-2;continue;}
        intel_gen9_vertex_format_t format;
        if (!intel_gen9_vertex_format(all.typed_vertices ? all.texcoord_type[i] : 3,&format) ||
            (offset&3) || offset>d->vertex_stride-format.bytes) return false;
        prepared.attribute_offset[slot]=offset;
        prepared.attribute_format[slot]=format.format; prepared.attribute_control[slot]=format.control;
        if (offset+format.bytes>prepared.vertex_bytes) prepared.vertex_bytes=offset+format.bytes;
    }
    if ((uint64_t)d->vertex_offset+(uint64_t)(d->vertex_count-1)*d->vertex_stride+
        prepared.vertex_bytes>d->vertices_bytes) return false;
    for (unsigned i=0;i<16;i++) if (all.bound_mask&(1u<<i)) {
        const intel_rcs_texture_binding_t *t=&all.textures[i];
        if (!intel_gen9_image_state(&t->image,t->gpu,t->bytes,t->first,t->count,t->srgb,prepared.surface[i]) ||
            !intel_gen9_sampler_state(&t->sampler,INTEL_RCS_SAMPLED_BORDERS+i*64,
                INTEL_RCS_SAMPLED_BYTES,&prepared.sampler[i])) return false;
        for (unsigned j=0;j<4;j++)
            if (t->gpu<gpu[j]+extent[j] && gpu[j]<t->gpu+t->bytes) return false;
        if (indices && t->gpu<index.gpu+index.bytes && index.gpu<t->gpu+t->bytes) return false;
        prepared.count=i+1;
    }
    return intel_rcs_bound_emit(batch,d,&p,list,count,&prepared);
}

bool intel_rcs_sampled_build(uint32_t *batch,size_t bytes,const intel_rcs_sampled_draw_t *input,
    const struct intel_d3d9_program *program,size_t program_bytes,
    const intel_rcs_scissor_t *scissors,unsigned count)
{
    return intel_rcs_sampling_build(batch,bytes,input,program,program_bytes,scissors,count,NULL,NULL);
}
