#include "intel_bcs.h"
#include "intel_ppgtt.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif

#define BCS_BASE 0x22000u
#define BCS_HEAD (BCS_BASE + 0x34)
#define BCS_TAIL (BCS_BASE + 0x30)
#define BCS_HWS (BCS_BASE + 0x80)
#define BCS_MI_MODE (BCS_BASE + 0x9c)
#define BCS_RESET (BCS_BASE + 0xd0)
#define BCS_ELSP (BCS_BASE + 0x230)
#define BCS_STATUS (BCS_BASE + 0x234)
#define BCS_MODE (BCS_BASE + 0x29c)
#define BCS_CSB (BCS_BASE + 0x370)
#define BCS_CSB_PTR (BCS_BASE + 0x3a0)
#define BCS_ESR (BCS_BASE + 0xb8)
#define BCS_POLL_LIMIT 200000u

bool intel_bcs_build(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint64_t context_gpu,
    uint64_t ring_gpu, uint64_t pml4_dma, uint32_t context_id,
    uint64_t token, uint64_t *descriptor)
{
    uintptr_t c = (uintptr_t)context, r = (uintptr_t)ring, d = (uintptr_t)descriptor;
    if (!context || !ring || !descriptor || (c & 3) || (r & 3) || (d & 7) ||
        context_bytes < INTEL_BCS_CONTEXT_BYTES || ring_bytes < INTEL_BCS_RING_BYTES ||
        c > UINT64_MAX - INTEL_BCS_CONTEXT_BYTES || r > UINT64_MAX - INTEL_BCS_RING_BYTES ||
        d > UINT64_MAX - 8 ||
        (c < r + INTEL_BCS_RING_BYTES && r < c + INTEL_BCS_CONTEXT_BYTES) ||
        (d < c + INTEL_BCS_CONTEXT_BYTES && c < d + 8) ||
        (d < r + INTEL_BCS_RING_BYTES && r < d + 8) ||
        !context_gpu || (context_gpu & 4095) || context_gpu > 0x100000000ULL - 8192 ||
        !ring_gpu || (ring_gpu & 4095) || ring_gpu > 0x100000000ULL - 4096 ||
        (context_gpu < ring_gpu + 4096 && ring_gpu < context_gpu + 8192) ||
        !pml4_dma || (pml4_dma & 4095) || pml4_dma >= INTEL_GGTT_PHYS_LIMIT ||
        !context_id || context_id >= (1u << 21) || !token || token == UINT64_MAX)
        return false;
    memset(context, 0, INTEL_BCS_CONTEXT_BYTES);
    memset(ring, 0, INTEL_BCS_RING_BYTES);
    /* Gen9 XCS logical ring layout. The register offsets are the hardware
     * context image ABI (also described by i915's gen9_xcs_offsets table).
     * Engine context restore is inhibited on this fresh, reset-only context;
     * hardware may save its engine state into the remainder of page two. */
    uint32_t *state = context + 1024;
    static const uint16_t ring_regs[] = {0x244, 0x34, 0x30, 0x38, 0x3c,
        0x168, 0x140, 0x110, 0x11c, 0x114, 0x118, 0x1c0, 0x1c4, 0x1c8};
    state[1] = 0x1100101b; /* MI_LOAD_REGISTER_IMM, 14, force posted */
    for (unsigned i = 0; i < 14; i++) state[2 + 2*i] = BCS_BASE + ring_regs[i];
    state[3] = 0x000f0009; /* inhibit initial engine restore/synchronous switch */
    state[7] = INTEL_BCS_TAIL;
    state[9] = (uint32_t)ring_gpu;
    state[11] = 1; /* one page, enabled */
    state[33] = 0x11001011; /* timestamp and PDP3..0 (high, low) */
    state[34] = BCS_BASE + 0x3a8;
    for (unsigned i = 0; i < 8; i++) state[36 + 2*i] = BCS_BASE + 0x28c - 4*i;
    state[49] = (uint32_t)(pml4_dma >> 32);
    state[51] = (uint32_t)pml4_dma;
    state[65] = 0x11001001;
    state[66] = BCS_BASE + 0x200; /* BCS_SWCTRL; unchanged in token-only test */
    /* Qword store into the per-process HWSP scratch area. Address bit 5 is
     * clear, as required by MI_FLUSH_DW's Gen9 qword-write restriction. The
     * following NOOP padding leaves a cacheline-half-aligned ring tail. */
    ring[0] = 0x13244003; /* MI_FLUSH_DW, index, TLB invalidate, post-sync */
    ring[1] = INTEL_BCS_TOKEN_OFFSET; /* PPHWSP index, not raw CPU physical */
    ring[2] = 0;
    ring[3] = (uint32_t)token;
    ring[4] = (uint32_t)(token >> 32);
    *descriptor = ((uint64_t)context_id << 32) | context_gpu | 0x11d;
    return true;
}

bool intel_bcs_build_fence(uint32_t *context,size_t context_bytes,uint32_t *ring,
    size_t ring_bytes,uint64_t context_gpu,uint64_t ring_gpu,uint64_t pml4_dma,
    uint32_t context_id,uint64_t token,uint64_t fence_gpu,uint64_t value,uint64_t *descriptor)
{
    if (!fence_gpu || (fence_gpu&7) || fence_gpu>INTEL_PPGTT_VA_LIMIT-8) return false;
    if (!intel_bcs_build(context,context_bytes,ring,ring_bytes,context_gpu,ring_gpu,
        pml4_dma,context_id,token,descriptor)) return false;
    /* Initial flush/TLB barrier cannot publish the completion token. */
    for (unsigned i=0;i<5;i++) ring[16+i]=ring[i];
    ring[3]=ring[4]=0;
    ring[8]=0x10200003; /* MI_STORE_DATA_IMM: qword, per-process address */
    ring[9]=(uint32_t)fence_gpu; ring[10]=(uint32_t)(fence_gpu>>32);
    ring[11]=(uint32_t)value; ring[12]=(uint32_t)(value>>32);
    context[1024+7]=INTEL_BCS_FENCE_TAIL;
    return true;
}

static bool intel_bcs_encode_copy(uint32_t *ring, uint64_t token,
                                   const intel_bcs_copy_t *input)
{
    if (!token || token == UINT64_MAX) return false;
    if (!input) return false;
    intel_bcs_copy_t copy = *input;
    if (!copy.width || copy.width > 32767 || !copy.height || copy.height > 32767 ||
        (copy.source_pitch & 3) || (copy.destination_pitch & 3) ||
        copy.source_pitch > 32764 || copy.destination_pitch > 32764 ||
        copy.width > copy.source_pitch / 4 || copy.width > copy.destination_pitch / 4 ||
        copy.source_x > copy.source_pitch / 4 - copy.width ||
        copy.destination_x > copy.destination_pitch / 4 - copy.width ||
        copy.source_y > 32767 - copy.height || copy.destination_y > 32767 - copy.height ||
        !copy.source || (copy.source & 63) || !copy.destination || (copy.destination & 63) ||
        !copy.source_bytes || copy.source_bytes > INTEL_PPGTT_VA_LIMIT ||
        !copy.destination_bytes || copy.destination_bytes > INTEL_PPGTT_VA_LIMIT ||
        copy.source > INTEL_PPGTT_VA_LIMIT - copy.source_bytes ||
        copy.destination > INTEL_PPGTT_VA_LIMIT - copy.destination_bytes ||
        (uint64_t)(copy.source_y+copy.height-1)*copy.source_pitch + (copy.source_x+copy.width)*4 > copy.source_bytes ||
        (uint64_t)(copy.destination_y+copy.height-1)*copy.destination_pitch + (copy.destination_x+copy.width)*4 > copy.destination_bytes ||
        (copy.source < copy.destination + copy.destination_bytes &&
         copy.destination < copy.source + copy.source_bytes)) return false;
    memset(ring, 0, INTEL_BCS_COPY_TAIL);
    ring[0] = 0x54f00008; /* XY_SRC_COPY_BLT, RGB+alpha, ten dwords */
    ring[1] = 0x03cc0000 | copy.destination_pitch; /* 32bpp, source-copy ROP */
    ring[2] = copy.destination_y << 16 | copy.destination_x;
    ring[3] = (copy.destination_y+copy.height) << 16 | (copy.destination_x+copy.width);
    ring[4] = (uint32_t)copy.destination;
    ring[5] = (uint32_t)(copy.destination >> 32);
    ring[6] = copy.source_y << 16 | copy.source_x;
    ring[7] = copy.source_pitch;
    ring[8] = (uint32_t)copy.source;
    ring[9] = (uint32_t)(copy.source >> 32);
    ring[10] = 0x13244003;
    ring[11] = INTEL_BCS_TOKEN_OFFSET;
    ring[13] = (uint32_t)token;
    ring[14] = (uint32_t)(token >> 32);
    return true;
}

bool intel_bcs_build_copy(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint64_t context_gpu,
    uint64_t ring_gpu, uint64_t pml4_dma, uint32_t context_id,
    uint64_t token, const intel_bcs_copy_t *input, uint64_t *descriptor)
{
    uint32_t commands[16];
    if (!intel_bcs_encode_copy(commands, token, input) ||
        !intel_bcs_build(context, context_bytes, ring, ring_bytes, context_gpu,
                         ring_gpu, pml4_dma, context_id, token, descriptor)) return false;
    memcpy(ring, commands, sizeof(commands));
    context[1024+7] = INTEL_BCS_COPY_TAIL;
    context[1024+67] = 0x000f0000; /* BCS_SWCTRL: linear, normal cache flush */
    return true;
}

bool intel_bcs_append_copy(uint32_t *context, size_t context_bytes,
    uint32_t *ring, size_t ring_bytes, uint32_t completed_tail,
    uint64_t token, const intel_bcs_copy_t *copy, uint32_t *next_tail)
{
    uintptr_t c=(uintptr_t)context, r=(uintptr_t)ring, t=(uintptr_t)next_tail;
    if (!context || !ring || !next_tail || ((c|r|t)&3) ||
        context_bytes < 8192 || ring_bytes < 4096 ||
        c > UINT64_MAX-8192 || r > UINT64_MAX-4096 || t > UINT64_MAX-4 ||
        (c < r+4096 && r < c+8192) || (t < c+8192 && c < t+4) ||
        (t < r+4096 && r < t+4) || (completed_tail & 63) || completed_tail >= 4096)
        return false;
    uint32_t *state=context+1024;
    if ((state[5]&0x1ffffc) != completed_tail || (state[7]&0x1ffff8) != completed_tail ||
        state[2] != BCS_BASE+0x244 || state[4] != BCS_HEAD || state[6] != BCS_TAIL ||
        state[8] != BCS_BASE+0x38 || state[10] != BCS_BASE+0x3c || (state[11]&0x1ff001) != 1)
        return false;
    uint32_t commands[16];
    if (!intel_bcs_encode_copy(commands, token, copy)) return false;
    memcpy((uint8_t *)ring+completed_tail, commands, sizeof(commands));
    state[3] = 0x000f0008; /* restore hardware-saved engine state */
    state[7] = (completed_tail+sizeof(commands)) & 4095;
    *next_tail = state[7];
    return true;
}

/* Keep the copy-engine API scoped to BCS while sharing the hardware protocol. */
enum intel_bcs_status intel_bcs_enable(const intel_gt_io_t *io, void *ctx,
    uint64_t hwsp, intel_bcs_state_t *s)
{ return intel_cs_enable(INTEL_CS_COPY,io,ctx,hwsp,s); }
enum intel_bcs_status intel_bcs_submit(const intel_gt_io_t *io, void *ctx,
    bool (*read)(void *,uint64_t *),uint64_t descriptor,uint64_t token,uint32_t tail,intel_bcs_state_t *s)
{ return s && s->engine!=INTEL_CS_COPY ? INTEL_BCS_INVALID : intel_cs_submit(io,ctx,read,descriptor,token,tail,s); }
enum intel_bcs_status intel_bcs_check(const intel_gt_io_t *io,void *ctx,
    bool (*read)(void *,uint64_t *),intel_bcs_state_t *s)
{ return s && s->engine!=INTEL_CS_COPY ? INTEL_BCS_INVALID : intel_cs_check(io,ctx,read,s); }
enum intel_bcs_status intel_bcs_wait(const intel_gt_io_t *io,void *ctx,
    bool (*read)(void *,uint64_t *),intel_bcs_state_t *s)
{ return s && s->engine!=INTEL_CS_COPY ? INTEL_BCS_INVALID : intel_cs_wait(io,ctx,read,s); }
enum intel_bcs_status intel_bcs_invalidate_tlb(const intel_gt_io_t *io,void *ctx,intel_bcs_state_t *s)
{ return s && s->engine!=INTEL_CS_COPY ? INTEL_BCS_INVALID : intel_cs_invalidate_tlb(io,ctx,s); }
enum intel_bcs_status intel_bcs_close(const intel_gt_io_t *io,void *ctx,intel_bcs_state_t *s)
{ return s && s->engine!=INTEL_CS_COPY ? INTEL_BCS_INVALID : intel_cs_close(io,ctx,s); }
enum intel_bcs_status intel_bcs_run(const intel_gt_io_t *io,void *ctx,
    bool (*read)(void *,uint64_t *),uint64_t descriptor,uint64_t hwsp,uint64_t token,
    uint32_t tail,intel_bcs_state_t *s)
{ return intel_cs_run(INTEL_CS_COPY,io,ctx,read,descriptor,hwsp,token,tail,s); }
