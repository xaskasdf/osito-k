/* Existing strict engine model; exercise the production draw queue, not a
 * shader interpreter. Native pixel execution is checked on the P1. */
#define main bcs_baseline_main
#include "intel_bcs_host.c"
#undef main
static intel_rcs_queue_draw_t expected_draw;
static unsigned draw_calls;
static bool draw_read(void *ctx,uint32_t reg,uint32_t *value)
{
    (void)ctx;
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    bool result=bcs_model_read(&hw,reg==0x941c ? reg : reg+0x20000,value);
    if (reg==0x941c && *value==8) *value=2;
    if (reg==0x2080 && *value==0x1003000) *value=0x1020000;
    return result;
}
static bool draw_write(void *ctx,uint32_t reg,uint32_t value)
{
    (void)ctx;
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    if (reg==0x941c) { CHECK(value==2); value=8; }
    if (reg==0x2080 && value) { CHECK(value==0x1020000); value=0x1003000; }
    return bcs_model_write(&hw,reg==0x941c ? reg : reg+0x20000,value);
}
static bool draw_prepare(void *ctx,const intel_rcs_queue_draw_t *draw,uint64_t fence,
    uint64_t *descriptor,uint32_t *tail)
{
    CHECK(access_test.held[0]==fence);
    CHECK(!memcmp(draw,&expected_draw,sizeof(*draw))); draw_calls++;
    return bcs_queue_model_prepare(ctx,&queue_copy,fence,descriptor,tail);
}
static void draw_cases(void)
{
    const intel_gt_io_t io={draw_read,draw_write,bcs_queue_model_now,bcs_queue_model_relax};
    const intel_bcs_queue_ops_t ops={bcs_queue_model_prepare,bcs_queue_model_token,
        bcs_queue_model_acquire,bcs_queue_model_retire};
    for (unsigned mode=0;mode<10;mode++) {
        bcs_model_reset(); memset(&queue,0,sizeof(queue)); memset(&queue_engine,0,sizeof(queue_engine));
        memset(&access_test,0,sizeof(access_test)); prepares=draw_calls=prepare_failure=0;
        memset(&expected_draw,0x34,sizeof(expected_draw)); queue_backing[0]=0;
        intel_bcs_queue_ops_t current_ops=ops;
        if (mode>=8) current_ops.prepare=NULL;
        CHECK(intel_cs_enable(INTEL_CS_RENDER,&io,&hw,0x1020000,&queue_engine)==INTEL_CS_OK);
        CHECK(intel_bcs_queue_init_draw(&queue,&queue_engine,&io,&hw,&ops,NULL,token)==INTEL_CS_INVALID);
        CHECK(intel_bcs_queue_init_draw(&queue,&queue_engine,&io,&hw,&current_ops,draw_prepare,token)==INTEL_CS_OK);
        CHECK(intel_bcs_queue_attach(&queue,41,&queue_backing[0],&queue_ids[0])==INTEL_CS_OK);
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_CS_OK);
        if (mode>=8) {
            uint64_t unchanged=0xdead;
            CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&unchanged)==INTEL_CS_INVALID);
            CHECK(unchanged==0xdead);
        }
        intel_bcs_job_result_t result; uint64_t fence=0;
        unsigned rounds=mode ? 1 : 70;
        for (unsigned round=0;round<rounds;round++) {
            intel_rcs_queue_draw_t original=expected_draw;
            unsigned admission=1;
            intel_bcs_queue_gate_t gate={&admission,1,2,3};
            if (mode>=8) {
                CHECK(intel_bcs_queue_stage_draw(&queue,41,queue_ids[0],&original,&gate,&fence)==INTEL_CS_OK);
                CHECK(intel_bcs_queue_step(&queue)==INTEL_CS_OK && !draw_calls);
                CHECK(intel_bcs_queue_finish_stage(&queue,41,fence,&admission)==INTEL_CS_BUSY);
                admission=mode==8 ? 3 : 2;
                CHECK(intel_bcs_queue_finish_stage(&queue,41,fence,&admission)==INTEL_CS_OK);
            } else CHECK(intel_bcs_queue_enqueue_draw(&queue,41,queue_ids[0],&original,&fence)==INTEL_CS_OK);
            memset(&original,0xa5,sizeof(original)); /* complete input snapshot */
            if (mode==1) CHECK(intel_bcs_queue_cancel(&queue,41,fence)==INTEL_CS_OK);
            if (mode==2) access_test.exit_in_acquire=true;
            if (mode==3) {
                access_test.busy=1;
                CHECK(intel_bcs_queue_step(&queue)==INTEL_CS_BUSY);
                CHECK(!draw_calls && access_test.acquires==1 && access_test.retires==1 && !access_test.held[0]);
                access_test.busy=0;
            }
            if (mode==4) access_test.retire_busy=2;
            if (mode==5) prepare_failure=1;
            if (mode==7) {
                d3dkmt_release_process(41);
                CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_CS_OK);
                CHECK(result.phase==INTEL_BCS_JOB_CANCELLED && !draw_calls); break;
            }
            if (mode==6) {
                hw.delay=1000;
                CHECK(intel_bcs_queue_step(&queue)==INTEL_CS_PENDING);
                CHECK(access_test.held[0]==fence && !access_test.retires);
                hw.stall_stop=true;
                CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_CS_QUARANTINED);
                CHECK(access_test.held[0]==fence && !access_test.retires && !queue_engine.retired);
                hw.stall_stop=false;
                CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_CS_OK);
                CHECK(!access_test.held[0] && access_test.retires==1); break;
            }
            for (unsigned attempt=0;attempt<20;attempt++) {
                CHECK(intel_bcs_queue_result(&queue,41,fence,false,&result)==INTEL_CS_OK);
                if (result.phase>=INTEL_BCS_JOB_COMPLETE) break;
                enum intel_cs_status r=intel_bcs_queue_step(&queue);
                CHECK(r==INTEL_CS_OK || r==INTEL_CS_BUSY || r==INTEL_CS_PENDING || (mode==5 && r==INTEL_CS_INVALID));
                /* Exit may reap a terminal result on the next step, so stop now. */
            }
            CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_CS_OK);
            CHECK(result.phase==((mode==1 || mode==2 || mode==8) ? INTEL_BCS_JOB_CANCELLED :
                mode==5 ? INTEL_BCS_JOB_FAILED : INTEL_BCS_JOB_COMPLETE));
            CHECK(!access_test.held[0]);
            if (!mode) CHECK(draw_calls==round+1 && access_test.acquires==round+1 && access_test.retires==round+1);
        }
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_CS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_CS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_CS_OK);
    }
    bcs_queue_host_start();
    uint64_t unchanged=0xdead;
    CHECK(intel_bcs_queue_enqueue_draw(&queue,41,queue_ids[0],&expected_draw,&unchanged)==INTEL_CS_INVALID);
    CHECK(unchanged==0xdead);
    CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_CS_OK);
}
int main(void)
{
    draw_cases();
    printf("Intel RCS queued primitives: %u checks passed\n",checks);
    return 0;
}
