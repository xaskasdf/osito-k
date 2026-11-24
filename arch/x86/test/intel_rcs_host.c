/* Shared protocol model with strict RCS register/reset/address translation. */
#define main bcs_baseline_main
#include "intel_bcs_host.c"
#undef main
#include "../drivers/intel_rcs.h"
static uint32_t rcs_context[INTEL_RCS_CONTEXT_BYTES/4+2],rcs_ring[1026];
static unsigned render_resets,render_invalidations;

static bool rcs_model_read(void *ctx,uint32_t reg,uint32_t *value)
{
    if (reg==0x4260) { *value=0; return ++hw.ops!=hw.fail_at; }
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    bool result=bcs_model_read(ctx,reg==0x941c ? reg : reg+0x20000,value);
    if (reg==0x941c && *value==8) *value=2;
    if (reg==0x2080 && *value==0x1003000) *value=0x1020000;
    return result;
}
static bool rcs_model_write(void *ctx,uint32_t reg,uint32_t value)
{
    if (reg==0x4260) { CHECK(value==1); render_invalidations++; return ++hw.ops!=hw.fail_at; }
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    if (reg==0x941c) { CHECK(value==2); render_resets++; value=8; }
    if (reg==0x2080 && value) { CHECK(value==0x1020000); value=0x1003000; }
    return bcs_model_write(ctx,reg==0x941c ? reg : reg+0x20000,value);
}
static const intel_gt_io_t rcs_io={rcs_model_read,rcs_model_write,bcs_model_now,bcs_model_relax};

static void rcs_build_cases(void)
{
    memset(rcs_context,0x55,sizeof(rcs_context)); memset(rcs_ring,0x77,sizeof(rcs_ring));
    uint32_t *c=rcs_context+1,*r=rcs_ring+1,*s=c+1024;
    uint64_t descriptor=0;
    CHECK(intel_rcs_build(c,INTEL_RCS_CONTEXT_BYTES,r,4096,0x1000000,0x1016000,
        0x123456000ULL,0x1001,token,&descriptor));
    CHECK(rcs_context[0]==0x55555555 && rcs_context[INTEL_RCS_CONTEXT_BYTES/4+1]==0x55555555);
    CHECK(rcs_ring[0]==0x77777777 && rcs_ring[1025]==0x77777777);
    CHECK(descriptor==0x10010100011dULL && s[2]==0x2244 && s[3]==0xf0009);
    CHECK(s[7]==64 && s[9]==0x1016000 && s[11]==1 && s[49]==1 && s[51]==0x23456000);
    CHECK(s[65]==0x11000001 && s[66]==0x20c8);
    CHECK(r[0]==0x7a000004 && r[6]==0x7a000004 && r[8]==0x800 && r[9]==0);
    CHECK(r[7]==0x00345cbdu && r[10]==(uint32_t)token && r[11]==(uint32_t)(token>>32));
    for (unsigned i=0;i<1024;i++) CHECK(c[i]==0);
    for (unsigned i=2048;i<INTEL_RCS_CONTEXT_BYTES/4;i++) CHECK(!c[i]);
    for (unsigned i=12;i<1024;i++) CHECK(!r[i]);
    uint32_t saved[INTEL_RCS_CONTEXT_BYTES/4]; memcpy(saved,c,sizeof(saved));
    uint64_t guard=0xaabb;
    CHECK(!intel_rcs_build(c,8192,r,4096,0x1000000,0x1016000,0x12345000,1,1,&guard));
    CHECK(!intel_rcs_build(c,sizeof(saved),r,4096,0x1000000,0x1015000,0x12345000,1,1,&guard));
    CHECK(!intel_rcs_build(c,sizeof(saved),r,4096,0xfffff000,0x1016000,0x12345000,1,1,&guard));
    CHECK(!intel_rcs_build(c,sizeof(saved),c+2048,4096,0x1000000,0x1016000,0x12345000,1,1,&guard));
    CHECK(!intel_rcs_build(c,sizeof(saved),r,4096,0x1000000,0x1016000,0x12345001,1,1,&guard));
    CHECK(!intel_rcs_build(c,sizeof(saved),r,4096,0x1000000,0x1016000,0x12345000,1,0,&guard));
    CHECK(guard==0xaabb && !memcmp(saved,c,sizeof(saved)));
    for (unsigned i=2048;i<INTEL_RCS_CONTEXT_BYTES/4;i++) c[i]=0x79ab0000^i;
    uint32_t tail=64,next=0x999;
    CHECK(!intel_rcs_append(c,sizeof(saved),r,4096,tail,token+1,&next) && next==0x999);
    for (unsigned round=0;round<132;round++) {
        s[5]=s[7]=tail; /* confirmed hardware-saved idle context */
        CHECK(intel_rcs_append(c,sizeof(saved),r,4096,tail,token+round+1,&next));
        CHECK(next==(tail+64)%4096 && s[3]==0xf0008);
        CHECK(r[tail/4+10]==(uint32_t)(token+round+1));
        tail=next;
        for (unsigned i=2048;i<INTEL_RCS_CONTEXT_BYTES/4;i++) CHECK(c[i]==(0x79ab0000^i));
    }
}

static void rcs_lifetime_cases(void)
{
    const uint64_t desc=0x10010100011dULL;
    bcs_model_reset(); hw.tail=64; intel_cs_state_t s={0};
    CHECK(intel_cs_run(INTEL_CS_RENDER,&rcs_io,&hw,bcs_model_token,desc,0x1020000,token,64,&s)==INTEL_CS_OK);
    CHECK(s.completed && s.retired && s.engine==INTEL_CS_RENDER && render_resets==2);
    unsigned operations=hw.ops;
    for (unsigned failure=1;failure<=operations;failure++) {
        bcs_model_reset(); hw.tail=64; hw.fail_at=failure; s=(intel_cs_state_t){0};
        CHECK(intel_cs_run(INTEL_CS_RENDER,&rcs_io,&hw,bcs_model_token,desc,0x1020000,token,64,&s)!=INTEL_CS_OK);
        if (!s.retired && s.started) CHECK(intel_cs_close(&rcs_io,&hw,&s)==INTEL_CS_OK && s.retired);
    }
    for (unsigned scenario=0;scenario<5;scenario++) {
        bcs_model_reset(); hw.tail=64; s=(intel_cs_state_t){0};
        hw.missing_token=scenario==0; hw.wrong_id=scenario==1; hw.wrong_head=scenario==2;
        hw.stall_stop=scenario==3; hw.stall_reset=scenario==4;
        enum intel_cs_status result=intel_cs_run(INTEL_CS_RENDER,&rcs_io,&hw,bcs_model_token,desc,0x1020000,token,64,&s);
        CHECK(result==(scenario<3 ? INTEL_CS_TIMEOUT : INTEL_CS_QUARANTINED));
        if (scenario>=3) {
            CHECK(!s.retired); hw.stall_stop=hw.stall_reset=false;
            CHECK(intel_cs_close(&rcs_io,&hw,&s)==INTEL_CS_OK && s.retired);
        }
    }
    bcs_model_reset(); hw.tail=64; s=(intel_cs_state_t){0};
    CHECK(intel_cs_run(INTEL_CS_RENDER,&rcs_io,&hw,bcs_model_token,desc,0x1003000,token,64,&s)==INTEL_CS_INVALID);
    CHECK(!hw.ops && !s.started); /* HWSP must not overlap any of the 22 pages */
    CHECK(intel_cs_enable(INTEL_CS_RENDER,&rcs_io,&hw,0x1020000,&s)==INTEL_CS_OK);
    intel_bcs_queue_t render_queue={0};
    CHECK(intel_bcs_queue_init(&render_queue,&s,&rcs_io,&hw,&queue_ops,1)==INTEL_CS_OK);
    CHECK(intel_bcs_close(&rcs_io,&hw,&s)==INTEL_CS_INVALID && s.enabled);
    for (unsigned round=0;round<70;round++) {
        hw.tail=((round+1)*64)%4096; hw.next_token=token+round;
        CHECK(intel_cs_submit(&rcs_io,&hw,bcs_model_token,desc,hw.next_token,hw.tail,&s)==INTEL_CS_OK);
        CHECK(intel_cs_invalidate_tlb(&rcs_io,&hw,&s)==INTEL_CS_BUSY);
        CHECK(intel_cs_wait(&rcs_io,&hw,bcs_model_token,&s)==INTEL_CS_OK);
        CHECK(intel_cs_invalidate_tlb(&rcs_io,&hw,&s)==INTEL_CS_OK);
    }
    CHECK(s.completed_jobs==70 && s.tlb_invalidations==70 && render_invalidations==70);
    CHECK(intel_cs_close(&rcs_io,&hw,&s)==INTEL_CS_OK && s.retired);
    CHECK(intel_bcs_queue_shutdown(&render_queue)==INTEL_CS_OK);
}
int main(void)
{
    rcs_build_cases(); rcs_lifetime_cases();
    printf("Intel RCS context/session: %u checks passed\n",checks);
    return 0;
}
