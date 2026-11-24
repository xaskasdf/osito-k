#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_vertex_test.h"
#include "../drivers/intel_rcs_program_test.h"
#include "../drivers/intel_gen9_shader.h"
static unsigned checks,cases;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);exit(1);}} while (0)
static void *load(const char *path,size_t *bytes)
{
    FILE *f=fopen(path,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>128);rewind(f);
    void *p=malloc(n);CHECK(p);CHECK(fread(p,1,n,f)==(size_t)n);fclose(f);*bytes=n;return p;
}
static uint32_t bits(float f) {uint32_t v;memcpy(&v,&f,4);return v;}
static void emit(FILE *file,const struct intel_d3d9_program *ps,size_t pb,
    const struct intel_d3d9_vertex_program *vs,size_t vb,unsigned variant,unsigned mask)
{
    intel_rcs_vertex_test_t *t=calloc(1,sizeof(*t)+vb);CHECK(t);memcpy(t+1,vs,vb);t->vertex_bytes=vb;
    intel_rcs_sampled_draw_t *d=&t->sampled.draw;
    d->primitive=(intel_rcs_primitive_draw_t){.pixel={.batch_gpu=0x200000,.program_gpu=0x230000,
        .destination_gpu=0x220000,.destination_bytes=32768,.destination_width=96,.destination_height=72,
        .destination_pitch=416,.width=96,.height=72,.dispatch_mask=mask},
        .vertices_gpu=0x250000,.vertices_bytes=8192,.vertex_stride=48,.vertex_offset=32,.vertex_count=6,.topology=4};
    d->primitive.pixel.constants.count=ps->parameter_count;
    for (unsigned i=0;i<ps->parameter_count;i++)d->primitive.pixel.constants.values[i]=bits(1);
    d->bound_mask=1;
    intel_rcs_texture_binding_t *image=&d->textures[0];image->gpu=0x210000;image->count=1;
    CHECK(intel_gen9_image_layout(4,4,1,21,0,&image->image));image->bytes=image->image.bytes;
    image->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0};
    /* Distinct exact UNORM colors. Only (2,1) is green: wrong varying/slot
     * mappings cannot silently pass by sampling a constant-color image. */
    memset(t->sampled.source,0xa5,sizeof(t->sampled.source));
    for (unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
        t->sampled.source[y*image->image.pitch/4+x]=x==2 && y==1 ? 0xff00ff00u : 0xffff00ffu;
    t->vertex=(intel_rcs_vertex_binding_t){.program_gpu=0x240000,.viewport_x=8,.viewport_y=4,.viewport_width=64,.viewport_height=64};
    t->vertex.attribute_type[0]=t->vertex.attribute_type[15]=3;t->vertex.attribute_offset[0]=16;
    t->vertex.constants.count=vs->base.parameter_count;
    const uint32_t *map=(const void *)((const uint8_t *)vs+vs->base.parameter_map_offset);
    float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    if (variant==1) {matrix[0]=matrix[5]=.5f;matrix[3]=-.25f;matrix[7]=.25f;}
    if (variant==2) {matrix[0]=matrix[5]=2;}
    if (variant==3) {matrix[15]=2;}
    for (unsigned i=0;i<vs->base.parameter_count;i++) {CHECK(map[i]<16);t->vertex.constants.values[i]=bits(matrix[map[i]]);}
    const float xy[6][2]={{-1,1},{1,1},{-1,-1},{-1,-1},{1,1},{1,-1}};
    for (unsigned i=0;i<6;i++) {
        float v[8]={.625f,.375f,0,1,xy[i][0],xy[i][1],.5f,1};
        memcpy((uint8_t *)t->sampled.vertices+32+i*48,v,sizeof(v));
    }
    unsigned left=variant==1 ? 16 : variant==3 ? 24 : 8;
    unsigned top=variant==1 ? 12 : variant==3 ? 20 : 4;
    unsigned width=variant==1 || variant==3 ? 32 : 64;
    for (unsigned i=0;i<8192;i++)t->sampled.expected[i]=0x5a5a5a5a;
    for (unsigned y=top;y<top+width;y++)for(unsigned x=left;x<left+width;x++)t->sampled.expected[y*104+x]=0xff00ff00;
    CHECK(intel_rcs_vertex_test_validate(ps,pb,t,sizeof(*t)+vb,mask));
    for (unsigned bad=0;bad<10;bad++) {
        intel_rcs_vertex_test_t *b=malloc(sizeof(*t)+vb);CHECK(b);memcpy(b,t,sizeof(*t)+vb);
        switch(bad) {
        case 0:b->reserved=1;break;
        case 1:b->vertex_bytes--;break;
        case 2:b->vertex.program_gpu=0x260000;break;
        case 3:b->sampled.draw.primitive.pixel.destination_gpu+=4096;break;
        case 4:b->sampled.draw.primitive.vertices_gpu+=4096;break;
        case 5:b->sampled.draw.textures[0].gpu=0x200000;break;
        case 6:((struct intel_d3d9_vertex_program *)(b+1))->base.stage=1;break;
        case 7:b->vertex.constants.count++;break;
        case 8:b->vertex.viewport_width=0;break;
        case 9:b->sampled.draw.primitive.vertices_bytes--;break;
        }
        CHECK(!intel_rcs_vertex_test_validate(ps,pb,b,sizeof(*t)+vb,mask));free(b);
    }
    CHECK(!intel_rcs_vertex_test_validate(ps,pb,t,sizeof(*t)-1,mask));
    intel_rcs_program_test_case_t c={pb,0,mask,sizeof(*t)+vb};
    CHECK(fwrite(&c,1,sizeof(c),file)==sizeof(c));CHECK(fwrite(ps,1,pb,file)==pb);
    CHECK(fwrite(t,1,sizeof(*t)+vb,file)==sizeof(*t)+vb);free(t);cases++;
}
int main(int argc,char **argv)
{
    CHECK(argc==5);size_t pb,vb[2];
    struct intel_d3d9_program *ps=load(argv[1],&pb);
    struct intel_d3d9_vertex_program *vs[2]={load(argv[2],vb),load(argv[3],vb+1)};
    FILE *f=fopen(argv[4],"wb");CHECK(f);
    intel_rcs_program_test_header_t header={INTEL_RCS_PROGRAM_TEST_MAGIC,4,0,0};CHECK(fwrite(&header,1,sizeof(header),f)==sizeof(header));
    for (unsigned profile=0;profile<2;profile++)for(unsigned variant=0;variant<4;variant++)
        for(unsigned mask=1;mask<=4;mask<<=1) if(ps->simd_mask&mask)emit(f,ps,pb,vs[profile],vb[profile],variant,mask);
    header.cases=cases;header.bytes=ftell(f);CHECK(!fseek(f,0,SEEK_SET));CHECK(fwrite(&header,1,sizeof(header),f)==sizeof(header));
    fclose(f);free(ps);free(vs[0]);free(vs[1]);printf("VERTEX FIXTURES checks=%u cases=%u failures=0\n",checks,cases);return 0;
}
