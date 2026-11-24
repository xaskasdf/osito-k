#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../drivers/intel_gen9_texture.h"

#define __gen_address_type uint64_t
#define __gen_user_data void
#define __gen_combine_address(data,location,address,delta) ((void)(data),(void)(location),(address)+(delta))
#include "gen9-texture-pack.h"
#define MIN(a,b) MIN2(a,b)
#define MAX(a,b) MAX2(a,b)

/* Type/environment adapter for the two unchanged ISL reference functions.
 * The unsupported tiling/miptail branches fail if ever reached. */
struct isl_extent2d { uint32_t w,h; };
struct isl_extent3d { uint32_t w,h,d; };
struct isl_extent4d {
    union {uint32_t w,width;}; union {uint32_t h,height;};
    union {uint32_t d,depth;}; union {uint32_t a,array_len;};
};
struct isl_device {int unused;};
struct isl_surf_init_info {unsigned levels,format;};
struct isl_tile_info {struct isl_extent2d logical_extent_el;unsigned tiling;};
struct isl_format_layout {unsigned bw,bh,bd,bpb;};
enum isl_msaa_layout {ISL_MSAA_LAYOUT_NONE,ISL_MSAA_LAYOUT_ARRAY};
enum {ISL_SURF_DIM_2D,ISL_SURF_DIM_3D};
struct isl_surf {
    unsigned levels,dim,msaa_layout,samples,tiling,miptail_start_level,format;
    struct isl_extent4d logical_level0_px,phys_level0_sa;
};
static struct isl_format_layout ref_format={1,1,1,32};
#define isl_format_get_layout(f) ((void)(f),&ref_format)
#define isl_minify(n,l) MAX(1u,(n)>>(l))
#define isl_align_npot(n,a) ((((n)+(a)-1)/(a))*(a))
#define isl_tiling_is_std_y(t) ((void)(t),0)
#define isl_tiling_is_64(t) ((void)(t),0)
#define isl_surf_get_image_alignment_sa(s) ((void)(s),(struct isl_extent3d){4,4,1})
#define isl_surf_get_array_pitch_sa_rows(s) ((void)(s),0)
#define isl_get_miptail_level_offset_el(...) abort()
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "isl-texture-reference.inc"
#pragma GCC diagnostic pop

static unsigned checks,images,samplers;
#define CHECK(x) do {checks++;if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while (0)

static void image_case(unsigned w,unsigned h,unsigned levels,unsigned format,unsigned pitch)
{
    intel_gen9_image_t image;
    CHECK(intel_gen9_image_layout(w,h,levels,format,pitch,&image));
    struct isl_surf_init_info info={levels,format};
    struct isl_tile_info tile={.logical_extent_el={1,1}};
    struct isl_extent3d align={4,4,1};
    struct isl_extent4d base={.w=w,.h=h,.d=1,.a=1};
    struct isl_extent2d size;
    isl_calc_phys_slice0_extent_sa_gfx4_2d(NULL,&info,&tile,ISL_MSAA_LAYOUT_NONE,
        &align,&base,levels,&size);
    CHECK(image.rows==size.h);
    CHECK(image.pitch>=size.w*4);
    CHECK(image.bytes==(((uint64_t)image.pitch*size.h+4095)&~4095ULL));
    struct isl_surf surf={.levels=levels,.dim=ISL_SURF_DIM_2D,.samples=1,
        .miptail_start_level=levels,.logical_level0_px=base,.phys_level0_sa=base};
    for (unsigned l=0;l<levels;l++) {
        uint32_t x,y,z,a;
        get_image_offset_sa_gfx4_2d(&surf,l,0,&x,&y,&z,&a);
        CHECK(image.mip[l].x==x && image.mip[l].y==y && z==0 && a==0);
        CHECK(image.mip[l].offset==y*image.pitch+x*4);
        CHECK(image.mip[l].offset+(uint64_t)(image.mip[l].height-1)*image.pitch+
            image.mip[l].width*4<=image.bytes);
        for (unsigned j=0;j<l;j++) {
            const intel_gen9_image_level_t *u=&image.mip[l],*v=&image.mip[j];
            CHECK(u->x+u->width<=v->x || v->x+v->width<=u->x ||
                u->y+u->height<=v->y || v->y+v->height<=u->y);
        }
    }
    uint64_t gpu=(1ULL<<36)+0x10000;
    for (unsigned first=0;first<levels;first++) for (unsigned srgb=0;srgb<2;srgb++) {
        uint32_t actual[16],expected[16];
        CHECK(intel_gen9_image_state(&image,gpu,image.bytes,first,levels-first,srgb,actual));
        struct GFX9_RENDER_SURFACE_STATE fields={
            .SurfaceType=SURFTYPE_2D,.SurfaceFormat=(format==21 ? 192 : 233)+srgb,
            .SurfaceHorizontalAlignment=HALIGN_4,.SurfaceVerticalAlignment=VALIGN_4,
            .RenderCacheReadWriteMode=1,.MOCS=2,.SurfaceQPitch=(size.h+3)/4,
            .Width=w-1,.Height=h-1,.SurfacePitch=image.pitch-1,
            .MIPCountLOD=levels-first-1,.SurfaceMinLOD=first,.MipTailStartLOD=levels,
            .ShaderChannelSelectRed=4,.ShaderChannelSelectGreen=5,
            .ShaderChannelSelectBlue=6,.ShaderChannelSelectAlpha=7,.SurfaceBaseAddress=gpu,
        };
        GFX9_RENDER_SURFACE_STATE_pack(NULL,expected,&fields);
        CHECK(!memcmp(actual,expected,sizeof(actual)));
    }
    images++;
}

static void sampler_case(intel_gen9_sampler_t d)
{
    intel_gen9_sampler_state_t actual;
    CHECK(intel_gen9_sampler_state(&d,0x3fc0,0x4000,&actual));
    static const unsigned address[]={0,TCM_WRAP,TCM_MIRROR,TCM_CLAMP,TCM_CLAMP_BORDER,TCM_MIRROR_ONCE};
    static const unsigned mip[]={MIPFILTER_NONE,MIPFILTER_NEAREST,MIPFILTER_LINEAR};
    struct GFX9_SAMPLER_STATE fields={
        .TextureLODBias=d.lod_bias/256.0f,.MinModeFilter=d.min_filter-1,
        .MagModeFilter=d.mag_filter-1,.MipModeFilter=mip[d.mip_filter],
        .MinLOD=d.min_lod/256.0f,.MaxLOD=d.max_lod/256.0f,.BorderColorPointer=0x3fc0,
        .TCXAddressControlMode=address[d.address_u],.TCYAddressControlMode=address[d.address_v],
        .TCZAddressControlMode=address[d.address_w],
        .RAddressMinFilterRoundingEnable=d.min_filter==2,.VAddressMinFilterRoundingEnable=d.min_filter==2,
        .UAddressMinFilterRoundingEnable=d.min_filter==2,.RAddressMagFilterRoundingEnable=d.mag_filter==2,
        .VAddressMagFilterRoundingEnable=d.mag_filter==2,.UAddressMagFilterRoundingEnable=d.mag_filter==2,
    };
    uint32_t expected[4];
    GFX9_SAMPLER_STATE_pack(NULL,expected,&fields);
    CHECK(!memcmp(actual.state,expected,sizeof(expected)));
    const unsigned shift[]={16,8,0,24};
    for (unsigned i=0;i<4;i++) {
        float c=((d.border_argb>>shift[i])&255)/255.0f;
        uint32_t bits;
        memcpy(&bits,&c,4);
        CHECK(actual.border[i]==bits);
    }
    samplers++;
}

static void invalid_inputs(void)
{
    intel_gen9_image_t image,copy;
    memset(&image,0xa5,sizeof(image));copy=image;
    const uint32_t bad[][5]={{0,1,1,21,0},{1,0,1,21,0},{4097,1,1,21,0},
        {1,4097,1,21,0},{1,1,0,21,0},{1,1,2,21,0},{1,1,14,21,0},
        {1,1,1,23,0},{32,1,6,21,3},{32,1,6,21,16},{1,1,1,21,262148},
        {4096,4096,13,21,262144},{1,1,1,21,UINT32_MAX}};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        CHECK(!intel_gen9_image_layout(bad[i][0],bad[i][1],bad[i][2],bad[i][3],bad[i][4],&image));
        CHECK(!memcmp(&image,&copy,sizeof(image)));
    }
    CHECK(!intel_gen9_image_layout(1,1,1,21,0,NULL));
    CHECK(intel_gen9_image_layout(63,29,6,21,0,&image));
    uint32_t state[16],before[16];memset(state,0xa5,sizeof(state));memcpy(before,state,sizeof(state));
    /* Corrupt every canonical metadata word, including unused mips. */
    for (unsigned i=0;i<sizeof(image)/4;i++) {
        copy=image;uint32_t words[sizeof(image)/4];memcpy(words,&copy,sizeof(copy));
        words[i]^=0x80000000;memcpy(&copy,words,sizeof(copy));
        CHECK(!intel_gen9_image_state(&copy,0x10000,image.bytes,0,6,0,state));
        CHECK(!memcmp(state,before,sizeof(state)));
    }
    const uint64_t gpu_bad[]={0,1,0x10001,1ULL<<47,UINT64_MAX};
    for (unsigned i=0;i<sizeof(gpu_bad)/8;i++)
        CHECK(!intel_gen9_image_state(&image,gpu_bad[i],image.bytes,0,6,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,image.bytes-1,0,6,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,1ULL<<29,0,6,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,image.bytes,6,1,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,image.bytes,0,7,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,image.bytes,0,0,0,state));
    CHECK(!intel_gen9_image_state(&image,0x10000,image.bytes,0,6,2,state));
    CHECK(!memcmp(state,before,sizeof(state)));
    /* Metadata may alias output; it is snapshotted before writing. */
    copy=image;
    CHECK(intel_gen9_image_state(&copy,0x10000,image.bytes,0,6,0,(uint32_t *)&copy));
    CHECK(intel_gen9_image_state(&image,0x10000,image.bytes,0,6,0,state));
    CHECK(!memcmp(state,&copy,sizeof(state)));
    intel_gen9_sampler_t good={1,1,0,3,3,3,0,0,0,0},d;
    intel_gen9_sampler_state_t packed,unchanged;
    memset(&packed,0xa5,sizeof(packed));unchanged=packed;
#define BAD(field,value) do {d=good;d.field=(value); \
    CHECK(!intel_gen9_sampler_state(&d,64,4096,&packed)); \
    CHECK(!memcmp(&packed,&unchanged,sizeof(packed)));} while (0)
    BAD(min_filter,0);BAD(min_filter,3);BAD(mag_filter,0);BAD(mag_filter,3);
    BAD(mip_filter,3);BAD(address_u,0);BAD(address_u,6);BAD(address_v,0);BAD(address_v,6);
    BAD(address_w,0);BAD(address_w,6);BAD(lod_bias,-4097);BAD(lod_bias,4096);
    BAD(min_lod,1);BAD(max_lod,4096);
#undef BAD
    CHECK(!intel_gen9_sampler_state(&good,0,4096,&packed));
    CHECK(!intel_gen9_sampler_state(&good,65,4096,&packed));
    CHECK(!intel_gen9_sampler_state(&good,0x1000000,0x1000010,&packed));
    CHECK(!intel_gen9_sampler_state(&good,64,79,&packed));
    CHECK(!intel_gen9_sampler_state(&good,64,32,&packed));
    CHECK(!memcmp(&packed,&unchanged,sizeof(packed)));
    CHECK(intel_gen9_sampler_state(&good,0xffffc0,0xffffd0,&packed));
}

int main(void)
{
    const unsigned edge[]={1,2,3,4,5,7,8,15,16,17,31,32,33,63,64,65,255,256,257,1023,1024,4095,4096};
    for (unsigned a=0;a<sizeof(edge)/4;a++) for (unsigned b=0;b<sizeof(edge)/4;b++) {
        unsigned max=MAX(edge[a],edge[b]),levels=1;
        while (max>1) {max>>=1;levels++;}
        for (unsigned l=1;l<=levels;l++)
            image_case(edge[a],edge[b],l,21+(a&1),0);
    }
    image_case(67,33,7,21,512);image_case(1,1,1,22,4);
    uint32_t random=0x1289abcd;
    for (unsigned i=0;i<4000;i++) {
        random=random*1664525+1013904223;unsigned w=(random&4095)+1;
        random=random*1664525+1013904223;unsigned h=(random&4095)+1;
        unsigned max=MAX(w,h),levels=1;
        while (max>1) {max>>=1;levels++;}
        image_case(w,h,1+(i%levels),21+(i&1),0);
    }
    for (unsigned filter=0;filter<12;filter++) for (unsigned u=1;u<=5;u++)
        for (unsigned v=1;v<=5;v++) for (unsigned w=1;w<=5;w++) {
            intel_gen9_sampler_t d={1+(filter&1),1+((filter>>1)&1),filter/4,u,v,w,
                -4096+(int)(samplers%8192),samplers%1024,4095,
                0x01010101u*(samplers&255)};
            sampler_case(d);
        }
    const int biases[]={-4096,-1,0,1,4095};
    for (unsigned i=0;i<5;i++)
        sampler_case((intel_gen9_sampler_t){2,2,2,4,5,1,biases[i],4095,4095,0x12abcdef});
    invalid_inputs();
    printf("Gen9 texture PASS checks=%u images=%u samplers=%u ISL-layout/XML-pack references\n",checks,images,samplers);
    return 0;
}
