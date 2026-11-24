#include "intel_rcs.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

static void rcs_encode_flush(uint32_t *ring,uint64_t token)
{
    memset(ring,0,INTEL_RCS_FLUSH_BYTES);
    /* Gen9 requires a null PIPE_CONTROL before VF cache invalidation.
     * The second command flushes writes and invalidates reads before its
     * CS-stalled qword post-sync to the per-process HWSP scratch index.
     * KBL PRM PIPE_CONTROL; i915 gen8_emit_flush_rcs follows this sequence. */
    ring[0]=0x7a000004;
    ring[6]=0x7a000004;
    ring[7]=(1u<<21)|(1u<<20)|(1u<<18)|(1u<<14)|(1u<<12)|
            (1u<<11)|(1u<<10)|(1u<<7)|(1u<<5)|(1u<<4)|(1u<<3)|(1u<<2)|1u;
    ring[8]=INTEL_RCS_TOKEN_OFFSET;
    ring[10]=(uint32_t)token; ring[11]=(uint32_t)(token>>32);
}

bool intel_rcs_build(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint64_t context_gpu,uint64_t ring_gpu,uint64_t pml4_dma,
    uint32_t context_id,uint64_t token,uint64_t *descriptor)
{
    uintptr_t c=(uintptr_t)context,r=(uintptr_t)ring,d=(uintptr_t)descriptor;
    if (!context || !ring || !descriptor || ((c|r)&3) || (d&7) ||
        context_bytes<INTEL_RCS_CONTEXT_BYTES || ring_bytes<4096 ||
        c>UINT64_MAX-INTEL_RCS_CONTEXT_BYTES || r>UINT64_MAX-4096 || d>UINT64_MAX-8 ||
        (c<r+4096 && r<c+INTEL_RCS_CONTEXT_BYTES) ||
        (d<c+INTEL_RCS_CONTEXT_BYTES && c<d+8) || (d<r+4096 && r<d+8) ||
        !context_gpu || (context_gpu&4095) || context_gpu>0x100000000ULL-INTEL_RCS_CONTEXT_BYTES ||
        !ring_gpu || (ring_gpu&4095) || ring_gpu>0xfffff000ULL ||
        (context_gpu<ring_gpu+4096 && ring_gpu<context_gpu+INTEL_RCS_CONTEXT_BYTES) ||
        !pml4_dma || (pml4_dma&4095) || pml4_dma>=INTEL_GGTT_PHYS_LIMIT ||
        !context_id || context_id>=(1u<<21) || !token || token==UINT64_MAX) return false;
    memset(context,0,INTEL_RCS_CONTEXT_BYTES); memset(ring,0,4096);
    uint32_t *state=context+1024;
    static const uint16_t offsets[]={0x244,0x34,0x30,0x38,0x3c,0x168,0x140,
        0x110,0x11c,0x114,0x118,0x1c0,0x1c4,0x1c8};
    state[1]=0x1100101b;
    for (unsigned i=0;i<14;i++) state[2+2*i]=0x2000+offsets[i];
    state[3]=0x000f0009; /* initial render restore inhibit, synchronous save */
    state[7]=INTEL_RCS_FLUSH_BYTES; state[9]=(uint32_t)ring_gpu; state[11]=1;
    state[33]=0x11001011; state[34]=0x23a8;
    for (unsigned i=0;i<8;i++) state[36+2*i]=0x228c-4*i;
    state[49]=(uint32_t)(pml4_dma>>32); state[51]=(uint32_t)pml4_dma;
    state[65]=0x11000001; state[66]=0x20c8; /* render power clock state */
    rcs_encode_flush(ring,token);
    *descriptor=((uint64_t)context_id<<32)|context_gpu|0x11d;
    return true;
}

bool intel_rcs_build_batch(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint64_t context_gpu,uint64_t ring_gpu,uint64_t pml4_dma,
    uint32_t context_id,uint64_t batch_gpu,uint64_t token,uint64_t *descriptor,uint32_t *tail)
{
    uintptr_t t=(uintptr_t)tail,c=(uintptr_t)context,r=(uintptr_t)ring,d=(uintptr_t)descriptor;
    if (!tail || (t&3) || t>UINT64_MAX-4 || c>UINT64_MAX-INTEL_RCS_CONTEXT_BYTES ||
        r>UINT64_MAX-4096 || d>UINT64_MAX-8 ||
        !batch_gpu || (batch_gpu&7) || batch_gpu>=(1ULL<<47) ||
        (t<c+INTEL_RCS_CONTEXT_BYTES && c<t+4) || (t<r+4096 && r<t+4) || (t<d+8 && d<t+4)) return false;
    if (!intel_rcs_build(context,context_bytes,ring,ring_bytes,context_gpu,ring_gpu,pml4_dma,
        context_id,token,descriptor)) return false;
    rcs_encode_flush(ring,0); /* initial flush cannot look like completion */
    ring[16]=0x18800101; ring[17]=(uint32_t)batch_gpu; ring[18]=(uint32_t)(batch_gpu>>32);
    rcs_encode_flush(ring+20,token);
    context[1024+7]=192; *tail=192;
    return true;
}

bool intel_rcs_append(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint32_t completed_tail,uint64_t token,uint32_t *next_tail)
{
    uintptr_t c=(uintptr_t)context,r=(uintptr_t)ring,t=(uintptr_t)next_tail;
    if (!context || !ring || !next_tail || ((c|r|t)&3) ||
        context_bytes<INTEL_RCS_CONTEXT_BYTES || ring_bytes<4096 ||
        c>UINT64_MAX-INTEL_RCS_CONTEXT_BYTES || r>UINT64_MAX-4096 || t>UINT64_MAX-4 ||
        (c<r+4096 && r<c+INTEL_RCS_CONTEXT_BYTES) ||
        (t<c+INTEL_RCS_CONTEXT_BYTES && c<t+4) || (t<r+4096 && r<t+4) ||
        (completed_tail&63) || completed_tail>=4096 || !token || token==UINT64_MAX) return false;
    uint32_t *state=context+1024;
    if ((state[5]&0x1ffffc)!=completed_tail || (state[7]&0x1ffff8)!=completed_tail ||
        state[2]!=0x2244 || state[4]!=0x2034 || state[6]!=0x2030 || state[8]!=0x2038 ||
        state[10]!=0x203c || (state[11]&0x1ff001)!=1) return false;
    rcs_encode_flush((uint32_t *)((uint8_t *)ring+completed_tail),token);
    state[3]=0x000f0008; state[7]=(completed_tail+INTEL_RCS_FLUSH_BYTES)&4095;
    *next_tail=state[7];
    return true;
}

bool intel_rcs_append_batch(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint32_t completed_tail,uint64_t batch_gpu,
    uint64_t token,uint32_t *next_tail)
{
    /* Validate everything before append changes the caller's saved image. */
    if (!batch_gpu || (batch_gpu&7) || batch_gpu>=(1ULL<<47)) return false;
    if (!intel_rcs_append(context,context_bytes,ring,ring_bytes,completed_tail,token,next_tail))
        return false;
    uint32_t words[32]={0};
    /* First-level PPGTT batch returns to this ring at BATCH_BUFFER_END. */
    words[0]=0x18800101; words[1]=(uint32_t)batch_gpu; words[2]=(uint32_t)(batch_gpu>>32);
    rcs_encode_flush(words+4,token);
    for (unsigned i=0;i<32;i++) ring[(completed_tail/4+i)&1023]=words[i];
    context[1024+7]=(completed_tail+128)&4095;
    *next_tail=context[1024+7];
    return true;
}

bool intel_rcs_cache_acquire(const intel_gt_io_t *io,void *ctx,intel_rcs_cache_t *s)
{
    if (!s || s->started || !io || !io->read || !io->write) return false;
    const uint32_t reg[]={0xc800,0xc804,0xb020},value[]={9,9,0x00100010};
    for (unsigned i=0;i<3;i++)
        if (!io->read(ctx,reg[i],&s->original[i]) || s->original[i]==UINT32_MAX) return false;
    s->started=true;
    for (unsigned i=0;i<3;i++) {
        uint32_t got;
        s->changed|=1u<<i;
        if (!io->write(ctx,reg[i],value[i]) || !io->read(ctx,reg[i],&got) || got!=value[i]) return false;
    }
    return s->ready=true;
}

bool intel_rcs_cache_restore(const intel_gt_io_t *io,void *ctx,intel_rcs_cache_t *s)
{
    if (!s || !io || !io->read || !io->write) return false;
    s->ready=false;
    const uint32_t reg[]={0xc800,0xc804,0xb020};
    for (unsigned i=3;i--;) if (s->changed&(1u<<i)) {
        uint32_t got;
        if (!io->write(ctx,reg[i],s->original[i]) ||
            !io->read(ctx,reg[i],&got) || got!=s->original[i]) return false;
        s->changed&=~(1u<<i);
    }
    return true;
}
