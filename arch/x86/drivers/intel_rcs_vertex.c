#include "intel_rcs_vertex.h"
#include "intel_gen9_shader.h"
#include "intel_ppgtt.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

static uint32_t vertex_half_integer(unsigned value)
{
    /* Exact binary32 n/2 for viewport integers <= 12288; no kernel FPU. */
    if (!value) return 0;
    unsigned bit=0;
    for (unsigned n=value;n>>=1;) bit++;
    return ((126+bit)<<23)|((value<<(23-bit))&0x7fffffu);
}

static bool rcs_vertex_build(uint32_t *batch,size_t bytes,const intel_rcs_sampled_draw_t *input,
    const struct intel_d3d9_program *pixel,size_t pixel_bytes,const intel_rcs_vertex_binding_t *binding,
    const struct intel_d3d9_vertex_program *program,size_t program_bytes,
    const intel_rcs_scissor_t *scissors,unsigned count,const intel_rcs_index_binding_t *indices)
{
    if (!input || !binding || ((uintptr_t)input&7) || ((uintptr_t)binding&7) ||
        !intel_gen9_vertex_program_validate(program,program_bytes) ||
        !intel_gen9_resource_program_validate(pixel,pixel_bytes)) return false;
    const intel_rcs_vertex_binding_t b=*binding;
    const intel_rcs_primitive_draw_t *draw=&input->primitive;
    const intel_rcs_pixel_draw_t *p=&draw->pixel;
    intel_rcs_vertex_state_t v={.program=*program,.program_gpu=b.program_gpu,.constants=b.constants};
    if (input->typed_vertices || input->position_offset || b.constants.count!=program->base.parameter_count ||
        !b.viewport_width || !b.viewport_height || b.viewport_width>p->destination_width ||
        b.viewport_height>p->destination_height || p->destination_width>4096 || p->destination_height>4096 ||
        b.viewport_x>p->destination_width-b.viewport_width || b.viewport_y>p->destination_height-b.viewport_height ||
        draw->vertex_stride<4 || draw->vertex_stride>2048 || (draw->vertex_stride&3)) return false;
    for (unsigned i=0;i<8;i++) if (input->texcoord_offset[i] || input->texcoord_type[i]) return false;
    unsigned element=0;
    for (unsigned i=0;i<16;i++) {
        if (!(program->input_mask&(1u<<i))) {
            if (b.attribute_offset[i] || b.attribute_type[i]) return false;
            continue;
        }
        intel_gen9_vertex_format_t f;
        unsigned offset=b.attribute_offset[i];
        if (!intel_gen9_vertex_format(b.attribute_type[i],&f) || f.bytes>draw->vertex_stride ||
            (offset&3) || offset>draw->vertex_stride-f.bytes) return false;
        v.attribute_offset[element]=offset;v.attribute_format[element]=f.format;v.attribute_control[element]=f.control;
        if (offset+f.bytes>v.vertex_bytes) v.vertex_bytes=offset+f.bytes;
        element++;
    }
    if (!element) v.vertex_bytes=4; /* A disabled-source dummy VF element. */
    unsigned output_extent=0;
    for (unsigned i=0;i<8;i++) if (pixel->texcoord_mask&(1u<<i)) {
        if (!(program->output_mask&(1u<<(3+i)))) return false;
        unsigned extent=program->output_slots[3+i]-1;
        if (extent>output_extent) output_extent=extent;
    }
    v.sbe_read_length=output_extent ? (output_extent+1)/2 : 1;
    uint64_t extent=(program->base.code_bytes+4095u)&~4095u;
    if (!b.program_gpu || (b.program_gpu&4095) || b.program_gpu>INTEL_PPGTT_VA_LIMIT-extent) return false;
    uint64_t other[]={p->batch_gpu,p->program_gpu,p->destination_gpu,draw->vertices_gpu};
    uint64_t size[]={INTEL_RCS_SAMPLED_BYTES,(pixel->code_bytes+4095u)&~4095u,p->destination_bytes,draw->vertices_bytes};
    for (unsigned i=0;i<4;i++) {
        if (other[i]>INTEL_PPGTT_VA_LIMIT || size[i]>INTEL_PPGTT_VA_LIMIT-other[i] ||
            (other[i]<b.program_gpu+extent && b.program_gpu<other[i]+size[i])) return false;
    }
    for (unsigned i=0;i<16;i++) if (input->bound_mask&(1u<<i)) {
        const intel_rcs_texture_binding_t *t=&input->textures[i];
        if (t->gpu>INTEL_PPGTT_VA_LIMIT || t->bytes>INTEL_PPGTT_VA_LIMIT-t->gpu ||
            (t->gpu<b.program_gpu+extent && b.program_gpu<t->gpu+t->bytes)) return false;
    }
    v.instruction_gpu=b.program_gpu<p->program_gpu ? b.program_gpu : p->program_gpu;
    uint64_t last=b.program_gpu+extent;
    if (p->program_gpu+size[1]>last) last=p->program_gpu+size[1];
    if (last-v.instruction_gpu>(1u<<28)) return false;
    v.instruction_bytes=(uint32_t)(last-v.instruction_gpu);
    v.pixel_offset=(uint32_t)(p->program_gpu-v.instruction_gpu);v.kernel_offset=(uint32_t)(b.program_gpu-v.instruction_gpu);
    v.viewport[0]=vertex_half_integer(b.viewport_width);
    v.viewport[1]=vertex_half_integer(b.viewport_height)|0x80000000u;
    v.viewport[2]=0x3f800000u;
    v.viewport[3]=vertex_half_integer(2*b.viewport_x+b.viewport_width);
    v.viewport[4]=vertex_half_integer(2*b.viewport_y+b.viewport_height);
    v.viewport[8]=v.viewport[10]=0xbf800000u;
    v.viewport[9]=v.viewport[11]=0x3f800000u;
    v.viewport[12]=vertex_half_integer(2*b.viewport_x);
    v.viewport[13]=vertex_half_integer(2*(b.viewport_x+b.viewport_width-1));
    v.viewport[14]=vertex_half_integer(2*b.viewport_y);
    v.viewport[15]=vertex_half_integer(2*(b.viewport_y+b.viewport_height-1));
    return intel_rcs_sampling_build(batch,bytes,input,pixel,pixel_bytes,scissors,count,&v,indices);
}

bool intel_rcs_vertex_build(uint32_t *batch,size_t bytes,const intel_rcs_sampled_draw_t *input,
    const struct intel_d3d9_program *pixel,size_t pixel_bytes,const intel_rcs_vertex_binding_t *binding,
    const struct intel_d3d9_vertex_program *program,size_t program_bytes,
    const intel_rcs_scissor_t *scissors,unsigned count)
{
    return rcs_vertex_build(batch,bytes,input,pixel,pixel_bytes,binding,program,program_bytes,scissors,count,NULL);
}

bool intel_rcs_indexed_build(uint32_t *batch,size_t bytes,const intel_rcs_sampled_draw_t *input,
    const struct intel_d3d9_program *pixel,size_t pixel_bytes,const intel_rcs_vertex_binding_t *binding,
    const struct intel_d3d9_vertex_program *program,size_t program_bytes,const intel_rcs_index_binding_t *indices,
    const void *data,size_t index_bytes,const intel_rcs_scissor_t *scissors,unsigned count)
{
    if (!input || ((uintptr_t)input&7) ||
        !intel_rcs_index_validate(indices,data,index_bytes,input->primitive.vertex_count,input->primitive.topology))
        return false;
    if (binding || program || program_bytes)
        return rcs_vertex_build(batch,bytes,input,pixel,pixel_bytes,binding,program,program_bytes,scissors,count,indices);
    return intel_rcs_sampling_build(batch,bytes,input,pixel,pixel_bytes,scissors,count,NULL,indices);
}
