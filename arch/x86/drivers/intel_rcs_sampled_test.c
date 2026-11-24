#include "intel_rcs_sampled_test.h"
#include "intel_gen9_shader.h"

bool intel_rcs_sampled_test_validate(const struct intel_d3d9_program *program,size_t program_bytes,
    const void *data,size_t bytes,uint32_t dispatch_mask)
{
    if (!data || ((uintptr_t)data&7) || bytes!=sizeof(intel_rcs_sampled_test_t) ||
        !intel_gen9_resource_shader_validate(program,program,program_bytes)) return false;
    const intel_rcs_sampled_test_t *t=data;
    const intel_rcs_sampled_draw_t *d=&t->draw;
    const intel_rcs_primitive_draw_t *v=&d->primitive;
    const intel_rcs_pixel_draw_t *p=&v->pixel;
    if (p->batch_gpu!=0x200000 || p->program_gpu!=0x230000 || p->destination_gpu!=0x220000 ||
        p->destination_bytes!=INTEL_RCS_SAMPLED_TEST_TARGET_BYTES || p->destination_width!=96 ||
        p->destination_height!=72 || p->destination_pitch!=416 || p->dispatch_mask!=dispatch_mask ||
        v->vertices_gpu!=0x250000 || v->vertices_bytes!=INTEL_RCS_SAMPLED_TEST_VERTEX_BYTES) return false;
    for (unsigned i=0;i<16;i++) if (d->bound_mask&(1u<<i)) {
        const intel_rcs_texture_binding_t *b=&d->textures[i];
        if (b->gpu<0x210000 || b->gpu>0x215000 || !b->bytes || b->bytes>0x215000-b->gpu)
            return false;
    }
    uint32_t batch[INTEL_RCS_SAMPLED_BYTES/4];
    if (!intel_rcs_sampled_build(batch,sizeof(batch),d,program,program_bytes,NULL,0)) return false;
    /* Validate the exact bytes the GPU will fetch, after range validation.
     * No CPU vertex transformation or shader execution occurs here. */
    const uint8_t *vertices=(const void *)t->vertices;
    for (unsigned i=0;i<v->vertex_count;i++) {
        const uint32_t *position=(const void *)(vertices+v->vertex_offset+i*v->vertex_stride);
        if ((position[0]&0x7fffffffu)>0x46800000u || (position[1]&0x7fffffffu)>0x46800000u ||
            position[2] || position[3]!=0x3f800000u) return false;
        for (unsigned a=0;a<8;a++) if (program->texcoord_mask&(1u<<a)) {
            const uint32_t *coord=(const void *)((const uint8_t *)position+d->texcoord_offset[a]);
            for (unsigned c=0;c<4;c++) if ((coord[c]&0x7fffffffu)>=0x7f800000u) return false;
        }
    }
    return true;
}
