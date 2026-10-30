#define _GNU_SOURCE
/* Production context encoding and MMIO retirement under injected failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../drivers/intel_bcs.h"
#include "../drivers/intel_bcs_queue.h"
static void bcs_queue_host_tests(void);
static void test_queue_stage(void);
static unsigned checks;
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if (!(x)) { \
    fprintf(stderr,"FAIL BCS %d: %s\n",__LINE__,#x); abort(); \
} } while (0)
static uint32_t context[2050], ring[1026];
static const uint64_t token = 0x123456789abcdef0ULL;
static struct {
    uint32_t regs[0x400/4], elsp[4], resets, writes, ops, fail_at;
    uint32_t status_reads, delay, mode, tail, csb_written;
    uint64_t ns, token_value, next_token,expected_descriptor;
    bool frozen, missing_token, wrong_id, wrong_head, stall_reset, stall_stop, stall_ready, emitted;
} hw;

static bool bcs_model_read(void *ctx, uint32_t reg, uint32_t *value)
{
    CHECK(ctx == &hw);
    if (++hw.ops == hw.fail_at) return false;
    if (reg == 0x941c) { *value = hw.stall_reset ? 8 : 0; return true; }
    CHECK(reg >= 0x22000 && reg < 0x22400 && !(reg & 3));
    if (reg == 0x22234 && hw.mode == 4 && !hw.resets && !hw.emitted && ++hw.status_reads >= hw.delay) {
        hw.regs[0x234/4] = 1;
        unsigned a=(hw.csb_written+1)%6,b=(a+1)%6;
        hw.regs[(0x370+a*8)/4]=1; hw.regs[(0x374+a*8)/4]=0;
        hw.regs[(0x370+b*8)/4]=0x18; hw.regs[(0x374+b*8)/4]=hw.wrong_id ? 9 : hw.elsp[2];
        hw.csb_written=b;
        hw.regs[0x3a0/4] = (hw.regs[0x3a0/4] & ~7u) | b;
        hw.emitted=true;
        hw.regs[0x34/4] = hw.wrong_head ? 0 : hw.tail;
        hw.regs[0x30/4] = hw.tail;
    }
    *value = hw.regs[(reg-0x22000)/4];
    return true;
}

static bool bcs_model_write(void *ctx, uint32_t reg, uint32_t value)
{
    CHECK(ctx == &hw);
    bool failed = ++hw.ops == hw.fail_at;
    hw.writes++;
    if (reg == 0x941c) {
        CHECK(value == 8);
        if (!hw.resets) CHECK((hw.regs[0x9c/4] & 0x300) == 0x300 && hw.regs[0xd0/4] == 3);
        hw.resets++;
        if (!hw.stall_reset) {
            hw.regs[0x9c/4] = 0x200; hw.regs[0xd0/4] = 0;
            hw.regs[0x234/4] = 1; hw.regs[0x29c/4] = 0;
        }
        return !failed;
    }
    CHECK(reg >= 0x22000 && reg < 0x22400 && !(reg & 3));
    uint32_t off = reg-0x22000, *v = &hw.regs[off/4];
    switch (off) {
    case 0x98: case 0xa8: CHECK(value == UINT32_MAX); *v = value; break;
    case 0x80: CHECK(value == 0x1003000 || value == 0); *v = value; break;
    case 0x9c: case 0x29c: case 0x3a0:
        *v = (*v & ~(value >> 16)) | (value & (value >> 16));
        if (off == 0x9c) *v |= 0x200;
        if (off == 0x9c && value == 0x01000100 && hw.stall_stop) *v &= ~0x200u;
        break;
    case 0xd0:
        CHECK(value == 0x10000 || value == 0x10001);
        *v = (value & 1) ? (hw.stall_ready ? 1 : 3) : 0;
        break;
    case 0x230:
        if (hw.mode==4 && hw.emitted && !(hw.regs[0x234/4]&0xc01c)) hw.mode=0;
        CHECK(hw.mode < 4 && hw.regs[0x80/4] == 0x1003000);
        CHECK((hw.regs[0x29c/4] & 0x8000) && !(hw.regs[0x9c/4] & 0x100));
        hw.elsp[hw.mode++] = value;
        if (hw.mode == 4) {
            CHECK(!hw.elsp[0] && !hw.elsp[1]);
            if (hw.expected_descriptor) CHECK(((uint64_t)hw.elsp[2]<<32 | hw.elsp[3])==hw.expected_descriptor);
            else {
                CHECK(hw.elsp[2]==0x1001 || hw.elsp[2]==0x1002);
                CHECK(hw.elsp[3]==0x0100011d+(hw.elsp[2]-0x1001)*0x4000);
            }
            hw.regs[0x234/4] = 0x4010;
            hw.status_reads=0; hw.emitted=false; hw.token_value=hw.next_token;
        }
        break;
    default: CHECK(!"unexpected write");
    }
    return !failed; /* the MMIO write may apply despite a reported error */
}

static uint64_t bcs_model_now(void *ctx)
{
    CHECK(ctx == &hw);
    if (!hw.frozen) hw.ns += 10000;
    return hw.ns;
}
static void bcs_model_relax(void *ctx) { CHECK(ctx == &hw); }
static bool bcs_model_token(void *ctx, uint64_t *value)
{
    CHECK(ctx == &hw);
    if (++hw.ops == hw.fail_at) return false;
    *value = !hw.missing_token ? hw.token_value : 0;
    return true;
}
static void bcs_model_reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.regs[0x9c/4] = 0x300; hw.regs[0x234/4] = 1; hw.delay = 3; hw.tail = 32;
    hw.csb_written=5; hw.next_token=token;
}
int main(void)
{
    uint64_t descriptor = 0;
    memset(context, 0x55, sizeof(context)); memset(ring, 0x77, sizeof(ring));
    CHECK(intel_bcs_build(context+1, 8192, ring+1, 4096, 0x1000000, 0x1002000,
                           0x112340000ULL, 0x1001, token, &descriptor));
    CHECK(descriptor == 0x10010100011dULL);
    CHECK(context[0] == 0x55555555 && context[2049] == 0x55555555);
    CHECK(ring[0] == 0x77777777 && ring[1025] == 0x77777777);
    uint32_t *s = context+1+1024, *r = ring+1;
    CHECK(s[1] == 0x1100101b && s[2] == 0x22244 && s[3] == 0xf0009);
    CHECK(s[5] == 0 && s[7] == 32 && s[9] == 0x1002000 && s[11] == 1);
    CHECK(s[48] == 0x22274 && s[49] == 1 && s[50] == 0x22270 && s[51] == 0x12340000);
    CHECK(s[33] == 0x11001011 && s[65] == 0x11001001 && s[66] == 0x22200);
    CHECK(r[0] == 0x13244003 && r[1] == 0x800 && r[2] == 0);
    CHECK(r[3] == 0x9abcdef0 && r[4] == 0x12345678);
    for (unsigned i = 5; i < 1024; i++) CHECK(!r[i]);
    for (unsigned i = 0; i < 1024; i++) CHECK(context[1+i] == 0);
    uint32_t saved[2050]; memcpy(saved, context, sizeof(saved));
    CHECK(!intel_bcs_build(context+1,8191,ring+1,4096,0x1000000,0x1002000,0x400000,1,token,&descriptor));
    CHECK(!intel_bcs_build(context+1,8192,context+2,4096,0x1000000,0x1002000,0x400000,1,token,&descriptor));
    const uint64_t bad[] = {0,1,4095,0xfffff000,0x100000000ULL,UINT64_MAX};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)
        CHECK(!intel_bcs_build(context+1,8192,ring+1,4096,bad[i],0x1002000,0x400000,1,token,&descriptor));
    CHECK(!intel_bcs_build(context+1,8192,ring+1,4096,0x1000000,0x1001000,0x400000,1,token,&descriptor));
    CHECK(!intel_bcs_build(context+1,8192,ring+1,4096,0x1000000,0x1002000,1ULL<<39,1,token,&descriptor));
    CHECK(!intel_bcs_build(context+1,8192,ring+1,4096,0x1000000,0x1002000,0x400000,1u<<21,token,&descriptor));
    CHECK(!memcmp(saved,context,sizeof(saved)));
    const intel_gt_io_t io = {bcs_model_read,bcs_model_write,bcs_model_now,bcs_model_relax};
    intel_bcs_state_t state = {0}; bcs_model_reset();
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_OK);
    CHECK(state.submitted && state.completed && state.retired && hw.resets==2 && hw.status_reads>=3);
    unsigned operations=hw.ops;
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_INVALID);
    CHECK(hw.ops==operations);
    for(unsigned fail=1;fail<=operations;fail++) {
        bcs_model_reset(); memset(&state,0,sizeof(state)); hw.fail_at=fail;
        enum intel_bcs_status result=intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state);
        CHECK(result!=INTEL_BCS_OK);
        CHECK(state.retired || result==INTEL_BCS_QUARANTINED);
        CHECK(!state.referenced || !state.retired || hw.resets==2);
        CHECK(hw.mode<=4); /* no retry submission */
    }
    for(unsigned bad_completion=0;bad_completion<3;bad_completion++) {
        bcs_model_reset(); memset(&state,0,sizeof(state));
        hw.missing_token=bad_completion==0; hw.wrong_id=bad_completion==1; hw.wrong_head=bad_completion==2;
        CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_TIMEOUT);
        CHECK(state.retired && hw.resets==2);
    }
    bcs_model_reset(); memset(&state,0,sizeof(state)); hw.stall_reset=true;
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_QUARANTINED);
    CHECK(state.completed && !state.retired);
    for (unsigned stop = 0; stop < 2; stop++) {
        bcs_model_reset(); memset(&state,0,sizeof(state));
        hw.stall_stop = stop != 0; hw.stall_ready = stop == 0;
        CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_QUARANTINED);
        CHECK(state.completed && !state.retired && !hw.resets);
    }
    bcs_model_reset(); memset(&state,0,sizeof(state)); hw.frozen=true; hw.missing_token=true;
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_QUARANTINED);
    CHECK(!state.retired && hw.ops<3000000);
    bcs_model_reset(); memset(&state,0,sizeof(state)); hw.regs[0x234/4]=0x4000;
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,32,&state)==INTEL_BCS_BUSY);
    CHECK(state.retired && !hw.writes);
    bcs_model_reset(); memset(&state,0,sizeof(state));
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1000000,token,32,&state)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,0x1fffff11dULL,0x1003000,token,32,&state)==INTEL_BCS_INVALID);
    CHECK(!hw.ops);
    intel_bcs_copy_t copy = {0x1235ff000ULL,8192,0x1403ff000ULL,8192,256,256,37,23,0,0,0,0};
    CHECK(intel_bcs_build_copy(context+1,8192,ring+1,4096,0x1000000,0x1002000,
        0x112340000ULL,0x1001,token,&copy,&descriptor));
    CHECK(s[7]==64 && s[67]==0xf0000);
    CHECK(r[0]==0x54f00008 && r[1]==0x03cc0100 && r[2]==0 && r[3]==0x170025);
    CHECK(r[4]==0x403ff000 && r[5]==1 && !r[6] && r[7]==256 && r[8]==0x235ff000 && r[9]==1);
    CHECK(r[10]==0x13244003 && r[11]==0x800 && !r[12] && r[13]==0x9abcdef0 && r[14]==0x12345678);
    for(unsigned i=15;i<1024;i++) CHECK(!r[i]);
    bcs_model_reset(); memset(&state,0,sizeof(state)); hw.tail=64;
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,64,&state)==INTEL_BCS_OK);
    CHECK(state.completed && state.retired && state.head==64 && state.tail==64);
    uint32_t saved_ring[1026]; memcpy(saved_ring,ring,sizeof(ring));
    memcpy(saved,context,sizeof(context));
    for(unsigned bad_copy=0;bad_copy<8;bad_copy++) {
        intel_bcs_copy_t bad=copy;
        if(bad_copy==0) bad.width=65;
        if(bad_copy==1) bad.height=33;
        if(bad_copy==2) bad.source_pitch=255;
        if(bad_copy==3) bad.destination_pitch=32768;
        if(bad_copy==4) bad.source=UINT64_MAX-63;
        if(bad_copy==5) bad.destination=copy.source+4096;
        if(bad_copy==6) bad.destination_bytes=32;
        if(bad_copy==7) bad.height=0;
        CHECK(!intel_bcs_build_copy(context+1,8192,ring+1,4096,0x1000000,0x1002000,
            0x112340000ULL,0x1001,token,&bad,&descriptor));
        CHECK(!memcmp(saved,context,sizeof(context)) && !memcmp(saved_ring,ring,sizeof(ring)));
    }
    bcs_model_reset(); memset(&state,0,sizeof(state));
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,0,&state)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,4096,&state)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_run(&io,&hw,bcs_model_token,descriptor,0x1003000,token,33,&state)==INTEL_BCS_INVALID);
    CHECK(!hw.ops && !state.started);
    /* One engine session, alternating contexts, CSB wrap and fresh fences.
     * Even early GPU-token visibility cannot retire a job before the CSB. */
    bcs_model_reset(); memset(&state,0,sizeof(state));
    CHECK(intel_bcs_enable(&io,&hw,0x1003000,&state)==INTEL_BCS_OK);
    CHECK(state.enabled && state.referenced && !state.retired);
    for(unsigned job=0;job<20;job++) {
        uint64_t desc=(job&1)?0x10020100411dULL:descriptor;
        hw.next_token=token+job; hw.tail=(job/2+1)*64;
        CHECK(intel_bcs_submit(&io,&hw,bcs_model_token,desc,hw.next_token,hw.tail,&state)==INTEL_BCS_OK);
        unsigned prior_ops=hw.ops;
        CHECK(intel_bcs_submit(&io,&hw,bcs_model_token,desc,hw.next_token,hw.tail,&state)==INTEL_BCS_BUSY);
        CHECK(hw.ops==prior_ops && state.inflight);
        CHECK(intel_bcs_check(&io,&hw,bcs_model_token,&state)==INTEL_BCS_PENDING);
        CHECK(intel_bcs_wait(&io,&hw,bcs_model_token,&state)==INTEL_BCS_OK);
        CHECK(!state.inflight && state.completed && !state.retired && !hw.resets);
        CHECK(state.completed_jobs==job+1 && state.submitted_jobs==job+1);
    }
    CHECK(intel_bcs_close(&io,&hw,&state)==INTEL_BCS_OK && state.retired && !state.enabled && hw.resets==2);
    CHECK(intel_bcs_submit(&io,&hw,bcs_model_token,descriptor,token+20,64,&state)==INTEL_BCS_INVALID);
    for(unsigned fail=4;fail<=7;fail++) {
        bcs_model_reset(); memset(&state,0,sizeof(state));
        CHECK(intel_bcs_enable(&io,&hw,0x1003000,&state)==INTEL_BCS_OK);
        hw.fail_at=hw.ops+fail;
        CHECK(intel_bcs_submit(&io,&hw,bcs_model_token,descriptor,token,32,&state)==INTEL_BCS_IO);
        CHECK(state.inflight && !state.retired);
        CHECK(intel_bcs_submit(&io,&hw,bcs_model_token,descriptor,token,32,&state)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_close(&io,&hw,&state)==INTEL_BCS_OK && state.retired);
    }
    /* Saved ring append preserves the hardware engine image and wrap count. */
    CHECK(intel_bcs_build_copy(context+1,8192,ring+1,4096,0x1000000,0x1002000,
        0x112340000ULL,0x1001,token,&copy,&descriptor));
    uint32_t tail=64;
    s[100]=0x1234abcd;
    for(unsigned job=1;job<140;job++) {
        s[5]=tail|0x00600000; /* hardware-saved head + wrap count */
        CHECK(intel_bcs_append_copy(context+1,8192,ring+1,4096,tail,token+job,&copy,&tail));
        CHECK(s[3]==0xf0008 && s[7]==tail && s[100]==0x1234abcd);
        CHECK(tail==((job+1)*64%4096));
        CHECK((s[5]&0xffe00000)==0x00600000);
    }
    memcpy(saved,context,sizeof(context)); memcpy(saved_ring,ring,sizeof(ring));
    CHECK(!intel_bcs_append_copy(context+1,8192,ring+1,4096,tail,token+150,&copy,&tail));
    CHECK(!memcmp(saved,context,sizeof(context)) && !memcmp(saved_ring,ring,sizeof(ring)));
    printf("Intel BCS: %u checks passed\n",checks);
    bcs_queue_host_tests();
    test_queue_stage();
    return 0;
}

/* The same MMIO model, through per-context queue callbacks. The production
 * scheduler calls real enable/submit/check/close, never synthetic fences. */
static intel_bcs_queue_t queue;
static intel_bcs_state_t queue_engine;
static unsigned queue_backing[INTEL_BCS_QUEUE_CONTEXTS];
static uint64_t queue_ids[INTEL_BCS_QUEUE_CONTEXTS];
static uint64_t prepared[256];
static unsigned prepares;
static bool prepare_failure;
static unsigned race_stage;
static pthread_barrier_t race_enter,race_release;
static void bcs_queue_race_test(void);
extern void d3dkmt_release_process(uint32_t);

static void bcs_queue_race_point(unsigned stage)
{
    if (race_stage!=stage) return;
    (void)pthread_barrier_wait(&race_enter);
    (void)pthread_barrier_wait(&race_release);
}

static bool bcs_queue_model_read(void *ctx, uint32_t reg, uint32_t *value)
{
    (void)ctx;
    if (reg==0x22234 && hw.mode==4 && !hw.resets) bcs_queue_race_point(3);
    return bcs_model_read(&hw,reg,value);
}
static bool bcs_queue_model_write(void *ctx, uint32_t reg, uint32_t value)
{
    (void)ctx;
    if (reg==0x22230 && hw.mode==3) bcs_queue_race_point(2);
    if (reg==0x2209c && value==0x01000100 && !hw.resets) bcs_queue_race_point(4);
    return bcs_model_write(&hw,reg,value);
}
static uint64_t bcs_queue_model_now(void *ctx)
{ (void)ctx; return bcs_model_now(&hw); }
static void bcs_queue_model_relax(void *ctx)
{ (void)ctx; bcs_model_relax(&hw); }
static bool bcs_queue_model_token(void *ctx, uint64_t *value)
{ (void)ctx; return bcs_model_token(&hw,value); }
static bool bcs_queue_model_prepare(void *ctx, const intel_bcs_copy_t *copy,
    uint64_t fence, uint64_t *descriptor, uint32_t *tail)
{
    CHECK(ctx==&queue_backing[0] || ctx==&queue_backing[1]);
    CHECK(copy->width==37 && copy->height==23);
    CHECK(queue.active<INTEL_BCS_QUEUE_JOBS && queue.worker_busy && !queue_engine.inflight);
    if (prepare_failure) return false;
    unsigned index=*(unsigned *)ctx;
    CHECK(queue.contexts[index].references && !queue.contexts[index].closing);
    *descriptor=index ? 0x10020100411dULL : 0x10010100011dULL;
    *tail=hw.tail=((prepares+1)*64)&4095;
    hw.next_token=fence;
    CHECK(prepares<256);
    prepared[prepares++]=fence;
    bcs_queue_race_point(1);
    return true;
}
static const intel_gt_io_t queue_io={bcs_queue_model_read,bcs_queue_model_write,
    bcs_queue_model_now,bcs_queue_model_relax};
static const intel_bcs_queue_ops_t queue_ops={.prepare=bcs_queue_model_prepare,.token_read=bcs_queue_model_token};
static const intel_bcs_copy_t queue_copy={0x1235ff000ULL,8192,0x1403ff000ULL,8192,256,256,37,23,0,0,0,0};
static void bcs_queue_access_cases(void);
static void bcs_queue_access_races(void);

static void bcs_queue_host_start(void)
{
    memset(&queue,0,sizeof(queue)); memset(&queue_engine,0,sizeof(queue_engine));
    bcs_model_reset(); prepares=0; prepare_failure=false;
    CHECK(intel_bcs_enable(&queue_io,NULL,0x1003000,&queue_engine)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_init(&queue,&queue_engine,&queue_io,NULL,&queue_ops,token)==INTEL_BCS_OK);
    for (unsigned i=0;i<2;i++) {
        queue_backing[i]=i;
        CHECK(intel_bcs_queue_attach(&queue,41+i,&queue_backing[i],&queue_ids[i])==INTEL_BCS_OK);
    }
}

static void test_queue_stage(void)
{
    unsigned before=checks;
    for (unsigned mode=0;mode<7;mode++) {
        bcs_queue_host_start();
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        unsigned word=11;
        intel_bcs_queue_gate_t gate={&word,11,17,23};
        uint64_t fence=0xfeed;
        intel_bcs_queue_gate_t invalid=gate; invalid.accepted=11;
        CHECK(intel_bcs_queue_stage(&queue,41,queue_ids[0],&queue_copy,&invalid,&fence)==INTEL_BCS_INVALID && fence==0xfeed);
        CHECK(intel_bcs_queue_stage(&queue,41,queue_ids[0],&queue_copy,&gate,&fence)==INTEL_BCS_OK);
        unsigned writes=hw.writes;
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_OK && hw.writes==writes && !prepares);
        intel_bcs_queue_snapshot_t snapshot;
        CHECK(intel_bcs_queue_snapshot(&queue,&snapshot)==INTEL_BCS_OK && snapshot.staged==1 && !snapshot.queued);
        CHECK(intel_bcs_queue_finish_stage(&queue,41,fence,&word)==INTEL_BCS_BUSY);
        if (mode==0 || mode==6) __atomic_store_n(&word,17,__ATOMIC_RELEASE);
        if (mode==1) __atomic_store_n(&word,23,__ATOMIC_RELEASE);
        if (mode==2 || mode==6) CHECK(intel_bcs_queue_close_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
        if (mode==3) intel_bcs_release_process(41);
        if (mode==4) CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        if (mode==5) CHECK(intel_bcs_queue_cancel(&queue,41,fence)==INTEL_BCS_OK);
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_finish_stage(&queue,41,fence,&word)==INTEL_BCS_OK);
        if (mode==0) {
            enum intel_bcs_status status=intel_bcs_queue_step(&queue);
            CHECK(status==INTEL_BCS_PENDING && prepares==1);
            do { status=intel_bcs_queue_step(&queue); } while (status==INTEL_BCS_PENDING);
            CHECK(status==INTEL_BCS_OK);
        }
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==(mode ? INTEL_BCS_JOB_CANCELLED : INTEL_BCS_JOB_COMPLETE));
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    }
    printf("Intel BCS staged admission: %u checks passed\n",checks-before);
}

static void bcs_queue_host_tests(void)
{
    unsigned prior_checks=checks;
    intel_bcs_job_result_t result;
    uint64_t fences[32], extra=0;
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_init(&queue,&queue_engine,&queue_io,NULL,&queue_ops,token)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_attach(&queue,43,&queue_backing[0],&extra)==INTEL_BCS_INVALID && !extra);
    CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_INVALID);
    intel_bcs_copy_t copy=queue_copy;
    for (unsigned i=0;i<32;i++) {
        CHECK(intel_bcs_queue_enqueue(&queue,41+i%2,queue_ids[i%2],&copy,&fences[i])==INTEL_BCS_OK);
        CHECK(fences[i]==token+i);
    }
    copy.width=0; /* job payloads must be captured, not caller-owned pointers */
    CHECK(queue.contexts[0].references==16 && queue.contexts[1].references==16 && !hw.mode);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_BUSY && !extra);
    CHECK(intel_bcs_queue_result(&queue,42,fences[0],false,&result)==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_cancel(&queue,42,fences[0])==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_close_context(&queue,42,queue_ids[0])==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_release_context(&queue,42,queue_ids[0])==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],true,&result)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
    CHECK(queue_engine.inflight && queue.contexts[0].published && prepares==1);
    CHECK(intel_bcs_queue_cancel(&queue,41,fences[0])==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_close_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
    CHECK(queue.contexts[0].references==1);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_INVALID);
    for (unsigned i=2;i<32;i+=2) {
        CHECK(intel_bcs_queue_result(&queue,41,fences[i],true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==INTEL_BCS_JOB_CANCELLED);
        CHECK(intel_bcs_queue_result(&queue,41,fences[i],false,&result)==INTEL_BCS_INVALID);
    }
    CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(extra==token+32); /* lower free slot must not jump ahead in FIFO */
    CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING); /* token visible; no matching CSB yet */
    CHECK(queue.contexts[0].references==1 && !queue_engine.completed_jobs);
    for (unsigned poll=0;poll<150 && queue_engine.completed_jobs<18;poll++) {
        enum intel_bcs_status r=intel_bcs_queue_step(&queue);
        CHECK(r==INTEL_BCS_OK || r==INTEL_BCS_PENDING);
    }
    CHECK(queue_engine.completed_jobs==18 && prepares==18 && !hw.resets);
    CHECK(prepared[0]==fences[0] && prepared[17]==extra);
    for (unsigned i=1;i<17;i++) CHECK(prepared[i]==fences[2*i-1]);
    CHECK(!queue.contexts[0].references && !queue.contexts[1].references);
    CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_COMPLETE && result.error==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK && hw.resets==2);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK && hw.resets==2);
    for (unsigned i=0;i<2;i++) {
        CHECK(intel_bcs_queue_release_context(&queue,41+i,queue_ids[i])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,41+i,queue_ids[i])==INTEL_BCS_INVALID);
    }
    CHECK(intel_bcs_queue_result(&queue,42,extra,true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_COMPLETE); /* result outlives the resource handle */
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_INVALID);

    /* Completed/cancelled results apply backpressure until explicitly read. */
    bcs_queue_host_start();
    for (unsigned i=0;i<32;i++) {
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fences[i])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_cancel(&queue,41,fences[i])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_cancel(&queue,41,fences[i])==INTEL_BCS_OK);
    }
    CHECK(!queue.contexts[0].references && !prepares);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_result(&queue,41,fences[5],true,&result)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(extra>fences[31] && intel_bcs_queue_cancel(&queue,41,fences[5])==INTEL_BCS_INVALID);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);

    /* Every submission observation/port write can fail, including a write
     * applied by the hardware. No retry, false completion or early release. */
    for (unsigned fail=1;fail<=7;fail++) {
        bcs_queue_host_start();
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
        hw.fail_at=hw.ops+fail;
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_IO);
        CHECK(queue.contexts[0].references==1 && queue.contexts[0].published);
        unsigned operations=hw.ops;
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_IO && hw.ops==operations);
        CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_close_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==INTEL_BCS_JOB_FAILED && result.error==INTEL_BCS_IO);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
    }
    /* Inject every I/O failure on the completion path as well. The token
     * arrives early in this model, so consuming it alone would fail here. */
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
    unsigned poll_start=hw.ops;
    enum intel_bcs_status status;
    do { status=intel_bcs_queue_step(&queue); } while (status==INTEL_BCS_PENDING);
    CHECK(status==INTEL_BCS_OK);
    unsigned poll_ops=hw.ops-poll_start;
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    for (unsigned fail=1;fail<=poll_ops;fail++) {
        bcs_queue_host_start();
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
        hw.fail_at=hw.ops+fail;
        do { status=intel_bcs_queue_step(&queue); } while (status==INTEL_BCS_PENDING);
        CHECK(status==INTEL_BCS_IO && queue.contexts[0].references==1);
        CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==INTEL_BCS_JOB_FAILED && result.error==INTEL_BCS_IO);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
    }
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
    hw.missing_token=true;
    unsigned attempts;
    for (attempts=0;attempts<60000;attempts++) {
        status=intel_bcs_queue_step(&queue);
        if (status!=INTEL_BCS_PENDING) break;
    }
    CHECK(attempts<60000 && status==INTEL_BCS_TIMEOUT && queue.contexts[0].references==1);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_FAILED && result.error==INTEL_BCS_TIMEOUT);

    /* A stuck shutdown may be retried, but keeps the active fence and every
     * bundle pinned even when all unsubmitted jobs have been cancelled. */
    bcs_queue_host_start();
    for (unsigned i=0;i<2;i++)
        CHECK(intel_bcs_queue_enqueue(&queue,41+i,queue_ids[i],&queue_copy,&fences[i])==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
    hw.stall_stop=true;
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_QUARANTINED && !queue.stopped);
    CHECK(queue.contexts[0].references==1 && !queue.contexts[1].references);
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],false,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_RUNNING);
    CHECK(intel_bcs_queue_result(&queue,42,fences[1],true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_CANCELLED);
    for (unsigned i=0;i<2;i++)
        CHECK(intel_bcs_queue_release_context(&queue,41+i,queue_ids[i])==INTEL_BCS_BUSY);
    hw.stall_stop=false;
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK && queue_engine.retired);
    CHECK(intel_bcs_queue_result(&queue,41,fences[0],true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_ABORTED);
    for (unsigned i=0;i<2;i++)
        CHECK(intel_bcs_queue_release_context(&queue,41+i,queue_ids[i])==INTEL_BCS_OK);

    bcs_queue_host_start();
    prepare_failure=true;
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_INVALID);
    CHECK(!queue.contexts[0].references && !queue.contexts[0].published && !hw.mode);
    CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_OK);
    CHECK(result.phase==INTEL_BCS_JOB_FAILED);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);

    /* Fence/context identity exhaustion is sticky, never wraps to a stale ID. */
    bcs_queue_host_start();
    for (unsigned i=2;i<INTEL_BCS_QUEUE_CONTEXTS;i++)
        CHECK(intel_bcs_queue_attach(&queue,41+i,&queue_backing[i],&queue_ids[i])==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_attach(&queue,99,&result,&extra)==INTEL_BCS_BUSY);
    queue.next_fence=UINT64_MAX-1;
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(extra==UINT64_MAX-1);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_cancel(&queue,41,extra)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_result(&queue,41,extra,true,&result)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_BUSY);
    queue.next_context=UINT64_MAX;
    CHECK(intel_bcs_queue_attach(&queue,99,&result,&extra)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    printf("Intel BCS queue: %u checks passed\n",checks-prior_checks);
    bcs_queue_race_test();
    bcs_queue_access_cases(); bcs_queue_access_races();
}

static void *bcs_queue_race_worker(void *argument)
{
    enum intel_bcs_status *status=argument;
    *status=race_stage==4 ? intel_bcs_queue_shutdown(&queue) : intel_bcs_queue_step(&queue);
    return NULL;
}

static void bcs_queue_race_test(void)
{
    unsigned before=checks;
    /* Stop a real worker at each callback/MMIO boundary. The calling thread
     * exercises exit/admission/result APIs while the worker is paused there.
     * If any metadata lock spans a callback, this test deadlocks and times out. */
    for (unsigned stage=1;stage<=4;stage++) for (unsigned shutdown=0;shutdown<2;shutdown++) {
        bcs_queue_host_start();
        uint64_t active, pending, other, extra=0;
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&active)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&pending)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&other)==INTEL_BCS_OK);
        if (stage>=3) CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
        CHECK(pthread_barrier_init(&race_enter,NULL,2)==0);
        CHECK(pthread_barrier_init(&race_release,NULL,2)==0);
        race_stage=stage;
        enum intel_bcs_status worker_status=INTEL_BCS_INVALID;
        pthread_t worker;
        CHECK(pthread_create(&worker,NULL,bcs_queue_race_worker,&worker_status)==0);
        (void)pthread_barrier_wait(&race_enter);
        unsigned operations=hw.ops;
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_cancel(&queue,41,active)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_result(&queue,41,active,false,&result)==INTEL_BCS_OK);
        CHECK(result.phase==(stage==1?INTEL_BCS_JOB_PREPARING:INTEL_BCS_JOB_RUNNING));
        CHECK(intel_bcs_queue_result(&queue,41,active,true,&result)==INTEL_BCS_BUSY);
        d3dkmt_release_process(41); /* actual production GDI/VM exit hook */
        d3dkmt_release_process(41); /* idempotent */
        CHECK(hw.ops==operations); /* exit did not touch MMIO or wait for worker */
        CHECK(intel_bcs_queue_result(&queue,41,pending,true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==INTEL_BCS_JOB_CANCELLED);
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&extra)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_queue_attach(&queue,41,&queue_backing[2],&extra)==INTEL_BCS_INVALID);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_BUSY);
        if (shutdown) CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_result(&queue,42,other,false,&result)==INTEL_BCS_OK);
        CHECK(result.phase==((stage==4 || shutdown)?INTEL_BCS_JOB_CANCELLED:INTEL_BCS_JOB_QUEUED));
        (void)pthread_barrier_wait(&race_release);
        CHECK(pthread_join(worker,NULL)==0);
        race_stage=0;
        CHECK(pthread_barrier_destroy(&race_enter)==0);
        CHECK(pthread_barrier_destroy(&race_release)==0);
        CHECK(worker_status==(stage==1 || stage==4?INTEL_BCS_OK:INTEL_BCS_PENDING));
        if (stage==1) {
            CHECK(!queue.contexts[0].published && !queue.contexts[0].references && !hw.mode);
            CHECK(intel_bcs_queue_result(&queue,41,active,true,&result)==INTEL_BCS_OK);
            CHECK(result.phase==INTEL_BCS_JOB_CANCELLED);
        } else if (stage!=4 && !shutdown) {
            enum intel_bcs_status status;
            do { status=intel_bcs_queue_step(&queue); } while (status==INTEL_BCS_PENDING);
            CHECK(status==INTEL_BCS_OK);
            CHECK(intel_bcs_queue_result(&queue,41,active,true,&result)==INTEL_BCS_OK);
            CHECK(result.phase==INTEL_BCS_JOB_COMPLETE);
        }
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_INVALID);
        for (unsigned i=0;i<2;i++)
            CHECK(intel_bcs_queue_release_context(&queue,41+i,queue_ids[i])==INTEL_BCS_OK);
        d3dkmt_release_process(41); /* no stale registry pointer after unpublish */
    }
    bcs_queue_host_start();
    CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
    uint64_t fences[32],extra;
    intel_bcs_job_result_t result;
    for (unsigned i=0;i<32;i++)
        CHECK(intel_bcs_queue_enqueue(&queue,41+i%2,queue_ids[i%2],&queue_copy,&fences[i])==INTEL_BCS_OK);
    d3dkmt_release_process(41);
    for (unsigned i=0;i<16;i++)
        CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&extra)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_enqueue(&queue,42,queue_ids[1],&queue_copy,&extra)==INTEL_BCS_BUSY);
    for (unsigned i=0;i<32;i++) {
        enum intel_bcs_status r=intel_bcs_queue_result(&queue,41+i%2,fences[i],false,&result);
        CHECK(r==(i%2?INTEL_BCS_OK:INTEL_BCS_INVALID));
        if (i%2) CHECK(result.phase==INTEL_BCS_JOB_QUEUED);
    }
    CHECK(!queue.contexts[0].references && queue.contexts[1].references==32);
    d3dkmt_release_process(42);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_OK && !hw.mode);
    for (unsigned i=0;i<32;i++) CHECK(queue.jobs[i].result.phase==INTEL_BCS_JOB_EMPTY);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
    CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    printf("Intel BCS concurrent exit: %u checks passed\n",checks-before);
}

static struct {
    uint64_t held[2];
    unsigned acquires,retires,busy,retire_busy;
    bool invalid,exit_in_acquire,retire_error;
} access_test;

static enum intel_bcs_status bcs_queue_model_acquire(void *ctx,uint64_t fence)
{
    unsigned i=*(unsigned *)ctx;
    CHECK(i<2 && !access_test.held[i] && queue.worker_busy && !queue_engine.inflight);
    access_test.held[i]=fence; access_test.acquires++;
    bcs_queue_race_point(5);
    if (access_test.exit_in_acquire) d3dkmt_release_process(41+i);
    return access_test.invalid ? INTEL_BCS_INVALID : access_test.busy ? INTEL_BCS_BUSY : INTEL_BCS_OK;
}
static enum intel_bcs_status bcs_queue_model_retire(void *ctx,uint64_t fence)
{
    unsigned i=*(unsigned *)ctx;
    CHECK(i<2 && access_test.held[i]==fence && queue.worker_busy);
    CHECK(!queue_engine.inflight || queue_engine.retired);
    if (hw.mode) CHECK(queue_engine.completed || queue_engine.retired);
    intel_bcs_job_result_t result;
    CHECK(intel_bcs_queue_result(&queue,41+i,fence,true,&result)==INTEL_BCS_BUSY);
    CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY);
    bcs_queue_race_point(6);
    access_test.retires++;
    if (access_test.retire_busy) { access_test.retire_busy--; return INTEL_BCS_BUSY; }
    if (access_test.retire_error) return INTEL_BCS_QUARANTINED;
    access_test.held[i]=0;
    return INTEL_BCS_OK;
}

static void bcs_queue_access_cases(void)
{
    unsigned before=checks;
    for (unsigned scenario=0;scenario<8;scenario++) {
        bcs_queue_host_start(); memset(&access_test,0,sizeof(access_test));
        queue.ops.acquire=bcs_queue_model_acquire; queue.ops.retire=bcs_queue_model_retire;
        access_test.busy=scenario==1; access_test.invalid=scenario==2;
        prepare_failure=scenario==3; access_test.exit_in_acquire=scenario==6;
        access_test.retire_busy=scenario==7 ? 0 : 2;
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        uint64_t fence=0;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        if (scenario==5) hw.fail_at=hw.ops+4;
        enum intel_bcs_status r=intel_bcs_queue_step(&queue);
        CHECK(access_test.held[0]==fence && queue.contexts[0].references==1);
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_BUSY);
        if (scenario==1 || scenario==2 || scenario==3 || scenario==6) {
            CHECK(r==INTEL_BCS_BUSY && !hw.mode && access_test.retires==1);
            CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY && access_test.held[0]);
            r=intel_bcs_queue_step(&queue);
            if (scenario==1) {
                CHECK(r==INTEL_BCS_BUSY && !access_test.held[0] && queue.contexts[0].references==1);
                CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_BCS_OK);
                CHECK(result.phase==INTEL_BCS_JOB_QUEUED && !prepares);
                access_test.busy=0;
                CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING && access_test.acquires==2);
                do { r=intel_bcs_queue_step(&queue); } while (r==INTEL_BCS_PENDING);
                CHECK(r==INTEL_BCS_OK && !access_test.held[0] && prepares==1);
            } else CHECK(r==(scenario==6 ? INTEL_BCS_OK : INTEL_BCS_INVALID));
        } else if (scenario==4 || scenario==5) {
            CHECK(r==(scenario==5 ? INTEL_BCS_IO : INTEL_BCS_PENDING) && !access_test.retires);
            hw.stall_stop=true;
            CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_QUARANTINED);
            CHECK(access_test.held[0] && !access_test.retires && !queue.stopped);
            hw.stall_stop=false;
            CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY && queue_engine.retired);
            CHECK(!queue.stopped && queue.contexts[0].references==1 && access_test.held[0]);
            CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY);
            CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK && queue.stopped);
        } else {
            CHECK(r==INTEL_BCS_PENDING && !access_test.retires);
            access_test.retire_error=scenario==7;
            do { r=intel_bcs_queue_step(&queue); } while (r==INTEL_BCS_PENDING);
            CHECK(queue_engine.completed && access_test.held[0]);
            unsigned operations=hw.ops;
            if (scenario==7) {
                CHECK(r==INTEL_BCS_QUARANTINED);
                access_test.retire_error=false;
                CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
            } else {
                CHECK(r==INTEL_BCS_BUSY && access_test.retires==1);
                CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY);
                CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_OK);
                CHECK(hw.ops==operations); /* no repeated completion or MMIO */
            }
        }
        CHECK(!access_test.held[0] && !queue.contexts[0].references);
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK);
        CHECK(result.phase==(scenario==2 || scenario==3 || scenario==5 ? INTEL_BCS_JOB_FAILED :
            scenario==4 ? INTEL_BCS_JOB_ABORTED : scenario==6 ? INTEL_BCS_JOB_CANCELLED : INTEL_BCS_JOB_COMPLETE));
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    }
    printf("Intel BCS job access: %u checks passed\n",checks-before);
}

static void bcs_queue_access_races(void)
{
    unsigned before=checks;
    for (unsigned stage=5;stage<=6;stage++) for (unsigned round=0;round<16;round++) {
        bcs_queue_host_start(); memset(&access_test,0,sizeof(access_test));
        queue.ops.acquire=bcs_queue_model_acquire; queue.ops.retire=bcs_queue_model_retire;
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        uint64_t fence=0;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        if (stage==6) { CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING); hw.delay=1; }
        CHECK(!pthread_barrier_init(&race_enter,NULL,2));
        CHECK(!pthread_barrier_init(&race_release,NULL,2)); race_stage=stage;
        enum intel_bcs_status result=INTEL_BCS_INVALID;
        pthread_t worker; CHECK(!pthread_create(&worker,NULL,bcs_queue_race_worker,&result));
        (void)pthread_barrier_wait(&race_enter);
        CHECK(access_test.held[0]==fence && queue.contexts[0].references==1);
        d3dkmt_release_process(41);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY && !queue.stopped);
        intel_bcs_job_result_t pending;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&pending)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
        (void)pthread_barrier_wait(&race_release);
        CHECK(!pthread_join(worker,NULL)); race_stage=0;
        CHECK(result==INTEL_BCS_OK && !access_test.held[0] && !queue.contexts[0].references);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
        CHECK(!pthread_barrier_destroy(&race_enter)); CHECK(!pthread_barrier_destroy(&race_release));
    }
    printf("Intel BCS job access races: %u checks passed, 32 forced exits\n",checks-before);
}
