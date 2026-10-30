#include "intel_cs.h"

static uint32_t cs_base(const intel_cs_state_t *s)
{ return s->engine==INTEL_CS_RENDER ? 0x2000u : 0x22000u; }
#define CS_BASE cs_base(s)
#define CS_HEAD (CS_BASE+0x34)
#define CS_TAIL (CS_BASE+0x30)
#define CS_HWS (CS_BASE+0x80)
#define CS_MI_MODE (CS_BASE+0x9c)
#define CS_RESET (CS_BASE+0xd0)
#define CS_ELSP (CS_BASE+0x230)
#define CS_STATUS (CS_BASE+0x234)
#define CS_MODE (CS_BASE+0x29c)
#define CS_CSB (CS_BASE+0x370)
#define CS_CSB_PTR (CS_BASE+0x3a0)
#define CS_ESR (CS_BASE+0xb8)
#define CS_RESET_MASK (s->engine==INTEL_CS_RENDER ? 2u : 8u)
#define CS_TLB (s->engine==INTEL_CS_RENDER ? 0x4260u : 0x426cu)
#define CS_POLL_LIMIT 200000u

static bool cs_read(const intel_gt_io_t *io, void *ctx, intel_cs_state_t *s,
                     uint32_t reg, uint32_t *value)
{
    s->fault_reg = reg;
    if (!io->read(ctx, reg, value)) { s->status = INTEL_CS_IO; return false; }
    s->last_value = *value;
    return true;
}

static bool cs_write(const intel_gt_io_t *io, void *ctx, intel_cs_state_t *s,
                      uint32_t reg, uint32_t value)
{
    s->fault_reg = reg;
    if (!io->write(ctx, reg, value)) { s->status = INTEL_CS_IO; return false; }
    /* ELSP is write-only. Its four dwords must be submitted in order. */
    return reg == CS_ELSP || cs_read(io, ctx, s, reg, &value);
}

static bool cs_poll(const intel_gt_io_t *io, void *ctx, intel_cs_state_t *s,
                     uint32_t reg, uint32_t mask, uint32_t want)
{
    uint64_t start = io->now_ns(ctx);
    for (unsigned i = 0; i < CS_POLL_LIMIT; i++) {
        uint32_t value;
        if (!cs_read(io, ctx, s, reg, &value)) return false;
        if (value != UINT32_MAX && (value & mask) == want) return true;
        if (io->now_ns(ctx) - start >= 2000000) break;
        io->relax(ctx);
    }
    s->status = INTEL_CS_TIMEOUT;
    return false;
}

static bool cs_retire(const intel_gt_io_t *io, void *ctx, intel_cs_state_t *s)
{
    /* No buffer can be released just because its fence was written: the CS
     * still owns its context/HWSP pointers. Stop, prepare and reset BCS before
     * dropping those references. Never force a reset when stop/ready fails. */
    if (!cs_write(io, ctx, s, CS_MI_MODE, 0x01000100) ||
        !cs_poll(io, ctx, s, CS_MI_MODE, 0x300, 0x300) ||
        !cs_write(io, ctx, s, CS_RESET, 0x10001) ||
        !cs_poll(io, ctx, s, CS_RESET, 2, 2)) return false;
    for (unsigned i = 0; i < 2; i++)
        if (!cs_write(io, ctx, s, 0x941c, CS_RESET_MASK) ||
            !cs_poll(io, ctx, s, 0x941c, CS_RESET_MASK, 0)) return false;
    uint64_t start = io->now_ns(ctx);
    unsigned n;
    for (n = 0; n < CS_POLL_LIMIT; n++) {
        if (io->now_ns(ctx) - start >= 50000) break;
        io->relax(ctx);
    }
    if (n == CS_POLL_LIMIT) { s->status = INTEL_CS_TIMEOUT; return false; }
    if (!cs_write(io, ctx, s, CS_RESET, 0x10000) ||
        !cs_poll(io, ctx, s, CS_RESET, 3, 0) ||
        !cs_write(io, ctx, s, CS_MI_MODE, 0x01000100) ||
        !cs_poll(io, ctx, s, CS_MI_MODE, 0x300, 0x300) ||
        !cs_write(io, ctx, s, CS_BASE + 0x98, UINT32_MAX) ||
        !cs_write(io, ctx, s, CS_HWS, 0) ||
        !cs_poll(io, ctx, s, CS_HWS, UINT32_MAX, 0) ||
        !cs_write(io, ctx, s, CS_MODE, 0x80000000) ||
        !cs_poll(io, ctx, s, CS_MODE, 0x8000, 0)) return false;
    s->retired = true;
    return true;
}

enum intel_cs_status intel_cs_enable(enum intel_cs_engine engine, const intel_gt_io_t *io, void *ctx,
    uint64_t hwsp_gpu, intel_cs_state_t *s)
{
    if ((unsigned)engine>INTEL_CS_RENDER || !s || s->started || !io || !io->read || !io->write || !io->now_ns || !io->relax ||
        !hwsp_gpu || (hwsp_gpu & 4095) || hwsp_gpu > 0xfffff000ULL) return INTEL_CS_INVALID;
    s->engine=engine; s->started = true; s->retired = true; s->status = INTEL_CS_IO;
    s->hwsp_gpu = (uint32_t)hwsp_gpu; s->csb_head = 5;
    uint32_t value;
    if (!cs_read(io, ctx, s, CS_MI_MODE, &value)) return s->status;
    if (value == UINT32_MAX || (value & 0x300) != 0x300) return s->status = INTEL_CS_BUSY;
    if (!cs_read(io, ctx, s, CS_STATUS, &value)) return s->status;
    if (value == UINT32_MAX || (value & 0xc01c)) return s->status = INTEL_CS_BUSY;
    if (!cs_read(io, ctx, s, CS_MODE, &value)) return s->status;
    if (value == UINT32_MAX || (value & 0x8000)) return s->status = INTEL_CS_BUSY;
    if (!cs_read(io, ctx, s, CS_ESR, &s->engine_error)) return s->status;
    if (s->engine_error) return s->status = INTEL_CS_ENGINE_ERROR;
    if (!cs_write(io, ctx, s, CS_BASE + 0x98, UINT32_MAX) ||
        !cs_write(io, ctx, s, CS_BASE + 0xa8, UINT32_MAX)) return s->status;
    s->referenced = true; s->retired = false;
    if (!cs_write(io, ctx, s, CS_HWS, (uint32_t)hwsp_gpu) ||
        !cs_poll(io, ctx, s, CS_HWS, UINT32_MAX, (uint32_t)hwsp_gpu) ||
        !cs_write(io, ctx, s, CS_CSB_PTR, 0xffff0505) ||
        !cs_poll(io, ctx, s, CS_CSB_PTR, 0x707, 0x505) ||
        !cs_write(io, ctx, s, CS_MODE, 0x80008000) ||
        !cs_poll(io, ctx, s, CS_MODE, 0x8000, 0x8000) ||
        !cs_write(io, ctx, s, CS_MI_MODE, 0x01000000) ||
        !cs_poll(io, ctx, s, CS_MI_MODE, 0x100, 0)) return s->status;
    s->enabled = true;
    return s->status = INTEL_CS_OK;
}

enum intel_cs_status intel_cs_submit(const intel_gt_io_t *io, void *ctx,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t expected, uint32_t expected_tail, intel_cs_state_t *s)
{
    uint64_t gpu=descriptor & 0xfffff000ULL;
    uint64_t context_bytes=s && s->engine==INTEL_CS_RENDER ?
        INTEL_CS_RENDER_CONTEXT_BYTES : INTEL_CS_COPY_CONTEXT_BYTES;
    if (!s || (unsigned)s->engine>INTEL_CS_RENDER || !s->enabled || s->retired || !io || !io->read || !io->write ||
        !io->now_ns || !io->relax || !token_read || !expected || expected==UINT64_MAX ||
        (expected_tail&7) || expected_tail>=4096 || (descriptor&0xfff)!=0x11d ||
        !gpu || gpu>0x100000000ULL-context_bytes || !(descriptor>>32) || descriptor>>53 ||
        (gpu < (uint64_t)s->hwsp_gpu+4096 && s->hwsp_gpu < gpu+context_bytes)) return INTEL_CS_INVALID;
    if (s->inflight) return INTEL_CS_BUSY;
    if (s->status != INTEL_CS_OK) return s->status;
    uint32_t value;
    if (!cs_read(io,ctx,s,CS_STATUS,&value)) return s->status;
    if (value==UINT32_MAX || (value&0xc01c)) return s->status=INTEL_CS_BUSY;
    if (!cs_read(io,ctx,s,CS_ESR,&s->engine_error)) return s->status;
    if (s->engine_error) return s->status=INTEL_CS_ENGINE_ERROR;
    if (!token_read(ctx,&s->observed_token)) return s->status=INTEL_CS_IO;
    if (s->observed_token==expected) return INTEL_CS_INVALID;
    s->context_id=(uint32_t)(descriptor>>32); s->target_tail=expected_tail;
    s->target_token=expected; s->completed=false; s->elapsed_ns=0;
    s->csb_status=s->csb_id=0; s->job_start_ns=io->now_ns(ctx);
    /* A failed fourth port write may already have started DMA. Keep every
     * resource referenced, forbid retry, and let close retire the engine. */
    s->inflight=true; s->submitted=true; s->submitted_jobs++;
    s->status=INTEL_CS_PENDING;
    if (!cs_write(io,ctx,s,CS_ELSP,0) || !cs_write(io,ctx,s,CS_ELSP,0) ||
        !cs_write(io,ctx,s,CS_ELSP,s->context_id) ||
        !cs_write(io,ctx,s,CS_ELSP,(uint32_t)descriptor)) return s->status;
    return INTEL_CS_OK;
}

enum intel_cs_status intel_cs_check(const intel_gt_io_t *io, void *ctx,
    bool (*token_read)(void *, uint64_t *), intel_cs_state_t *s)
{
    if (!s || (unsigned)s->engine>INTEL_CS_RENDER || !s->enabled || s->retired || !io || !io->read || !io->write ||
        !io->now_ns || !token_read) return INTEL_CS_INVALID;
    if (!s->inflight) return s->completed ? INTEL_CS_OK : INTEL_CS_INVALID;
    if (s->status!=INTEL_CS_PENDING) return s->status;
    uint32_t ptr;
    if (!cs_read(io,ctx,s,CS_STATUS,&s->execlist_status) ||
        !cs_read(io,ctx,s,CS_ESR,&s->engine_error) ||
        !cs_read(io,ctx,s,CS_CSB_PTR,&ptr)) return s->status;
    if (s->engine_error) return s->status=INTEL_CS_ENGINE_ERROR;
    uint32_t written=ptr&7, before=s->csb_head;
    if (written>=6) return s->status=INTEL_CS_IO;
    while (s->csb_head!=written) {
        s->csb_head=(s->csb_head+1)%6;
        if (!cs_read(io,ctx,s,CS_CSB+s->csb_head*8,&s->csb_status) ||
            !cs_read(io,ctx,s,CS_CSB+s->csb_head*8+4,&s->csb_id)) return s->status;
        if (s->csb_id==s->context_id && (s->csb_status&0x18)==0x18) s->completed=true;
    }
    if (before!=s->csb_head &&
        !cs_write(io,ctx,s,CS_CSB_PTR,0x07000000 | s->csb_head<<8)) return s->status;
    if (!token_read(ctx,&s->observed_token)) return s->status=INTEL_CS_IO;
    if (!cs_read(io,ctx,s,CS_HEAD,&s->head) || !cs_read(io,ctx,s,CS_TAIL,&s->tail)) return s->status;
    s->elapsed_ns=io->now_ns(ctx)-s->job_start_ns;
    if (s->completed && s->observed_token==s->target_token &&
        !(s->execlist_status&0xc01c) && (s->head&0x1ffffc)==s->target_tail &&
        (s->tail&0x1ffff8)==s->target_tail) {
        s->inflight=false; s->completed_jobs++;
        return s->status=INTEL_CS_OK;
    }
    if (s->elapsed_ns>=500000000) return s->status=INTEL_CS_TIMEOUT;
    return INTEL_CS_PENDING;
}

enum intel_cs_status intel_cs_wait(const intel_gt_io_t *io, void *ctx,
    bool (*token_read)(void *, uint64_t *), intel_cs_state_t *s)
{
    if (!io || !io->relax) return INTEL_CS_INVALID;
    for (unsigned n=0;n<CS_POLL_LIMIT;n++) {
        enum intel_cs_status result=intel_cs_check(io,ctx,token_read,s);
        if (result!=INTEL_CS_PENDING) return result;
        io->relax(ctx);
    }
    return s->status=INTEL_CS_TIMEOUT;
}

enum intel_cs_status intel_cs_invalidate_tlb(const intel_gt_io_t *io, void *ctx,
    intel_cs_state_t *s)
{
    if (!s || (unsigned)s->engine>INTEL_CS_RENDER || !s->enabled || s->retired || !io || !io->read || !io->write ||
        !io->now_ns || !io->relax) return INTEL_CS_INVALID;
    if (s->inflight) return INTEL_CS_BUSY;
    if (s->status!=INTEL_CS_OK) return s->status;
    if (s->submitted && (!s->completed || s->submitted_jobs!=s->completed_jobs))
        return INTEL_CS_BUSY;
    uint32_t value;
    if (!cs_read(io,ctx,s,CS_STATUS,&value)) return s->status;
    if (value==UINT32_MAX || (value&0xc01c)) return s->status=INTEL_CS_BUSY;
    if (!cs_read(io,ctx,s,CS_ESR,&s->engine_error)) return s->status;
    if (s->engine_error) return s->status=INTEL_CS_ENGINE_ERROR;
    /* KBL PRM, engine TLB control (0426Ch BCS, 04260h RCS): write bit 0; hardware clears it only after
     * invalidation. Requires a flushed, idle pipeline, established above by
     * the session's completion protocol. Never reset the engine per update. */
    if (!cs_write(io,ctx,s,CS_TLB,1) ||
        !cs_poll(io,ctx,s,CS_TLB,UINT32_MAX,0)) return s->status;
    s->tlb_invalidations++;
    return INTEL_CS_OK;
}

enum intel_cs_status intel_cs_close(const intel_gt_io_t *io, void *ctx, intel_cs_state_t *s)
{
    if (!s || (unsigned)s->engine>INTEL_CS_RENDER || !s->started || !io || !io->read || !io->write || !io->now_ns || !io->relax)
        return INTEL_CS_INVALID;
    s->enabled=false;
    if (!s->retired) {
        enum intel_cs_status result=s->status;
        uint32_t fault=s->fault_reg,last=s->last_value;
        if (!cs_retire(io,ctx,s)) return s->status=INTEL_CS_QUARANTINED;
        s->status=result; s->fault_reg=fault; s->last_value=last;
    }
    s->inflight=false;
    return INTEL_CS_OK;
}

enum intel_cs_status intel_cs_run(enum intel_cs_engine engine, const intel_gt_io_t *io, void *ctx,
    bool (*token_read)(void *, uint64_t *), uint64_t descriptor,
    uint64_t hwsp_gpu, uint64_t expected, uint32_t expected_tail, intel_cs_state_t *s)
{
    uint64_t gpu=descriptor&0xfffff000ULL;
    uint64_t context_bytes=engine==INTEL_CS_RENDER ?
        INTEL_CS_RENDER_CONTEXT_BYTES : INTEL_CS_COPY_CONTEXT_BYTES;
    if ((unsigned)engine>INTEL_CS_RENDER || !s || s->started || !token_read || !expected || expected==UINT64_MAX ||
        !expected_tail || (expected_tail&7) || expected_tail>=4096 ||
        (descriptor&0xfff)!=0x11d || !gpu || gpu>0x100000000ULL-context_bytes ||
        !(descriptor>>32) || descriptor>>53 || hwsp_gpu>0xfffff000ULL ||
        (gpu < hwsp_gpu+4096 && hwsp_gpu < gpu+context_bytes)) return INTEL_CS_INVALID;
    enum intel_cs_status result=intel_cs_enable(engine,io,ctx,hwsp_gpu,s);
    if (result==INTEL_CS_OK) result=intel_cs_submit(io,ctx,token_read,descriptor,expected,expected_tail,s);
    if (result==INTEL_CS_OK) result=intel_cs_wait(io,ctx,token_read,s);
    if (s->started && intel_cs_close(io,ctx,s)==INTEL_CS_QUARANTINED) return INTEL_CS_QUARANTINED;
    return result;
}
