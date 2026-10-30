/* Native command packing, checked against Intel KBL PRM and IGT v1.28
 * rendercopy_gen9.c. The four-instruction shader is from IGT, under the
 * accompanying IGT-LICENSE.txt. No Direct3D runtime is implemented here. */
#include "intel_rcs_draw.h"
#include "intel_ppgtt.h"
#include "intel_gen9_blend.h"
#include "intel_rcs_sampled.h"
#include "intel_rcs_vertex.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

static uint32_t rcs_draw_coord(uint32_t value,uint32_t extent)
{
    /* Exact integer ratio rounded to binary32; no kernel FPU state needed. */
    if (!value) return 0;
    unsigned exponent=127;
    while (value<extent) { value<<=1; exponent--; }
    uint64_t fraction=(((uint64_t)value<<23)+extent/2)/extent;
    return (exponent<<23)+(uint32_t)fraction-0x800000;
}

static void rcs_draw_surface(uint32_t *s,uint64_t gpu,uint32_t w,uint32_t h,uint32_t pitch)
{
    s[0]=(1u<<29)|(0xc0u<<18)|(1u<<16)|(1u<<14)|(1u<<8);
    s[1]=2u<<24; /* native UC MOCS index 1 */
    s[2]=((h-1)<<16)|(w-1); s[3]=pitch-1;
    s[5]=1u<<8;
    s[7]=(4u<<25)|(5u<<22)|(6u<<19)|(7u<<16);
    s[8]=(uint32_t)gpu; s[9]=(uint32_t)(gpu>>32);
}

static bool rcs_draw_emit(uint32_t *batch,const intel_rcs_draw_t *input,
    const struct intel_d3d9_program *pixel,uint64_t program_gpu,uint32_t dispatch_mask,
    const intel_rcs_primitive_draw_t *primitive,const intel_rcs_constants_t *constants,
    const intel_rcs_scissor_t *scissors,unsigned scissor_count,const intel_rcs_sampling_state_t *sampling)
{
    const intel_rcs_draw_t d=*input;
    const intel_rcs_vertex_state_t *vs=sampling ? sampling->vertex : NULL;
    const intel_rcs_index_binding_t *indices=sampling ? sampling->indices : NULL;
    const intel_umd_blend_t opaque={0};
    intel_gen9_blend_state_t blend;
    if (!intel_gen9_blend_pack(primitive ? &primitive->blend : &opaque,&blend)) return false;
    memset(batch,0,sampling ? INTEL_RCS_SAMPLED_BYTES : INTEL_RCS_DRAW_BYTES);
    enum { BT=0x1000,DST=0x1040,SRC=0x1080,SAMPLER=0x10c0,SHADER=0x1100,
           VB=0x1180,CC=0x11c0,BLEND=0x1200,CCVP=0x12c0,SFVP=0x1300,SCISSOR=0x1340,
           SCISSORS=0x1400,CONSTANTS=0x1800 };
    _Static_assert(SCISSORS+INTEL_RCS_MAX_SCISSORS*32==CONSTANTS,"scissor state bound");
    _Static_assert(CONSTANTS+INTEL_D3D9_PROGRAM_MAX_PARAMETERS*4==INTEL_RCS_DRAW_BYTES,"push payload bound");
    if (constants && constants->count)
        memcpy(batch+CONSTANTS/4,constants->values,constants->count*4);
    if (vs && vs->constants.count)
        memcpy(batch+INTEL_RCS_VERTEX_CONSTANTS/4,vs->constants.values,vs->constants.count*4);
    batch[BT/4]=DST; batch[BT/4+1]=pixel ? 0 : SRC;
    if (sampling) {
        batch[INTEL_RCS_SAMPLED_BT/4]=DST;
        for (unsigned i=0;i<16;i++) {
            batch[INTEL_RCS_SAMPLED_SAMPLERS/4+i*4]=1u<<31; /* disable unbound slots */
            if (!(sampling->mask&(1u<<i))) continue;
            batch[INTEL_RCS_SAMPLED_BT/4+1+i]=INTEL_RCS_SAMPLED_SURFACES+i*64;
            memcpy(batch+INTEL_RCS_SAMPLED_SURFACES/4+i*16,sampling->surface[i],64);
            memcpy(batch+INTEL_RCS_SAMPLED_SAMPLERS/4+i*4,sampling->sampler[i].state,16);
            memcpy(batch+INTEL_RCS_SAMPLED_BORDERS/4+i*16,sampling->sampler[i].border,16);
        }
    }
    rcs_draw_surface(batch+DST/4,d.destination_gpu,d.destination_width,d.destination_height,d.destination_pitch);
    batch[DST/4]=(batch[DST/4]&~(0x1ffu<<18))|(blend.format<<18);
    if (!pixel) {
        rcs_draw_surface(batch+SRC/4,d.source_gpu,d.source_width,d.source_height,d.source_pitch);
        batch[SAMPLER/4+3]=2|(2<<3)|(2<<6); /* nearest, clamp, normalized */
    }
    static const uint32_t shader[]={
        0x0080005a,0x2f403ae8,0x3a0000c0,0x008d0040,
        0x0080005a,0x2f803ae8,0x3a0000d0,0x008d0040,
        0x02800031,0x2e203a48,0x0e8d0f40,0x08840001,
        0x05800031,0x20003a40,0x0e8d0e20,0x90031000
    };
    if (!pixel) memcpy(batch+SHADER/4,shader,sizeof(shader));
    uint32_t *v=batch+VB/4;
    const uint32_t vx[]={d.width,0,0},vy[]={d.height,d.height,0};
    for (unsigned i=0;i<3;i++) {
        v[i*3]=(d.destination_x+vx[i])|((d.destination_y+vy[i])<<16);
        v[i*3+1]=pixel ? 0 : rcs_draw_coord(d.source_x+vx[i],d.source_width);
        v[i*3+2]=pixel ? 0 : rcs_draw_coord(d.source_y+vy[i],d.source_height);
    }
    for (unsigned i=0;i<16;i++) {
        batch[BLEND/4+1+i*2]=(0x11u<<21)|(1u<<26);
        batch[BLEND/4+2+i*2]=2; /* pre-blend clamp; blend disabled */
    }
    batch[BLEND/4]=blend.header;
    batch[BLEND/4+1]=blend.entry[0];batch[BLEND/4+2]=blend.entry[1];
    for (unsigned i=0;i<4;i++) batch[CC/4+2+i]=blend.constant[i];
    batch[CCVP/4]=0xff7fffff; batch[CCVP/4+1]=0x7f7fffff;
    batch[SFVP/4+9]=batch[SFVP/4+11]=0x3f800000;
    if (vs) {
        memcpy(batch+SFVP/4,vs->viewport,sizeof(vs->viewport));
        batch[CCVP/4]=0;batch[CCVP/4+1]=0x3f800000;
    }
    batch[SCISSOR/4+1]=((d.destination_height-1)<<16)|(d.destination_width-1);
    if (primitive) {
        batch[SCISSOR/4]=d.destination_x|(d.destination_y<<16);
        batch[SCISSOR/4+1]=(d.destination_x+d.width-1)|((d.destination_y+d.height-1)<<16);
    }
    for (unsigned i=0;i<scissor_count;i++) {
        const intel_rcs_scissor_t *r=&scissors[i];
        batch[(SCISSORS+i*32)/4]=r->x|(r->y<<16);
        batch[(SCISSORS+i*32)/4+1]=(r->x+r->width-1)|((r->y+r->height-1)<<16);
    }
    unsigned n=0;
    /* All packet sizes are compile-time constants; no caller-supplied stream.
     * The state page starts at 4096; even 32 scissors fit before that page. */
#define P(...) do { const uint32_t p[]={__VA_ARGS__}; \
    _Static_assert(sizeof(p)<=80,"packet bound"); \
    if (n+sizeof(p)/4>1024) return false; \
    memcpy(batch+n,p,sizeof(p)); n+=sizeof(p)/4; } while (0)
#define Z(op,len) do { P((op)|((len)-2)); n+=(len)-1; } while (0)
    P(0x69040300); /* PIPELINE_SELECT 3D, Gen9 selection mask */
    Z(0x61020000,3); /* STATE_SIP */
    uint32_t curb=pixel ? pixel->curb_read_length : 0;
    uint32_t vcurb=vs ? vs->program.base.curb_read_length : 0,vpush=(vcurb+31)/32;
    for (unsigned i=0;i<5;i++) P(0x79120000+(i<<16),i==0 ? vpush : i==4 ? (vpush<<16)|((curb+31)/32) : 0);
    uint32_t lo=(uint32_t)d.batch_gpu,hi=(uint32_t)(d.batch_gpu>>32);
    uint64_t instruction=pixel ? program_gpu : d.batch_gpu;
    uint32_t instruction_bytes=pixel ? (pixel->code_bytes+4095)&~4095u : 8192;
    if (vs) {instruction=vs->instruction_gpu;instruction_bytes=vs->instruction_bytes;}
    P(0x61010010,1,0,1,lo|1,hi,lo|1,hi,0,0,(uint32_t)instruction|1,(uint32_t)(instruction>>32),
      0xfffff001,sampling ? INTEL_RCS_SAMPLED_BYTES|1 : 0x2001,0xfffff001,instruction_bytes|1,0,0,0);
    P(0x78230000,CCVP,0x78210000,SFVP);
    unsigned urb=vs ? vs->program.urb_entry_size : sampling && pixel->varying_inputs>6 ? 3 : 2;
    P(0x78300000,64|((urb-1)<<16)|(4<<25)); /* 64 entries, 2/3 x 64B */
    P(0x78330000,4<<25,0x78310000,4<<25,0x78320000,4<<25);
    P(0x78240000,BLEND|1,0x780e0000,CC|1);
    P(0x780d0000,primitive ? 1<<4 : 0,0x78180000,1); /* D3D9 integer center; single sample */
    Z(0x78520000,5); /* WM_HZ_OP disabled */
    Z(0x78190000,11); Z(0x781b0000,9); /* constant HS, HS */
    P(0x78270000,0,0x782c0000,0); Z(0x781c0000,4); /* TE disabled */
    Z(0x78160000,11); Z(0x78110000,10);
    P(0x78290000,0,0x782e0000,0);
    Z(0x781a0000,11); Z(0x781d0000,11);
    P(0x78280000,0,0x782d0000,0);
    if (vcurb) {
        uint64_t address=d.batch_gpu+INTEL_RCS_VERTEX_CONSTANTS;
        P(0x78150209,0,vcurb<<16,0,0,0,0,0,0,(uint32_t)address,(uint32_t)(address>>32));
    } else {Z(0x78150000,11);}
    if (vs) {
        P(0x78100007,vs->kernel_offset,0,vs->program.base.flags<<16,0,0,
          (vs->program.base.dispatch_grf_start[0]<<20)|(vs->program.urb_read_length<<11),
          (63u<<23)|5u,(1u<<21)|(vs->sbe_read_length<<16));
    } else {Z(0x78100000,9);}
    P(0x78260000,0,0x782b0000,0);
    Z(0x781e0000,5); /* streamout off */
    if (vs) {P(0x78120002,0,(1u<<31)|(1u<<30)|(1u<<28),1u<<5);}
    else {Z(0x78120000,4);}
    uint32_t attributes=pixel ? pixel->varying_inputs : 1;
    unsigned read_length=vs ? vs->sbe_read_length : sampling && attributes ? (attributes+1)/2 : 1;
    uint32_t flat=sampling ? (pixel->layer_input<9 ? 1u<<pixel->layer_input : 0) : (pixel && attributes ? 1 : 0);
    P(0x781f0004,(attributes<<22)|(1<<29)|(1<<28)|(read_length<<11)|(1<<5)|
      (pixel && attributes ? 1<<21 : 0),0,flat,sampling ? ((1u<<(attributes*2))-1) : (attributes ? 3 : 0),0);
    if (sampling) {
        uint32_t swizzle[8]={0};
        for (unsigned i=0;i<attributes;i++)
            swizzle[i/2]|=(i==pixel->layer_input ? 0xf000u : vs ? sampling->attribute_offset[i] : i)<<(16*(i&1));
        P(0x78510009,swizzle[0],swizzle[1],swizzle[2],swizzle[3],swizzle[4],swizzle[5],swizzle[6],swizzle[7],0,0);
    } else if (pixel && attributes) {
        /* The Gen9 header-only workaround requests a dummy layer input.
         * Override every component with zero; no preceding shader wrote it. */
        P(0x78510009,0xf000,0,0,0,0,0,0,0,0,0);
    } else { Z(0x78510000,11); }
    P(0x78500003,(1<<21)|(1<<16)|(primitive ? 1<<1 : 0)|(vs ? 1u : 0),0,0,0); /* raster, no culling */
    if (vs) {P(0x78130002,2,0,(1u<<11)|8u);} else {Z(0x78130000,4);}
    P(0x78140000,pixel ? pixel->barycentric_modes<<11 : 1<<11); /* barycentrics */
    if (curb) {
        uint64_t constant_gpu=d.batch_gpu+CONSTANTS;
        /* Gen9 constant buffer 3, lengths in 32B units; native UC MOCS 1.
         * Unused rounded tail is zeroed with the owned batch allocation. */
        P(0x78170209,0,curb<<16,0,0,0,0,0,0,(uint32_t)constant_gpu,(uint32_t)(constant_gpu>>32));
    } else { Z(0x78170000,11); }
    if (pixel) {
        /* KSP ordering depends on enabled widths, not simply 8/16/32.
         * Gen9 Variable Pixel Dispatch: KSP0 singleton/8, KSP1 32, KSP2 16. */
        unsigned index[3]={3,3,3};
        if (dispatch_mask&1) index[0]=0;
        else if (dispatch_mask==2) index[0]=1;
        else if (dispatch_mask==4) index[0]=2;
        if ((dispatch_mask&4) && (dispatch_mask&3)) index[1]=2;
        if ((dispatch_mask&2) && (dispatch_mask&5)) index[2]=1;
        uint32_t ksp[3]={0},grf[3]={0};
        for (unsigned i=0;i<3;i++) if (index[i]<3) {
            ksp[i]=pixel->kernel_offset[index[i]]+(vs ? vs->pixel_offset : 0);
            grf[i]=pixel->dispatch_grf_start[index[i]];
        }
        uint32_t resources=sampling ? ((sampling->count+1)<<18)|(((sampling->count+3)/4)<<27) : 1u<<18;
        P(0x7820000a,ksp[0],0,resources|(pixel->flags<<16),0,0,
          (63u<<23)|dispatch_mask|(curb ? 1u<<11 : 0),(grf[0]<<16)|(grf[1]<<8)|grf[2],ksp[1],0,ksp[2],0);
    } else {
        P(0x7820000a,SHADER,0,(1<<27)|(2<<18),0,0,(62u<<23)|2,6<<16,0,0,0,0);
    }
    P(0x784d0000,blend.ps,0x784f0000,(1u<<31)|(attributes ? 1<<8 : 0));
    P(0x782a0000,sampling ? INTEL_RCS_SAMPLED_BT : BT,0x782f0000,
      sampling ? (sampling->count ? INTEL_RCS_SAMPLED_SAMPLERS : 0) : (pixel ? 0 : SAMPLER),0x780f0000,SCISSOR);
    Z(0x784e0000,4); Z(0x78050000,8); Z(0x78070000,5); Z(0x78060000,5);
    P(0x78040001,0,1);
    P(0x79000002,0,((d.destination_height-1)<<16)|(d.destination_width-1),0);
    uint64_t vertices=primitive ? primitive->vertices_gpu+primitive->vertex_offset : d.batch_gpu+VB;
    uint32_t stride=primitive ? primitive->vertex_stride : 12;
    uint32_t vertex_bytes=primitive ? (primitive->vertex_count-1)*stride+(sampling ? sampling->vertex_bytes : 16) : 36;
    P(0x78080003,(1<<14)|stride,(uint32_t)vertices,(uint32_t)(vertices>>32),vertex_bytes);
    if (vs) {
        unsigned elements=vs->program.base.varying_inputs;
        P(0x78090000|((elements ? elements : 1)*2-1));
        if (!elements) {P(1u<<25,0x22220000);}
        for (unsigned i=0;i<elements;i++)
            P((1u<<25)|vs->attribute_offset[i]|(vs->attribute_format[i]<<16),vs->attribute_control[i]);
    } else if (sampling) {
        unsigned elements=2+(attributes ? attributes : 1);
        P(0x78090000|(elements*2-1),(1<<25),0x22220000,(1<<25)|sampling->position_offset,0x11110000);
        for (unsigned i=0;i<elements-2;i++) {
            uint32_t offset=sampling->attribute_offset[i];
            P((1<<25)|(offset==UINT32_MAX ? 0 : offset|(sampling->attribute_format[i]<<16)),
              offset==UINT32_MAX ? 0x22220000 : sampling->attribute_control[i]);
        }
    } else if (primitive) {
        P(0x78090005,(1<<25),0x22220000,
          (1<<25),0x11110000,(1<<25),0x22220000); /* header, R32G32B32A32_FLOAT position, dummy */
    } else {
        P(0x78090005,(1<<25),0x22220000,
          (1<<25)|(0xf6<<16),0x11230000,(1<<25)|(0x85<<16)|4,0x11230000);
    }
    P(0x784b0000,primitive ? primitive->topology : 0xf,0x780c0000,0,0x78490001,0,0);
    if (indices) {
        uint64_t address=indices->gpu+indices->offset;
        P(0x780a0003,2u|((indices->stride==2 ? 1u : 2u)<<8),
            (uint32_t)address,(uint32_t)(address>>32),indices->count*indices->stride);
    }
    for (unsigned i=0;i<(scissor_count ? scissor_count : 1);i++) {
        if (scissor_count) { P(0x780f0000,SCISSORS+i*32); }
        P(0x7b000005,indices ? 1u<<8 : 0,indices ? indices->count : primitive ? primitive->vertex_count : 3,
            0,1,0,indices ? (uint32_t)indices->base_vertex : 0);
    }
    P(0x05000000);
#undef Z
#undef P
    return true;
}

bool intel_rcs_draw_build(uint32_t *batch,size_t bytes,const intel_rcs_draw_t *input)
{
    uintptr_t b=(uintptr_t)batch;
    if (!batch || !input || (b&3) || ((uintptr_t)input&7) || bytes<INTEL_RCS_DRAW_BYTES ||
        b>UINT64_MAX-INTEL_RCS_DRAW_BYTES) return false;
    /* Copy caller metadata before touching output, including legal aliasing. */
    intel_rcs_draw_t d;
    memcpy(&d,input,sizeof(d));
    uint64_t gpu[]={d.batch_gpu,d.source_gpu,d.destination_gpu};
    uint64_t extent[]={INTEL_RCS_DRAW_BYTES,d.source_bytes,d.destination_bytes};
    for (unsigned i=0;i<3;i++) {
        if (!gpu[i] || (gpu[i]&4095) || !extent[i] || extent[i]>(1ULL<<28) ||
            gpu[i]>INTEL_PPGTT_VA_LIMIT-extent[i]) return false;
        for (unsigned j=0;j<i;j++)
            if (gpu[i]<gpu[j]+extent[j] && gpu[j]<gpu[i]+extent[i]) return false;
    }
    uint32_t w[]={d.source_width,d.destination_width},h[]={d.source_height,d.destination_height};
    uint32_t pitch[]={d.source_pitch,d.destination_pitch};
    for (unsigned i=0;i<2;i++)
        if (!w[i] || w[i]>16384 || !h[i] || h[i]>16384 || (pitch[i]&3) ||
            pitch[i]<w[i]*4 || pitch[i]>262144 ||
            (uint64_t)pitch[i]*h[i]>extent[i+1]) return false;
    if (!d.width || !d.height || d.width>d.source_width || d.height>d.source_height ||
        d.width>d.destination_width || d.height>d.destination_height ||
        d.source_x>d.source_width-d.width || d.source_y>d.source_height-d.height ||
        d.destination_x>d.destination_width-d.width || d.destination_y>d.destination_height-d.height)
        return false;
    return rcs_draw_emit(batch,&d,NULL,0,0,NULL,NULL,NULL,0,NULL);
}

bool intel_rcs_pixel_validate(const struct intel_d3d9_program *input,size_t bytes)
{
    uintptr_t address=(uintptr_t)input;
    if (!input || (address&3) || bytes<sizeof(*input) || address>((uintptr_t)-1)-bytes) return false;
    const struct intel_d3d9_program p=*input;
    if ((p.abi!=INTEL_D3D9_COMPILER_ABI && p.abi!=INTEL_D3D9_PROGRAM_LEGACY_ABI) || p.header_bytes!=sizeof(p) || p.pci_id!=0x3e9b ||
        p.allocation_bytes!=bytes || p.bytecode_offset!=sizeof(p) ||
        !p.bytecode_bytes || (p.bytecode_bytes&3) || p.bytecode_bytes>65536 ||
        p.code_offset<sizeof(p)+p.bytecode_bytes || (p.code_offset&63) ||
        !p.code_bytes || (p.code_bytes&7) || p.code_bytes>INTEL_RCS_PIXEL_MAX_CODE ||
        p.code_offset>bytes || p.code_bytes!=bytes-p.code_offset || p.stage!=1 ||
        !p.simd_mask || (p.simd_mask&~7u) || !p.grf_used || p.grf_used>128 ||
        p.scratch_bytes || p.constant_data_bytes || p.constant_data_offset ||
        (p.flags&~1u) || p.varying_inputs>1 || p.barycentric_modes ||
        p.parameter_count>INTEL_D3D9_PROGRAM_MAX_PARAMETERS || p.curb_read_length!=(p.parameter_count+7)/8 ||
        p.layer_input!=(p.varying_inputs ? 0 : UINT32_MAX)) return false;
    if (p.parameter_count) {
        if (p.abi==INTEL_D3D9_PROGRAM_LEGACY_ABI || (p.parameter_map_offset&3) ||
            p.parameter_map_offset<sizeof(p)+p.bytecode_bytes || p.parameter_map_offset>p.code_offset ||
            p.parameter_count*4>p.code_offset-p.parameter_map_offset) return false;
    } else if (p.parameter_map_offset) return false;
    for (unsigned i=0;i<5;i++) if (p.reserved[i]) return false;
    for (unsigned i=0;i<3;i++) if (p.simd_mask&(1u<<i))
        if ((p.kernel_offset[i]&63) || p.kernel_offset[i]>=p.code_bytes ||
            p.dispatch_grf_start[i]>=128 || (p.parameter_count &&
            (p.dispatch_grf_start[i]<2 || p.dispatch_grf_start[i]>p.grf_used ||
             p.curb_read_length>p.grf_used-p.dispatch_grf_start[i]))) return false;
    return true;
}

static bool rcs_pixel_draw_valid(const intel_rcs_pixel_draw_t *input,const struct intel_d3d9_program *program)
{
    const intel_rcs_pixel_draw_t d=*input;
    const struct intel_d3d9_program p=*program;
    uint64_t gpu[]={d.batch_gpu,d.program_gpu,d.destination_gpu};
    uint64_t extent[]={INTEL_RCS_DRAW_BYTES,(p.code_bytes+4095)&~4095u,d.destination_bytes};
    for (unsigned i=0;i<3;i++) {
        if (!gpu[i] || (gpu[i]&4095) || !extent[i] || extent[i]>(1ULL<<28) ||
            gpu[i]>INTEL_PPGTT_VA_LIMIT-extent[i]) return false;
        for (unsigned j=0;j<i;j++)
            if (gpu[i]<gpu[j]+extent[j] && gpu[j]<gpu[i]+extent[i]) return false;
    }
    if (!d.destination_width || d.destination_width>4096 || !d.destination_height || d.destination_height>4096 ||
        (d.destination_pitch&3) || d.destination_pitch<d.destination_width*4 || d.destination_pitch>262144 ||
        (uint64_t)d.destination_pitch*d.destination_height>d.destination_bytes || !d.width || !d.height ||
        d.width>d.destination_width || d.height>d.destination_height ||
        d.x>d.destination_width-d.width || d.y>d.destination_height-d.height ||
        !d.dispatch_mask || (d.dispatch_mask&~p.simd_mask) || d.constants.count!=p.parameter_count) return false;
    return true;
}

bool intel_rcs_pixel_build(uint32_t *batch,size_t bytes,const intel_rcs_pixel_draw_t *input,
    const struct intel_d3d9_program *program,size_t program_bytes)
{
    uintptr_t b=(uintptr_t)batch;
    if (!batch || !input || (b&3) || ((uintptr_t)input&7) || bytes<INTEL_RCS_DRAW_BYTES ||
        b>((uintptr_t)-1)-INTEL_RCS_DRAW_BYTES || !intel_rcs_pixel_validate(program,program_bytes)) return false;
    intel_rcs_pixel_draw_t d;
    memcpy(&d,input,sizeof(d));
    const struct intel_d3d9_program p=*program;
    if (!rcs_pixel_draw_valid(&d,&p)) return false;
    intel_rcs_draw_t draw={0};
    draw.batch_gpu=d.batch_gpu; draw.destination_gpu=d.destination_gpu;
    draw.destination_width=d.destination_width; draw.destination_height=d.destination_height;
    draw.destination_pitch=d.destination_pitch; draw.destination_x=d.x; draw.destination_y=d.y;
    draw.width=d.width; draw.height=d.height;
    return rcs_draw_emit(batch,&draw,&p,d.program_gpu,d.dispatch_mask,NULL,&d.constants,NULL,0,NULL);
}

bool intel_rcs_primitive_build(uint32_t *batch,size_t bytes,const intel_rcs_primitive_draw_t *input,
    const struct intel_d3d9_program *program,size_t program_bytes)
{
    return intel_rcs_primitives_build(batch,bytes,input,program,program_bytes,NULL,0);
}

bool intel_rcs_primitives_build(uint32_t *batch,size_t bytes,const intel_rcs_primitive_draw_t *input,
    const struct intel_d3d9_program *program,size_t program_bytes,
    const intel_rcs_scissor_t *scissors,unsigned scissor_count)
{
    uintptr_t b=(uintptr_t)batch;
    if (!batch || !input || (b&3) || ((uintptr_t)input&7) || bytes<INTEL_RCS_DRAW_BYTES ||
        b>((uintptr_t)-1)-INTEL_RCS_DRAW_BYTES || !intel_rcs_pixel_validate(program,program_bytes)) return false;
    intel_rcs_primitive_draw_t d;
    memcpy(&d,input,sizeof(d));
    const struct intel_d3d9_program p=*program;
    if (scissor_count>INTEL_RCS_MAX_SCISSORS || (scissor_count && !scissors)) return false;
    intel_rcs_scissor_t list[INTEL_RCS_MAX_SCISSORS];
    if (scissor_count) memcpy(list,scissors,scissor_count*sizeof(*list));
    for (unsigned i=0;i<scissor_count;i++) {
        const intel_rcs_scissor_t *r=&list[i];
        if (!r->width || !r->height || r->width>d.pixel.width || r->height>d.pixel.height ||
            r->x<d.pixel.x || r->y<d.pixel.y || r->x-d.pixel.x>d.pixel.width-r->width ||
            r->y-d.pixel.y>d.pixel.height-r->height) return false;
    }
    if (!rcs_pixel_draw_valid(&d.pixel,&p) || !d.vertices_gpu || (d.vertices_gpu&4095) ||
        !d.vertices_bytes || d.vertices_bytes>(1ULL<<28) || d.vertices_gpu>INTEL_PPGTT_VA_LIMIT-d.vertices_bytes ||
        d.vertex_stride<16 || d.vertex_stride>2048 || (d.vertex_stride&3) || (d.vertex_offset&3) ||
        d.vertex_count<3 || d.vertex_count>65535 || d.topology<INTEL_RCS_TRIANGLE_LIST ||
        d.topology>INTEL_RCS_TRIANGLE_FAN || (d.topology==INTEL_RCS_TRIANGLE_LIST && d.vertex_count%3) ||
        (uint64_t)d.vertex_offset+(uint64_t)(d.vertex_count-1)*d.vertex_stride+16>d.vertices_bytes) return false;
    uint64_t gpu[]={d.pixel.batch_gpu,d.pixel.program_gpu,d.pixel.destination_gpu};
    uint64_t extent[]={INTEL_RCS_DRAW_BYTES,(p.code_bytes+4095)&~4095u,d.pixel.destination_bytes};
    for (unsigned i=0;i<3;i++)
        if (gpu[i]<d.vertices_gpu+d.vertices_bytes && d.vertices_gpu<gpu[i]+extent[i]) return false;
    intel_rcs_draw_t draw={.batch_gpu=d.pixel.batch_gpu,.destination_gpu=d.pixel.destination_gpu,
        .destination_width=d.pixel.destination_width,.destination_height=d.pixel.destination_height,
        .destination_pitch=d.pixel.destination_pitch,.destination_x=d.pixel.x,.destination_y=d.pixel.y,
        .width=d.pixel.width,.height=d.pixel.height};
    return rcs_draw_emit(batch,&draw,&p,d.pixel.program_gpu,d.pixel.dispatch_mask,&d,&d.pixel.constants,
                         list,scissor_count,NULL);
}

bool intel_rcs_bound_emit(uint32_t *batch,const intel_rcs_primitive_draw_t *d,
    const struct intel_d3d9_program *p,const intel_rcs_scissor_t *scissors,unsigned count,
    const intel_rcs_sampling_state_t *sampling)
{
    intel_rcs_draw_t draw={.batch_gpu=d->pixel.batch_gpu,.destination_gpu=d->pixel.destination_gpu,
        .destination_width=d->pixel.destination_width,.destination_height=d->pixel.destination_height,
        .destination_pitch=d->pixel.destination_pitch,.destination_x=d->pixel.x,.destination_y=d->pixel.y,
        .width=d->pixel.width,.height=d->pixel.height};
    return rcs_draw_emit(batch,&draw,p,d->pixel.program_gpu,d->pixel.dispatch_mask,d,&d->pixel.constants,
                         scissors,count,sampling);
}
