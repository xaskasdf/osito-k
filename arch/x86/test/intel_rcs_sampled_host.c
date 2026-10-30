#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_rcs_sampled.h"
#include "../drivers/intel_gen9_shader.h"
#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data,location,address,delta) ((void)(data),(void)(location),(address)+(delta))
#include "gen9-texture-pack.h"
static unsigned checks,draws;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL %u: %s\n",__LINE__,#x);exit(1);}} while (0)
static uint32_t storage[4098],before[4098];

static intel_rcs_sampled_draw_t fixture(const struct intel_d3d9_program *p)
{
    intel_rcs_sampled_draw_t d={0};
    d.primitive=(intel_rcs_primitive_draw_t){
        .pixel={.batch_gpu=0x1000000,.program_gpu=0x1100000,.destination_gpu=0x1200000,
            .destination_bytes=16384,.destination_width=64,.destination_height=64,.destination_pitch=256,
            .x=3,.y=5,.width=51,.height=53,.dispatch_mask=p->simd_mask,.constants={.count=p->parameter_count}},
        .vertices_gpu=0x1300000,.vertices_bytes=4096,.vertex_offset=32,.vertex_stride=144,
        .vertex_count=6,.topology=INTEL_RCS_TRIANGLE_LIST,
    };
    for (unsigned i=0;i<8;i++) d.texcoord_offset[i]=16+i*16;
    d.bound_mask=p->texture_mask;
    for (unsigned i=0;i<16;i++) if (d.bound_mask&(1u<<i)) {
        intel_rcs_texture_binding_t *t=&d.textures[i];
        t->gpu=0x2000000+i*0x20000;
        CHECK(intel_gen9_image_layout(17,11,5,21,0,&t->image));
        t->bytes=t->image.bytes;t->count=5;
        t->sampler=(intel_gen9_sampler_t){1,1,0,3,3,3,0,0,0,0xff112233};
    }
    return d;
}

static void inspect(const uint32_t *b,const intel_rcs_sampled_draw_t *d,
    const struct intel_d3d9_program *p,unsigned scissors)
{
    unsigned found=0,end=0,count=0,ve=0,highest=0;
    for (unsigned i=0;i<16;i++) if (p->sampler_mask&(1u<<i)) highest=i+1;
    for (unsigned at=1;at<1024;) {
        uint32_t op=b[at];unsigned len=(op&255)+2;
        if (op==0x05000000) {end=at;break;}
        if (!op) {at++;continue;} /* MI_NOOP padding from disabled fixed state */
        CHECK((op>>29)==3 && at+len<=1024);
        switch (op&0xffff0000u) {
        case 0x61010000:CHECK(b[at+13]==0x4001);found|=1;break;
        case 0x781f0000: {
            struct GFX9_3DSTATE_SBE s={GFX9_3DSTATE_SBE_header,
                .VertexURBEntryReadOffset=1,.VertexURBEntryReadLength=p->varying_inputs ? (p->varying_inputs+1)/2 : 1,
                .AttributeSwizzleEnable=p->varying_inputs!=0,.NumberofSFOutputAttributes=p->varying_inputs,
                .ForceVertexURBEntryReadOffset=true,.ForceVertexURBEntryReadLength=true,
                .ConstantInterpolationEnable=p->layer_input<9 ? 1u<<p->layer_input : 0,
            };
            for (unsigned i=0;i<p->varying_inputs;i++)s.AttributeActiveComponentFormat[i]=ACTIVE_COMPONENT_XYZW;
            uint32_t expected[6];GFX9_3DSTATE_SBE_pack(NULL,expected,&s);
            CHECK(len==6 && !memcmp(b+at,expected,sizeof(expected)));found|=2;break;
        }
        case 0x78510000: {
            struct GFX9_3DSTATE_SBE_SWIZ s={GFX9_3DSTATE_SBE_SWIZ_header};
            for (unsigned i=0;i<p->varying_inputs;i++) {
                if (i==p->layer_input) {
                    s.Attribute[i].ComponentOverrideX=true;s.Attribute[i].ComponentOverrideY=true;
                    s.Attribute[i].ComponentOverrideZ=true;s.Attribute[i].ComponentOverrideW=true;
                } else s.Attribute[i].SourceAttribute=i;
            }
            uint32_t expected[11];GFX9_3DSTATE_SBE_SWIZ_pack(NULL,expected,&s);
            CHECK(len==11 && !memcmp(b+at,expected,sizeof(expected)));found|=4;break;
        }
        case 0x78300000: {
            struct GFX9_3DSTATE_URB_VS s={GFX9_3DSTATE_URB_VS_header,
                .VSNumberofURBEntries=64,.VSURBEntryAllocationSize=p->varying_inputs>6 ? 2 : 1,
                .VSURBStartingAddress=4};
            uint32_t expected[2];GFX9_3DSTATE_URB_VS_pack(NULL,expected,&s);
            CHECK(len==2 && !memcmp(b+at,expected,sizeof(expected)));found|=8;break;
        }
        case 0x78140000:CHECK(b[at+1]==p->barycentric_modes<<11);found|=16;break;
        case 0x78200000:CHECK(((b[at+3]>>18)&255)==highest+1 && ((b[at+3]>>27)&7)==(highest+3)/4);found|=32;break;
        case 0x78090000: {
            unsigned elements=2+(p->varying_inputs ? p->varying_inputs : 1);
            CHECK(len==1+elements*2);
            uint32_t offset[11]={0},controls[11];
            for (unsigned i=0;i<11;i++) controls[i]=2;
            controls[1]=1;
            for (unsigned i=0;i<8;i++) if (p->texcoord_mask&(1u<<i)) {
                unsigned slot=((p->texcoord_map>>(i*4))&15)+2;
                offset[slot]=d->texcoord_offset[i];controls[slot]=1;
            }
            for (unsigned i=0;i<elements;i++) {
                struct GFX9_VERTEX_ELEMENT_STATE s={.Valid=true,.SourceElementOffset=offset[i],
                    .Component0Control=controls[i],.Component1Control=controls[i],
                    .Component2Control=controls[i],.Component3Control=controls[i]};
                uint32_t expected[2];GFX9_VERTEX_ELEMENT_STATE_pack(NULL,expected,&s);
                CHECK(!memcmp(expected,b+at+1+i*2,sizeof(expected)));
            }
            ve++;found|=64;break;
        }
        case 0x7b000000:count++;break;
        case 0x782a0000:CHECK(b[at+1]==INTEL_RCS_SAMPLED_BT);found|=128;break;
        case 0x782f0000:CHECK(b[at+1]==(highest ? INTEL_RCS_SAMPLED_SAMPLERS : 0));found|=256;break;
        default:break;
        }
        at+=len;
    }
    CHECK(found==511 && end && ve==1 && count==(scissors ? scissors : 1));
    CHECK(b[INTEL_RCS_SAMPLED_BT/4]==0x1040);
    for (unsigned i=0;i<16;i++) {
        unsigned binding=b[INTEL_RCS_SAMPLED_BT/4+1+i];
        if (p->texture_mask&(1u<<i)) {
            const intel_rcs_texture_binding_t *t=&d->textures[i];
            uint32_t surface[16];intel_gen9_sampler_state_t sampler;
            CHECK(binding==INTEL_RCS_SAMPLED_SURFACES+i*64);
            CHECK(intel_gen9_image_state(&t->image,t->gpu,t->bytes,t->first,t->count,t->srgb,surface));
            CHECK(!memcmp(b+binding/4,surface,64));
            CHECK(intel_gen9_sampler_state(&t->sampler,INTEL_RCS_SAMPLED_BORDERS+i*64,16384,&sampler));
            CHECK(!memcmp(b+INTEL_RCS_SAMPLED_SAMPLERS/4+i*4,sampler.state,16));
            CHECK(!memcmp(b+INTEL_RCS_SAMPLED_BORDERS/4+i*16,sampler.border,16));
        } else CHECK(!binding && b[INTEL_RCS_SAMPLED_SAMPLERS/4+i*4]==1u<<31);
    }
}

static void run(const intel_rcs_sampled_draw_t *d,const struct intel_d3d9_program *p,
    const intel_rcs_scissor_t *r,unsigned count)
{
    memset(storage,0xa5,sizeof(storage));
    CHECK(intel_rcs_sampled_build(storage+1,16384,d,p,p->allocation_bytes,r,count));
    CHECK(storage[0]==0xa5a5a5a5 && storage[4097]==0xa5a5a5a5);
    inspect(storage+1,d,p,count);draws++;
}

static void rejects(const struct intel_d3d9_program *program)
{
    intel_rcs_sampled_draw_t original=fixture(program);
    for (unsigned bad=0;bad<36;bad++) {
        intel_rcs_sampled_draw_t d=original;
        struct intel_d3d9_program p=*program;
        intel_rcs_scissor_t r={3,5,51,53};unsigned count=0;
        switch (bad) {
        case 0:d.primitive.pixel.batch_gpu=0;break;
        case 1:d.primitive.pixel.batch_gpu++;break;
        case 2:d.primitive.pixel.program_gpu=d.primitive.pixel.batch_gpu+8192;break;
        case 3:d.primitive.pixel.destination_gpu=d.primitive.pixel.batch_gpu+12288;break;
        case 4:d.primitive.vertices_gpu=d.primitive.pixel.batch_gpu+12288;break;
        case 5:d.primitive.pixel.destination_bytes=16383;break;
        case 6:d.primitive.pixel.destination_width=0;break;
        case 7:d.primitive.pixel.destination_pitch=255;break;
        case 8:d.primitive.pixel.x=UINT32_MAX;break;
        case 9:d.primitive.pixel.y=64;break;
        case 10:d.primitive.pixel.dispatch_mask=8;break;
        case 11:d.primitive.pixel.constants.count++;break;
        case 12:d.primitive.vertex_count=2;break;
        case 13:d.primitive.vertex_count=4;break;
        case 14:d.primitive.vertex_offset=UINT32_MAX-3;break;
        case 15:d.primitive.vertex_stride=15;break;
        case 16:d.texcoord_offset[0]=UINT32_MAX;break;
        case 17:d.texcoord_offset[0]=140;break;
        case 18:d.bound_mask=0;break;
        case 19:d.bound_mask=0x10001;break;
        case 20:d.textures[0].gpu=d.primitive.pixel.batch_gpu+12288;break;
        case 21:d.textures[0].gpu=d.primitive.pixel.program_gpu;break;
        case 22:d.textures[0].gpu=d.primitive.pixel.destination_gpu;break;
        case 23:d.textures[0].gpu=d.primitive.vertices_gpu;break;
        case 24:d.textures[0].bytes=1;break;
        case 25:d.textures[0].image.mip[1].offset++;break;
        case 26:d.textures[0].first=5;break;
        case 27:d.textures[0].sampler.min_filter=3;break;
        case 28:d.primitive.blend.flags=UINT32_MAX;break;
        case 29:count=33;break;
        case 30:count=1;r.width=0;break;
        case 31:count=1;r.x=2;break;
        case 32:p.abi=2;break;
        case 33:p.texcoord_map=0xfffffff9;break;
        case 34:d.textures[0].gpu=(1ULL<<47)-4096;d.textures[0].bytes=8192;break;
        case 35:d.primitive.vertices_bytes=32+5*144+31;break;
        }
        memset(storage,0xa5,sizeof(storage));memcpy(before,storage,sizeof(storage));
        CHECK(!intel_rcs_sampled_build(storage+1,16384,&d,&p,p.allocation_bytes,&r,count));
        CHECK(!memcmp(storage,before,sizeof(storage)));
    }
    CHECK(!intel_rcs_sampled_build(storage+1,16383,&original,program,program->allocation_bytes,NULL,0));
    CHECK(!intel_rcs_sampled_build(storage+1,16384,&original,program,program->allocation_bytes,NULL,1));
    CHECK(!intel_rcs_sampled_build(NULL,16384,&original,program,program->allocation_bytes,NULL,0));
    CHECK(!intel_rcs_sampled_build(storage+1,16384,NULL,program,program->allocation_bytes,NULL,0));
    /* Exhaust all eight coordinates/nine VUE attributes and sixteen slots.
     * This validates metadata packing only, not a compiled executable. */
    struct intel_d3d9_program p=*program;
    p.texcoord_mask=255;p.texcoord_map=0x76543210;p.varying_inputs=9;p.layer_input=8;
    p.texture_mask=p.sampler_mask=65535;
    intel_rcs_sampled_draw_t d=fixture(&p);
    run(&d,&p,NULL,0);
    /* Sharing a read-only image between sampler slots is permitted. */
    d.textures[15].gpu=d.textures[0].gpu;
    run(&d,&p,NULL,0);
}

int main(int argc,char **argv)
{
    CHECK(argc==2);
    const char *names[]={"ps20-sample","ps30-sample","ps30-sparse","ps30-pair","ps30-dependent",
        "ps30-tint","ps30-coordinate-swizzle","ps30-constant-coordinate","ps30-input-color",
        "ps30-unused-declaration","ps30-dead-sample","ps20-hlsl-sample","ps30-hlsl-pair"};
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        char path[1024];snprintf(path,sizeof(path),"%s/%s.program",argv[1],names[i]);
        FILE *f=fopen(path,"rb");CHECK(f!=NULL);CHECK(!fseek(f,0,SEEK_END));
        long n=ftell(f);CHECK(n>=128 && n<65536);rewind(f);
        void *blob=malloc((size_t)n);CHECK(blob!=NULL);CHECK(fread(blob,1,(size_t)n,f)==(size_t)n);fclose(f);
        const struct intel_d3d9_program *p=blob;
        CHECK(intel_gen9_resource_shader_validate(p,blob,(size_t)n));
        intel_rcs_sampled_draw_t d=fixture(p);
        intel_rcs_scissor_t rects[32];
        for (unsigned j=0;j<32;j++)rects[j]=(intel_rcs_scissor_t){3+j,5,1,53};
        for (unsigned mask=1;mask<8;mask++) if (!(mask&~p->simd_mask)) {
            d.primitive.pixel.dispatch_mask=mask;
            for (unsigned topology=4;topology<=6;topology++) {
                d.primitive.topology=topology;
                run(&d,p,NULL,0);run(&d,p,rects,32);
            }
        }
        if (i==0)rejects(p);
        free(blob);
    }
    printf("Gen9 sampled builder PASS checks=%u draws=%u compiled_programs=13 XML-packet reference\n",checks,draws);
    return 0;
}
