/* Host bounds/encoding oracle only. GPU pixel execution is a separate test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/intel_bcs.h"
#include "../drivers/intel_rcs_draw.h"
#include "../drivers/intel_gen9_texture.h"
#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data,location,address,delta) ((void)(data),(void)(location),(address)+(delta))
#include "gen9-copy-pack.h"
static unsigned checks,copies;
#define CHECK(x) do { checks++; if (!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);} } while (0)
static uint32_t context[2048],ring[1024],batch[2048];

static void surface_check(const uint32_t *actual,uint64_t gpu,unsigned w,unsigned h,unsigned pitch)
{
    struct GFX9_RENDER_SURFACE_STATE ref={.SurfaceType=SURFTYPE_2D,.SurfaceFormat=0xc0,
        .SurfaceHorizontalAlignment=HALIGN_4,.SurfaceVerticalAlignment=VALIGN_4,
        .RenderCacheReadWriteMode=ReadWriteCache,.MOCS=2,.Width=w-1,.Height=h-1,
        .SurfacePitch=pitch-1,.MipTailStartLOD=1,.SurfaceBaseAddress=gpu,
        .ShaderChannelSelectRed=SCS_RED,.ShaderChannelSelectGreen=SCS_GREEN,
        .ShaderChannelSelectBlue=SCS_BLUE,.ShaderChannelSelectAlpha=SCS_ALPHA};
    uint32_t expected[16]; GFX9_RENDER_SURFACE_STATE_pack(NULL,expected,&ref);
    CHECK(!memcmp(actual,expected,sizeof(expected)));
}
static void copy_case(const intel_bcs_copy_t *c)
{
    uint64_t descriptor;
    CHECK(intel_bcs_build_copy(context,sizeof(context),ring,sizeof(ring),0x1000000,
        0x1002000,0x400000,1,123,c,&descriptor));
    CHECK(ring[0]==0x54f00008 && (ring[1]&0xffff)==c->destination_pitch);
    CHECK(((uint64_t)ring[5]<<32|ring[4])==c->destination);
    CHECK(((uint64_t)ring[9]<<32|ring[8])==c->source);
    unsigned x=ring[2]&0xffff,y=ring[2]>>16,rx=ring[3]&0xffff,by=ring[3]>>16;
    CHECK(x==c->destination_x && y==c->destination_y && rx-x==c->width && by-y==c->height);
    CHECK((ring[6]&0xffff)==c->source_x && (ring[6]>>16)==c->source_y && ring[7]==c->source_pitch);
    /* Saved-ring reuse must preserve all four captured coordinates. */
    uint32_t saved[16]; memcpy(saved,ring,sizeof(saved));
    context[1029]=context[1031]=64;
    uint32_t tail=0;
    CHECK(intel_bcs_append_copy(context,sizeof(context),ring,sizeof(ring),64,123,c,&tail));
    CHECK(tail==128 && !memcmp(ring+16,saved,sizeof(saved)));
    intel_rcs_draw_t d={.batch_gpu=0x300000000ULL,.source_gpu=c->source,.destination_gpu=c->destination,
        .source_bytes=c->source_bytes,.destination_bytes=c->destination_bytes,
        .source_width=c->source_x+c->width,.source_height=c->source_y+c->height,.source_pitch=c->source_pitch,
        .destination_width=c->destination_x+c->width,.destination_height=c->destination_y+c->height,
        .destination_pitch=c->destination_pitch,.source_x=c->source_x,.source_y=c->source_y,
        .destination_x=c->destination_x,.destination_y=c->destination_y,.width=c->width,.height=c->height};
    CHECK(intel_rcs_draw_build(batch,sizeof(batch),&d));
    surface_check(batch+0x1080/4,c->source,d.source_width,d.source_height,d.source_pitch);
    surface_check(batch+0x1040/4,c->destination,d.destination_width,d.destination_height,d.destination_pitch);
    CHECK(batch[0x1180/4]==(rx|(by<<16)) && batch[0x1180/4+3]==(x|(by<<16)) &&
        batch[0x1180/4+6]==(x|(y<<16)));
    /* Geometry coordinates reference exactly the requested source corners. */
    for (unsigned i=0;i<3;i++) {
        float u=(float)(c->source_x+(i==0?c->width:0))/d.source_width;
        float v=(float)(c->source_y+(i==2?0:c->height))/d.source_height;
        uint32_t ub,vb;memcpy(&ub,&u,4);memcpy(&vb,&v,4);
        CHECK(batch[0x1180/4+i*3+1]==ub && batch[0x1180/4+i*3+2]==vb);
    }
    copies++;
}
static void layout_cases(void)
{
    const unsigned dimensions[]={1,2,7,31,65,127,257,1023,2048,4096};
    for (unsigned i=0;i<sizeof(dimensions)/sizeof(*dimensions);i++) {
        unsigned w=dimensions[i],h=w==1?1:w-1,levels=1;
        for (unsigned n=w;n>1;n>>=1) levels++;
        intel_gen9_image_t image;
        CHECK(intel_gen9_image_layout(w,h,levels,21,0,&image));
        for (unsigned l=0;l<levels;l++) {
            intel_bcs_copy_t c={.source=0x100000000ULL,.source_bytes=image.bytes,
                .destination=0x200000000ULL,.source_pitch=image.pitch,.destination_pitch=image.pitch+64,
                .width=image.mip[l].width,.height=image.mip[l].height,
                .source_x=image.mip[l].x,.source_y=image.mip[l].y,
                .destination_x=image.mip[l].x+4,.destination_y=image.mip[l].y+4};
            c.destination_bytes=(uint64_t)(c.destination_y+c.height)*c.destination_pitch;
            copy_case(&c);
        }
    }
}
static void invalid_cases(void)
{
    intel_bcs_copy_t base={.source=0x100000000ULL,.source_bytes=1000,.destination=0x200000000ULL,
        .destination_bytes=1000,.source_pitch=128,.destination_pitch=128,.width=5,.height=3,
        .source_x=3,.source_y=2,.destination_x=7,.destination_y=4};
    uint32_t original_context[2048],original_ring[1024];
    memset(context,0xa5,sizeof(context));memset(ring,0x5a,sizeof(ring));
    memcpy(original_context,context,sizeof(context));memcpy(original_ring,ring,sizeof(ring));
    for (unsigned bad=0;bad<18;bad++) {
        intel_bcs_copy_t c=base;
        switch(bad) {
        case 0:c.source_x=UINT32_MAX;break;case 1:c.source_y=UINT32_MAX;break;
        case 2:c.destination_x=UINT32_MAX;break;case 3:c.destination_y=UINT32_MAX;break;
        case 4:c.source_x=28;break;case 5:c.destination_x=28;break;
        case 6:c.source_y=32765;break;case 7:c.destination_y=32765;break;
        case 8:c.source_bytes=543;break;case 9:c.destination_bytes=815;break;
        case 10:c.source_y=6;break;case 11:c.destination_y=6;break;
        case 12:c.width=UINT32_MAX;break;case 13:c.height=UINT32_MAX;break;
        case 14:c.source+=4;break;case 15:c.destination+=16;break;
        case 16:c.source_pitch=0;break;case 17:c.destination_pitch=0;break;
        }
        uint64_t descriptor=0xfeed;
        CHECK(!intel_bcs_build_copy(context,sizeof(context),ring,sizeof(ring),0x1000000,
            0x1002000,0x400000,1,123,&c,&descriptor));
        CHECK(descriptor==0xfeed && !memcmp(context,original_context,sizeof(context)) &&
            !memcmp(ring,original_ring,sizeof(ring)));
    }
    /* Exact last byte is valid for BCS; render surfaces need the full pitch. */
    base.source_bytes=544;base.destination_bytes=816;
    uint64_t descriptor;
    CHECK(intel_bcs_build_copy(context,sizeof(context),ring,sizeof(ring),0x1000000,
        0x1002000,0x400000,1,123,&base,&descriptor));
}
int main(void)
{
    layout_cases();invalid_cases();
    printf("Native copy rectangles: %u checks, %u mip rectangles; XML surface and float-coordinate oracles\n",checks,copies);
    return 0;
}
