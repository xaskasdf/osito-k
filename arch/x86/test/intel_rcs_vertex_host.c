#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_vertex.h"
#include "../drivers/intel_gen9_shader.h"
#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data,location,address,delta) ((void)(data),(void)(location),(address)+(delta))
#include "gen9-vertex-pack.h"
static unsigned checks,draws;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL %u %s\n",__LINE__,#x);exit(1);}} while (0)
static uint32_t batch[4096],previous[4096];
static void *load(const char *path,size_t *bytes)
{
    FILE *f=fopen(path,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>128);rewind(f);
    void *p=malloc(n);CHECK(p);CHECK(fread(p,1,n,f)==(size_t)n);fclose(f);*bytes=n;return p;
}
static void inspect(const intel_rcs_sampled_draw_t *d,const intel_rcs_vertex_binding_t *v,
    const struct intel_d3d9_vertex_program *vs,const struct intel_d3d9_program *ps)
{
    unsigned found=0,vc=(vs->base.curb_read_length+31)/32,pc=(ps->curb_read_length+31)/32;
    uint64_t base=v->program_gpu<d->primitive.pixel.program_gpu ? v->program_gpu : d->primitive.pixel.program_gpu;
    for (unsigned at=1;at<1024;) {
        unsigned op=batch[at],len=(op&255)+2;
        if (op==0x05000000) break;
        if (!op) {at++;continue;}
        CHECK((op>>29)==3 && at+len<=1024);
        uint32_t reference[16]={0};
        switch (op&0xffff0000u) {
        case 0x78150000: {
            struct GFX9_3DSTATE_CONSTANT_VS s={GFX9_3DSTATE_CONSTANT_VS_header,.MOCS=2};
            s.ConstantBody.ReadLength[3]=vs->base.curb_read_length;
            s.ConstantBody.Buffer[3]=d->primitive.pixel.batch_gpu+INTEL_RCS_VERTEX_CONSTANTS;
            GFX9_3DSTATE_CONSTANT_VS_pack(NULL,reference,&s);
            CHECK(len==11 && !memcmp(reference,batch+at,44));found|=1024;break;
        }
        case 0x78100000: {
            struct GFX9_3DSTATE_VS s={GFX9_3DSTATE_VS_header,.Enable=true,.SIMD8DispatchEnable=true,
                .KernelStartPointer=v->program_gpu-base,.FloatingPointMode=vs->base.flags,
                .DispatchGRFStartRegisterForURBData=2,.VertexURBEntryReadLength=vs->urb_read_length,
                .MaximumNumberofThreads=63,.VertexURBEntryOutputReadOffset=1,.VertexURBEntryOutputLength=2};
            GFX9_3DSTATE_VS_pack(NULL,reference,&s);CHECK(len==9 && !memcmp(reference,batch+at,36));found|=1;break;
        }
        case 0x78120000: {
            struct GFX9_3DSTATE_CLIP s={GFX9_3DSTATE_CLIP_header,.ClipEnable=true,.APIMode=APIMODE_D3D,
                .ViewportXYClipTestEnable=true,.ForceZeroRTAIndexEnable=true};
            GFX9_3DSTATE_CLIP_pack(NULL,reference,&s);CHECK(len==4 && !memcmp(reference,batch+at,16));found|=2;break;
        }
        case 0x78130000: {
            struct GFX9_3DSTATE_SF s={GFX9_3DSTATE_SF_header,.ViewportTransformEnable=true,.PointWidth=1,.PointWidthSource=1};
            GFX9_3DSTATE_SF_pack(NULL,reference,&s);CHECK(len==4 && !memcmp(reference,batch+at,16));found|=4;break;
        }
        case 0x78300000: {
            struct GFX9_3DSTATE_URB_VS s={GFX9_3DSTATE_URB_VS_header,.VSNumberofURBEntries=64,.VSURBEntryAllocationSize=vs->urb_entry_size-1,.VSURBStartingAddress=4};
            GFX9_3DSTATE_URB_VS_pack(NULL,reference,&s);CHECK(len==2 && !memcmp(reference,batch+at,8));found|=8;break;
        }
        case 0x79120000: {
            struct GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_VS s={GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_VS_header,.ConstantBufferSize=vc};
            GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_VS_pack(NULL,reference,&s);CHECK(!memcmp(reference,batch+at,8));found|=16;break;
        }
        case 0x79160000: {
            struct GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_PS s={GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_PS_header,.ConstantBufferOffset=vc,.ConstantBufferSize=pc};
            GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_PS_pack(NULL,reference,&s);CHECK(!memcmp(reference,batch+at,8));found|=32;break;
        }
        case 0x781f0000: {
            struct GFX9_3DSTATE_SBE s={GFX9_3DSTATE_SBE_header,.VertexURBEntryReadOffset=1,.VertexURBEntryReadLength=2,
                .AttributeSwizzleEnable=true,.NumberofSFOutputAttributes=ps->varying_inputs,
                .ForceVertexURBEntryReadOffset=true,.ForceVertexURBEntryReadLength=true,
                .ConstantInterpolationEnable=ps->layer_input<9 ? 1u<<ps->layer_input : 0};
            for (unsigned i=0;i<ps->varying_inputs;i++) s.AttributeActiveComponentFormat[i]=ACTIVE_COMPONENT_XYZW;
            GFX9_3DSTATE_SBE_pack(NULL,reference,&s);CHECK(!memcmp(reference,batch+at,24));found|=64;break;
        }
        case 0x78510000: {
            struct GFX9_3DSTATE_SBE_SWIZ s={GFX9_3DSTATE_SBE_SWIZ_header};
            for (unsigned i=0;i<ps->varying_inputs;i++) {
                if (i==ps->layer_input) {s.Attribute[i].ComponentOverrideX=s.Attribute[i].ComponentOverrideY=s.Attribute[i].ComponentOverrideZ=s.Attribute[i].ComponentOverrideW=true;}
                else s.Attribute[i].SourceAttribute=vs->output_slots[3]-2;
            }
            GFX9_3DSTATE_SBE_SWIZ_pack(NULL,reference,&s);CHECK(!memcmp(reference,batch+at,44));found|=128;break;
        }
        case 0x78200000:CHECK(batch[at+1]==ps->kernel_offset[0]+d->primitive.pixel.program_gpu-base);found|=256;break;
        case 0x78090000: {
            CHECK(len==5);
            for (unsigned i=0;i<2;i++) {
                unsigned reg=i ? 15 : 0;intel_gen9_vertex_format_t f;CHECK(intel_gen9_vertex_format(v->attribute_type[reg],&f));
                struct GFX9_VERTEX_ELEMENT_STATE s={.Valid=true,.SourceElementFormat=f.format,.SourceElementOffset=v->attribute_offset[reg],
                    .Component0Control=(f.control>>28)&7,.Component1Control=(f.control>>24)&7,
                    .Component2Control=(f.control>>20)&7,.Component3Control=(f.control>>16)&7};
                GFX9_VERTEX_ELEMENT_STATE_pack(NULL,reference,&s);CHECK(!memcmp(reference,batch+at+1+i*2,8));
            }
            found|=512;break;
        }
        default:break;
        }
        at+=len;
    }
    CHECK(found==2047);
    struct GFX9_SF_CLIP_VIEWPORT view={
        .ViewportMatrixElementm00=v->viewport_width/2.0f,.ViewportMatrixElementm11=-(float)v->viewport_height/2.0f,
        .ViewportMatrixElementm22=1,.ViewportMatrixElementm30=v->viewport_x+v->viewport_width/2.0f,
        .ViewportMatrixElementm31=v->viewport_y+v->viewport_height/2.0f,
        .XMinClipGuardband=-1,.XMaxClipGuardband=1,.YMinClipGuardband=-1,.YMaxClipGuardband=1,
        .XMinViewPort=v->viewport_x,.XMaxViewPort=v->viewport_x+v->viewport_width-1,
        .YMinViewPort=v->viewport_y,.YMaxViewPort=v->viewport_y+v->viewport_height-1};
    uint32_t reference[16];GFX9_SF_CLIP_VIEWPORT_pack(NULL,reference,&view);
    CHECK(!memcmp(batch+0x1300/4,reference,sizeof(reference)));
    CHECK(!memcmp(batch+INTEL_RCS_VERTEX_CONSTANTS/4,v->constants.values,v->constants.count*4));
    CHECK(!memcmp(batch+0x1800/4,d->primitive.pixel.constants.values,ps->parameter_count*4));
}
int main(int argc,char **argv)
{
    CHECK(argc==3);size_t vb,pb;
    struct intel_d3d9_vertex_program *vs=load(argv[1],&vb);
    struct intel_d3d9_program *ps=load(argv[2],&pb);
    CHECK(intel_gen9_vertex_shader_validate(vs,vs,vb));CHECK(intel_gen9_resource_shader_validate(ps,ps,pb));
    intel_rcs_sampled_draw_t d={.primitive={.pixel={.batch_gpu=0x1000000,.program_gpu=0x1100000,
        .destination_gpu=0x1200000,.destination_bytes=16384,.destination_width=64,.destination_height=64,.destination_pitch=256,
        .width=64,.height=64,.dispatch_mask=1},.vertices_gpu=0x1300000,.vertices_bytes=4096,.vertex_stride=64,.vertex_count=6,.topology=4},.bound_mask=1};
    d.primitive.pixel.constants.count=ps->parameter_count;
    for (unsigned i=0;i<ps->parameter_count;i++) d.primitive.pixel.constants.values[i]=0x3f000000+i;
    intel_rcs_texture_binding_t *t=&d.textures[0];t->gpu=0x2000000;
    CHECK(intel_gen9_image_layout(8,8,1,21,0,&t->image));t->bytes=t->image.bytes;t->count=1;
    t->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0};
    intel_rcs_vertex_binding_t v={.program_gpu=0x1110000,.viewport_x=5,.viewport_y=7,.viewport_width=41,.viewport_height=43};
    v.constants.count=vs->base.parameter_count;
    for (unsigned i=0;i<v.constants.count;i++) v.constants.values[i]=0x3e000000+i;
    v.attribute_type[0]=3;v.attribute_offset[0]=16;
    for (unsigned order=0;order<2;order++) for (unsigned type=0;type<17;type++) {
        v.program_gpu=order ? 0x10f0000 : 0x1110000;v.attribute_type[15]=type;
        CHECK(intel_rcs_vertex_build(batch,sizeof(batch),&d,ps,pb,&v,vs,vb,NULL,0));draws++;inspect(&d,&v,vs,ps);
        memcpy(previous,batch,sizeof(batch));
        for (unsigned bad=0;bad<12;bad++) {
            intel_rcs_vertex_binding_t broken=v;intel_rcs_sampled_draw_t draw=d;
            switch(bad) {
            case 0:broken.attribute_type[15]=17;break;
            case 1:broken.attribute_offset[0]=49;break;
            case 2:broken.viewport_width=0;break;
            case 3:broken.viewport_x=64;break;
            case 4:broken.program_gpu=draw.primitive.pixel.program_gpu;break;
            case 5:broken.program_gpu=draw.textures[0].gpu;break;
            case 6:broken.attribute_offset[14]=4;break;
            case 7:broken.constants.count++;break;
            case 8:draw.primitive.vertices_bytes=300;break;
            case 9:draw.texcoord_offset[0]=16;break;
            case 10:broken.program_gpu=UINT64_MAX-4095;break;
            case 11:broken.program_gpu=draw.primitive.vertices_gpu;break;
            }
            CHECK(!intel_rcs_vertex_build(batch,sizeof(batch),&draw,ps,pb,&broken,vs,vb,NULL,0));
            CHECK(!memcmp(previous,batch,sizeof(batch)));
        }
    }
    free(vs);free(ps);printf("VERTEX BUILDER checks=%u draws=%u failures=0\n",checks,draws);return 0;
}
