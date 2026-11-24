/* Native indexed encoding and DMA fixtures; CPU code only checks inputs and
 * analytic pixel expectations. No software vertex processing is submitted. */
#define main unused_vertex_fixture_main
#include "intel_rcs_vertex_fixture_host.c"
#undef main
#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data,location,address,delta) ((void)(data),(void)(location),(address)+(delta))
#include "gen9-index-pack.h"
static uint32_t index_batch[4096],saved_batch[4096];
static unsigned indexed_cases,invalid_cases;

static void inspect_index(const intel_rcs_indexed_test_t *t)
{
    const intel_rcs_index_binding_t *b=&t->indices;unsigned found=0;
    for(unsigned at=1;at<1024;) {
        uint32_t op=index_batch[at];if(op==0x05000000)break;
        if(!op){at++;continue;}
        unsigned length=(op&255)+2;CHECK((op>>29)==3 && at+length<=1024);
        uint32_t expected[8]={0};
        if((op&0xffff0000u)==0x780a0000) {
            struct GFX9_3DSTATE_INDEX_BUFFER p={GFX9_3DSTATE_INDEX_BUFFER_header,
                .MOCS=2,.IndexFormat=b->stride==2 ? INDEX_WORD : INDEX_DWORD,
                .BufferStartingAddress=b->gpu+b->offset,.BufferSize=b->count*b->stride};
            GFX9_3DSTATE_INDEX_BUFFER_pack(NULL,expected,&p);
            CHECK(length==5 && !memcmp(index_batch+at,expected,20));found|=1;
        } else if((op&0xffff0000u)==0x7b000000) {
            struct GFX9_3DPRIMITIVE p={GFX9_3DPRIMITIVE_header,.VertexAccessType=RANDOM,
                .VertexCountPerInstance=b->count,.InstanceCount=1,.BaseVertexLocation=b->base_vertex};
            GFX9_3DPRIMITIVE_pack(NULL,expected,&p);
            CHECK(length==7 && !memcmp(index_batch+at,expected,28));found|=2;
        } else if((op&0xffff0000u)==0x780c0000) {
            CHECK(length==2 && !index_batch[at+1]);found|=4; /* restart disabled */
        }
        at+=length;
    }
    CHECK(found==7);
}
static bool build_indexed(const intel_rcs_indexed_test_t *t,const struct intel_d3d9_program *ps,size_t pb)
{
    const struct intel_d3d9_vertex_program *vs=t->vertex_bytes ? (const void *)(t+1) : NULL;
    return intel_rcs_indexed_build(index_batch,sizeof(index_batch),&t->sampled.draw,ps,pb,
        vs ? &t->vertex : NULL,vs,t->vertex_bytes,&t->indices,
        (const uint8_t *)t->sampled.vertices+4096,4096,NULL,0);
}
static void indexed_emit(FILE *file,const struct intel_d3d9_program *ps,size_t pb,
    const struct intel_d3d9_vertex_program *vs,size_t vb,unsigned mode,unsigned stride,unsigned mask)
{
    intel_rcs_indexed_test_t *t=calloc(1,sizeof(*t)+vb);CHECK(t);
    if(vs) {memcpy(t+1,vs,vb);t->vertex_bytes=vb;}
    unsigned topology=mode==1 ? 5 : mode==2 ? 6 : 4;
    unsigned first_vertex=mode==1 ? 2 : 0;
    uint32_t minimum=mode==2 ? 2 : mode==3 ? 65532 : mode==4 ? 0x80000000u : 0;
    int32_t base=mode==1 ? 2 : mode==4 ? INT32_MIN : -(int32_t)minimum;
    intel_rcs_sampled_draw_t *d=&t->sampled.draw;
    d->primitive=(intel_rcs_primitive_draw_t){.pixel={.batch_gpu=0x200000,.program_gpu=0x230000,
        .destination_gpu=0x220000,.destination_bytes=32768,.destination_width=96,.destination_height=72,
        .destination_pitch=416,.width=96,.height=72,.dispatch_mask=mask},
        .vertices_gpu=0x250000,.vertices_bytes=4096,.vertex_stride=48,.vertex_offset=32,
        .vertex_count=first_vertex+4,.topology=topology};
    d->primitive.pixel.constants.count=ps->parameter_count;
    for(unsigned i=0;i<ps->parameter_count;i++)d->primitive.pixel.constants.values[i]=bits(1);
    d->bound_mask=1;
    intel_rcs_texture_binding_t *image=&d->textures[0];image->gpu=0x210000;image->count=1;
    CHECK(intel_gen9_image_layout(4,4,1,21,0,&image->image));image->bytes=image->image.bytes;
    image->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0};
    memset(t->sampled.source,0xa5,sizeof(t->sampled.source));
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
        t->sampled.source[y*image->image.pitch/4+x]=x==2 && y==1 ? 0xff00ff00u : 0xffff00ffu;
    unsigned variant=mode%4;
    if(vs) {
        t->vertex=(intel_rcs_vertex_binding_t){.program_gpu=0x240000,.viewport_x=8,.viewport_y=4,.viewport_width=64,.viewport_height=64};
        t->vertex.attribute_type[0]=t->vertex.attribute_type[15]=3;t->vertex.attribute_offset[0]=16;
        t->vertex.constants.count=vs->base.parameter_count;
        const uint32_t *map=(const void *)((const uint8_t *)vs+vs->base.parameter_map_offset);
        float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        if(variant==1){matrix[0]=matrix[5]=.5f;matrix[3]=-.25f;matrix[7]=.25f;}
        if(variant==2){matrix[0]=matrix[5]=2;}
        if(variant==3)matrix[15]=2;
        for(unsigned i=0;i<vs->base.parameter_count;i++){CHECK(map[i]<16);t->vertex.constants.values[i]=bits(matrix[map[i]]);}
    } else {
        d->typed_vertices=1;d->position_offset=16;d->texcoord_offset[0]=0;d->texcoord_type[0]=3;
    }
    const float xy[4][2]={{-1,1},{1,1},{-1,-1},{1,-1}};
    memset(t->sampled.vertices,0xa5,sizeof(t->sampled.vertices));
    for(unsigned i=0;i<4;i++) {
        float v[8]={.625f,.375f,0,1,xy[i][0],xy[i][1],.5f,1};
        if(!vs){v[4]=8+(xy[i][0]+1)*32;v[5]=4+(1-xy[i][1])*32;}
        memcpy((uint8_t *)t->sampled.vertices+32+(i+first_vertex)*48,v,sizeof(v));
    }
    t->indices=(intel_rcs_index_binding_t){.gpu=0x251000,.bytes=4096,.offset=3*stride,
        .count=topology==4 ? 6 : 4,.stride=stride,.base_vertex=base,.min_index=minimum,.num_vertices=4};
    const unsigned list[]={0,1,2,2,1,3},strip[]={0,1,2,3},fan[]={0,1,3,2};
    const unsigned *order=topology==4 ? list : topology==5 ? strip : fan;
    uint8_t *raw=(uint8_t *)t->sampled.vertices+4096+t->indices.offset;
    for(unsigned i=0;i<t->indices.count;i++)for(unsigned byte=0;byte<stride;byte++)
        raw[i*stride+byte]=(uint8_t)((minimum+order[i])>>(byte*8));
    unsigned left=vs && variant==1 ? 16 : vs && variant==3 ? 24 : 8;
    unsigned top=vs && variant==1 ? 12 : vs && variant==3 ? 20 : 4;
    unsigned width=vs && (variant==1 || variant==3) ? 32 : 64;
    for(unsigned i=0;i<8192;i++)t->sampled.expected[i]=0x5a5a5a5a;
    for(unsigned y=top;y<top+width;y++)for(unsigned x=left;x<left+width;x++)t->sampled.expected[y*104+x]=0xff00ff00;
    CHECK(build_indexed(t,ps,pb));inspect_index(t);memcpy(saved_batch,index_batch,sizeof(index_batch));
    CHECK(intel_rcs_indexed_test_validate(ps,pb,t,sizeof(*t)+vb,mask));
    for(unsigned bad=0;bad<22;bad++) {
        intel_rcs_indexed_test_t *b=malloc(sizeof(*t)+vb);CHECK(b);memcpy(b,t,sizeof(*t)+vb);
        switch(bad) {
        case 0:b->indices.stride=1;break;
        case 1:b->indices.offset++;break;
        case 2:b->indices.count=2;break;
        case 3:b->indices.count=65536;break;
        case 4:b->indices.bytes=b->indices.offset+b->indices.count*stride-1;break;
        case 5:b->indices.offset=0xfffffffc;break;
        case 6:b->indices.num_vertices=0;break;
        case 7:b->indices.base_vertex=INT32_MIN;b->indices.min_index=0;break;
        case 8:b->indices.base_vertex=INT32_MAX;break;
        case 9:b->indices.min_index=0xffffffff;break;
        case 10:b->indices.gpu=UINT64_MAX-4095;break;
        case 11:b->indices.gpu=b->sampled.draw.primitive.pixel.batch_gpu;break;
        case 12:b->indices.gpu=b->sampled.draw.primitive.pixel.program_gpu;break;
        case 13:b->indices.gpu=b->sampled.draw.primitive.pixel.destination_gpu;break;
        case 14:b->indices.gpu=b->sampled.draw.primitive.vertices_gpu;break;
        case 15:b->indices.gpu=b->sampled.draw.textures[0].gpu;break;
        case 16:b->indices.gpu=vs ? b->vertex.program_gpu : 0;break;
        case 17:b->sampled.draw.primitive.vertices_bytes=64;break;
        case 18:b->sampled.draw.primitive.vertex_count=0;break;
        case 19:b->sampled.draw.primitive.topology=3;break;
        case 20:{uint8_t *p=(uint8_t *)b->sampled.vertices+4096+b->indices.offset;
            for(unsigned byte=0;byte<stride;byte++)p[byte]=(uint8_t)((minimum+4)>>(8*byte));
            break;}
        case 21:b->indices.bytes=8192;break;
        }
        CHECK(!build_indexed(b,ps,pb));CHECK(!memcmp(saved_batch,index_batch,sizeof(index_batch)));
        free(b);invalid_cases++;
    }
    intel_rcs_program_test_case_t c={pb,0,mask,sizeof(*t)+vb};
    CHECK(fwrite(&c,1,sizeof(c),file)==sizeof(c));CHECK(fwrite(ps,1,pb,file)==pb);
    CHECK(fwrite(t,1,sizeof(*t)+vb,file)==sizeof(*t)+vb);free(t);indexed_cases++;
}
int main(int argc,char **argv)
{
    CHECK(argc==5);size_t pb,vb[2];struct intel_d3d9_program *ps=load(argv[1],&pb);
    struct intel_d3d9_vertex_program *vs[2]={load(argv[2],vb),load(argv[3],vb+1)};
    FILE *file=fopen(argv[4],"wb");CHECK(file);
    intel_rcs_program_test_header_t header={INTEL_RCS_PROGRAM_TEST_MAGIC,5,0,0};CHECK(fwrite(&header,1,sizeof(header),file)==sizeof(header));
    for(unsigned profile=0;profile<3;profile++)for(unsigned mode=0;mode<5;mode++)for(unsigned stride=2;stride<=4;stride+=2) {
        if(mode==4 && stride==2)continue;
        for(unsigned mask=1;mask<=4;mask<<=1)if(ps->simd_mask&mask)
            indexed_emit(file,ps,pb,profile ? vs[profile-1] : NULL,profile ? vb[profile-1] : 0,mode,stride,mask);
    }
    header.cases=indexed_cases;header.bytes=ftell(file);CHECK(!fseek(file,0,SEEK_SET));CHECK(fwrite(&header,1,sizeof(header),file)==sizeof(header));
    fclose(file);free(ps);free(vs[0]);free(vs[1]);
    printf("INDEXED checks=%u cases=%u rejected=%u failures=0\n",checks,indexed_cases,invalid_cases);return 0;
}
