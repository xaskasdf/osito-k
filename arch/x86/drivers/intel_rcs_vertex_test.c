#include "intel_rcs_vertex_test.h"
#include "intel_gen9_shader.h"

bool intel_rcs_vertex_test_validate(const struct intel_d3d9_program *program,size_t program_bytes,
    const void *data,size_t bytes,uint32_t dispatch_mask)
{
    if (!data || ((uintptr_t)data&7) || bytes<sizeof(intel_rcs_vertex_test_t) ||
        bytes>sizeof(intel_rcs_vertex_test_t)+131072 ||
        !intel_gen9_resource_shader_validate(program,program,program_bytes)) return false;
    const intel_rcs_vertex_test_t *t=data;
    if (t->reserved || t->vertex_bytes!=bytes-sizeof(*t) || t->vertex.program_gpu!=0x240000) return false;
    const struct intel_d3d9_vertex_program *vs=(const void *)(t+1);
    if (!intel_gen9_vertex_shader_validate(vs,vs,t->vertex_bytes)) return false;
    const intel_rcs_sampled_draw_t *d=&t->sampled.draw;
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
    return intel_rcs_vertex_build(batch,sizeof(batch),d,program,program_bytes,&t->vertex,
        vs,t->vertex_bytes,NULL,0);
}

bool intel_rcs_indexed_test_validate(const struct intel_d3d9_program *program,size_t program_bytes,
    const void *data,size_t bytes,uint32_t dispatch_mask)
{
    if (!data || ((uintptr_t)data&7) || bytes<sizeof(intel_rcs_indexed_test_t) ||
        bytes>sizeof(intel_rcs_indexed_test_t)+131072 ||
        !intel_gen9_resource_shader_validate(program,program,program_bytes)) return false;
    const intel_rcs_indexed_test_t *t=data;
    if (t->reserved || t->vertex_bytes!=bytes-sizeof(*t) ||
        t->indices.gpu!=0x251000 || t->indices.bytes!=4096) return false;
    const struct intel_d3d9_vertex_program *vs=t->vertex_bytes ? (const void *)(t+1) : NULL;
    if (vs) {
        if (t->vertex.program_gpu!=0x240000 || !intel_gen9_vertex_shader_validate(vs,vs,t->vertex_bytes)) return false;
    } else {
        const uint8_t *b=(const void *)&t->vertex;
        for (unsigned i=0;i<sizeof(t->vertex);i++) if(b[i]) return false;
    }
    const intel_rcs_sampled_draw_t *d=&t->sampled.draw;
    const intel_rcs_primitive_draw_t *v=&d->primitive;
    const intel_rcs_pixel_draw_t *p=&v->pixel;
    if (p->batch_gpu!=0x200000 || p->program_gpu!=0x230000 || p->destination_gpu!=0x220000 ||
        p->destination_bytes!=INTEL_RCS_SAMPLED_TEST_TARGET_BYTES || p->destination_width!=96 ||
        p->destination_height!=72 || p->destination_pitch!=416 || p->dispatch_mask!=dispatch_mask ||
        v->vertices_gpu!=0x250000 || v->vertices_bytes!=4096) return false;
    for (unsigned i=0;i<16;i++) if (d->bound_mask&(1u<<i)) {
        const intel_rcs_texture_binding_t *b=&d->textures[i];
        if (b->gpu<0x210000 || b->gpu>0x215000 || !b->bytes || b->bytes>0x215000-b->gpu) return false;
    }
    uint32_t batch[INTEL_RCS_SAMPLED_BYTES/4];
    return intel_rcs_indexed_build(batch,sizeof(batch),d,program,program_bytes,vs ? &t->vertex : NULL,
        vs,t->vertex_bytes,&t->indices,(const uint8_t *)t->sampled.vertices+4096,4096,NULL,0);
}
