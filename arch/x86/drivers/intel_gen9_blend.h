/* Gen9 fixed-function blend state. No pixel processing runs on the CPU.
 * Field definitions: Intel KBL BLEND_STATE_ENTRY/COLOR_CALC_STATE and
 * 3DSTATE_PS_BLEND; matching Mesa gen40/gen80/gen90 XML is in-tree. */
#ifndef OSITO_INTEL_GEN9_BLEND_H
#define OSITO_INTEL_GEN9_BLEND_H
#include "intel_gpu_umd_abi.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t header,entry[2],ps,constant[4],format;
} intel_gen9_blend_state_t;

static inline unsigned intel_gen9_blend_factor(unsigned d3d)
{
    static const unsigned values[16]={0,17,1,2,18,3,19,4,20,5,21,6,0,0,7,23};
    return d3d<16 ? values[d3d] : 0;
}
static inline uint32_t intel_gen9_blend_unorm8(unsigned value)
{
    if (!value) return 0;
    unsigned exponent=127;
    while (value<255) {value<<=1;exponent--;}
    return (exponent<<23)+(uint32_t)((((uint64_t)value<<23)+127)/255)-0x800000;
}
static inline bool intel_gen9_blend_pack(const intel_umd_blend_t *input,
                                        intel_gen9_blend_state_t *output)
{
    if (!input || !output) return false;
    intel_umd_blend_t d=*input;
    intel_gen9_blend_state_t out={0};
    out.format=192;out.ps=1u<<30;
    out.entry[0]=(17u<<21)|(1u<<26);out.entry[1]=2;
    if (!d.flags && !d.rgb && !d.alpha && !d.factor) {*output=out;return true;}
    if (!(d.flags&INTEL_UMD_BLEND_VALID) || (d.flags&~(INTEL_UMD_BLEND_VALID|0xf3u)) ||
        (d.rgb&~0x1fffu) || (d.alpha&~0x1fffu)) return false;
    unsigned src=intel_gen9_blend_factor(d.rgb&31),dst=intel_gen9_blend_factor((d.rgb>>5)&31);
    unsigned asrc=intel_gen9_blend_factor(d.alpha&31),adst=intel_gen9_blend_factor((d.alpha>>5)&31);
    unsigned op=(d.rgb>>10)&7,aop=(d.alpha>>10)&7;
    if (!src || !dst || !asrc || !adst || op<1 || op>5 || aop<1 || aop>5) return false;
    /* MIN/MAX ignore factors. Gen9 requires ONE to match that contract. */
    if (op>=4) src=dst=1;
    if (aop>=4) asrc=adst=1;
    /* Alpha factors address the alpha component even for COLOR enums.
     * SRC_ALPHA_SATURATE has an alpha factor of ONE. */
    const unsigned alpha_map[25]={0,1,3,3,4,4,1,8,8,0,0,0,0,0,0,0,0,17,19,19,20,20,0,24,24};
    asrc=alpha_map[asrc];adst=alpha_map[adst];
    unsigned writes=(d.flags>>4)&15;
    /* D3D mask is RGBA; the Gen9 disable bits are BGRA. */
    unsigned disables=((~writes&1)<<2)|(~writes&2)|((~writes&4)>>2)|(~writes&8);
    bool enabled=(d.flags&INTEL_UMD_BLEND_ENABLE)!=0;
    out.header=enabled ? 1u<<30 : 0; /* Independent alpha uses effective factors. */
    out.entry[0]=disables|((aop-1)<<5)|(adst<<8)|(asrc<<13)|((op-1)<<18)|(dst<<21)|(src<<26);
    out.entry[1]=3; /* Clamp before and after UNORM8 blending. */
    out.ps=writes ? 1u<<30 : 0;
    if (enabled) {
        out.entry[0]|=1u<<31;
        out.ps|=(1u<<29)|(1u<<7)|(src<<14)|(dst<<9)|(asrc<<24)|(adst<<19);
    }
    if (d.flags&INTEL_UMD_BLEND_XRGB) out.format=233;
    const unsigned shifts[4]={16,8,0,24};
    for (unsigned i=0;i<4;i++) out.constant[i]=intel_gen9_blend_unorm8((d.factor>>shifts[i])&255);
    *output=out;return true;
}
#endif
