/* Native Gen9 image layout/packing. Field definitions: Intel Gen9 PRM,
 * in-tree gen90.xml and Intel ISL's GFX4_2D layout (HALIGN=VALIGN=4).
 * This prepares metadata only; sampling, filtering and sRGB run on the GPU. */
#include "intel_gen9_texture.h"
#include "intel_ppgtt.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

bool intel_gen9_image_layout(uint32_t w,uint32_t h,uint32_t levels,
    uint32_t format,uint32_t pitch,intel_gen9_image_t *output)
{
    if (!output || !w || !h || w>4096 || h>4096 || !levels ||
        levels>INTEL_GEN9_IMAGE_LEVELS ||
        (format!=INTEL_GEN9_IMAGE_BGRA8 && format!=INTEL_GEN9_IMAGE_BGRX8)) return false;
    unsigned last=w>h ? w : h,maximum=1;
    while (last>1) {last>>=1;maximum++;}
    if (levels>maximum) return false;
    intel_gen9_image_t image={0};
    image.width=w;image.height=h;image.levels=levels;image.format=format;
    uint32_t x=0,y=0,columns=0,rows=0;
    for (unsigned l=0;l<levels;l++) {
        uint32_t lw=w>>l,lh=h>>l;
        if (!lw) lw=1;
        if (!lh) lh=1;
        image.mip[l]=(intel_gen9_image_level_t){x,y,lw,lh,0};
        uint32_t aw=levels==1 ? lw : (lw+3)&~3u;
        uint32_t ah=levels==1 ? lh : (lh+3)&~3u;
        if (columns<x+aw) columns=x+aw;
        if (rows<y+ah) rows=y+ah;
        /* L1 is below L0, L2 right of L1, subsequent mips below L2. */
        if (l==1) x+=aw;
        else y+=ah;
    }
    if (!pitch) pitch=(columns*4+63)&~63u;
    if ((pitch&3) || pitch<columns*4 || pitch>262144) return false;
    uint64_t bytes=((uint64_t)pitch*rows+4095)&~4095ULL;
    if (bytes>(1ULL<<28)) return false;
    image.pitch=pitch;image.rows=rows;image.bytes=(uint32_t)bytes;
    for (unsigned l=0;l<levels;l++)
        image.mip[l].offset=image.mip[l].y*pitch+image.mip[l].x*4;
    *output=image;
    return true;
}

bool intel_gen9_image_state(const intel_gen9_image_t *input,uint64_t gpu,uint64_t bytes,
    uint32_t first,uint32_t count,uint32_t srgb,uint32_t state[16])
{
    if (!input || !state) return false;
    const intel_gen9_image_t image=*input;
    intel_gen9_image_t expected;
    if (!intel_gen9_image_layout(image.width,image.height,image.levels,image.format,
        image.pitch,&expected) || memcmp(&image,&expected,sizeof(image)) ||
        !gpu || (gpu&4095) || bytes<image.bytes || bytes>(1ULL<<28) || (bytes&4095) ||
        gpu>INTEL_PPGTT_VA_LIMIT-bytes || !count || first>=image.levels ||
        count>image.levels-first || srgb>1) return false;
    uint32_t format=(image.format==INTEL_GEN9_IMAGE_BGRA8 ? 192 : 233)+srgb;
    uint32_t out[16]={0};
    out[0]=(1u<<29)|(format<<18)|(1u<<16)|(1u<<14)|(1u<<8);
    out[1]=(2u<<24)|((image.rows+3)/4); /* UC MOCS 1, compact QPitch */
    out[2]=((image.height-1)<<16)|(image.width-1);
    out[3]=image.pitch-1;
    out[5]=(count-1)|(first<<4)|(image.levels<<8); /* no tiled mip tail */
    out[7]=(4u<<25)|(5u<<22)|(6u<<19)|(7u<<16);
    out[8]=(uint32_t)gpu;out[9]=(uint32_t)(gpu>>32);
    memcpy(state,out,sizeof(out));
    return true;
}

static uint32_t gen9_border_channel(uint32_t value)
{
    if (!value) return 0;
    unsigned exponent=127;
    while (value<255) {value<<=1;exponent--;}
    return (exponent<<23)+(uint32_t)((((uint64_t)value<<23)+127)/255)-0x800000;
}

bool intel_gen9_sampler_state(const intel_gen9_sampler_t *input,uint32_t border_offset,
    uint32_t dynamic_bytes,intel_gen9_sampler_state_t *output)
{
    if (!input || !output) return false;
    const intel_gen9_sampler_t d=*input;
    if (d.min_filter<1 || d.min_filter>2 || d.mag_filter<1 || d.mag_filter>2 ||
        d.mip_filter>2 || !d.address_u || d.address_u>5 || !d.address_v || d.address_v>5 ||
        !d.address_w || d.address_w>5 || d.lod_bias< -4096 || d.lod_bias>4095 ||
        d.min_lod>d.max_lod || d.max_lod>4095 || !border_offset || (border_offset&63) ||
        border_offset>0xffffc0 || border_offset>dynamic_bytes || dynamic_bytes-border_offset<16)
        return false;
    static const uint8_t address[]={0,0,1,2,4,5},mip[]={0,1,3};
    intel_gen9_sampler_state_t out={0};
    out.state[0]=(((uint32_t)d.lod_bias&8191)<<1)|((d.min_filter-1)<<14)|
        ((d.mag_filter-1)<<17)|(mip[d.mip_filter]<<20);
    /* DX10/OGL border representation: four IEEE binary32 components, also
     * suitable for D3D9 UNORM8 color after exact ARGB normalization. */
    out.state[1]=(d.min_lod<<20)|(d.max_lod<<8);
    out.state[2]=border_offset;
    out.state[3]=address[d.address_w]|(address[d.address_v]<<3)|(address[d.address_u]<<6);
    if (d.min_filter==2) out.state[3]|=(1u<<13)|(1u<<15)|(1u<<17);
    if (d.mag_filter==2) out.state[3]|=(1u<<14)|(1u<<16)|(1u<<18);
    const unsigned shift[]={16,8,0,24};
    for (unsigned i=0;i<4;i++) out.border[i]=gen9_border_channel((d.border_argb>>shift[i])&255);
    *output=out;
    return true;
}
