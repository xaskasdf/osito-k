#define main resource_baseline_main
#include "intel_gpu_resource_host.c"
#undef main
#include "native_umd_shader_reference.h"
static unsigned tlb_writes,fail_tlb;
static uint64_t now_ns;
static bool context_read(void *ctx,uint32_t reg,uint32_t *value)
{
    (void)ctx;
    CHECK(reg==0x2234 || reg==0x20b8 || reg==0x4260);
    *value=reg==0x2234 ? 1 : 0;
    return !(fail_tlb && reg==0x4260);
}
static bool context_write(void *ctx,uint32_t reg,uint32_t value)
{ (void)ctx; CHECK(reg==0x4260 && value==1); tlb_writes++; return true; }
static uint64_t context_now(void *ctx) { (void)ctx; return now_ns+=10000; }
static void context_relax(void *ctx) { (void)ctx; }
static void context_cases(void)
{
    for (unsigned mode=0;mode<9;mode++) {
        res_reset(); tlb_writes=fail_tlb=0; now_ns=0;
        intel_bcs_context_t c={0}; intel_gpu_buffer_t b[7]={0};
        intel_ggtt_space_t space={0}; uint8_t bitmap[8]={0};
        const intel_ggtt_io_t io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
        const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
        CHECK(intel_ggtt_space_init(&space,&layout,1,63,bitmap,sizeof(bitmap),&io,ggtt)==INTEL_GGTT_BIND_OK);
        for (unsigned i=0;i<7;i++) CHECK(intel_gpu_buffer_create(&b[i],&allocator,&ram,i<4 ? 7 : 9,8192)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_reserve_draw(&c,&space,&allocator,&ram,1,0x1001,
            0x10000000,0x20000000,16,0x10000000,0x30000000,0x40000000));
        CHECK(intel_bcs_context_activate_draw(&c,7,&b[0],&b[1],&b[0])==INTEL_BCS_INVALID);
        CHECK(intel_bcs_context_activate_draw(&c,7,&b[0],&b[1],&b[6])==INTEL_BCS_INVALID);
        CHECK(!b[0].residents && !b[1].residents && !c.owner);
        CHECK(intel_bcs_context_activate_draw(&c,7,&b[0],&b[1],&b[2])==INTEL_BCS_OK);
        CHECK(b[0].residents==1 && b[1].residents==1 && b[2].residents==1);
        /* Independently walk all four mapping roots and check final permissions. */
        const uint64_t addresses[]={c.source_gpu,c.destination_gpu,c.batch_gpu,c.program_gpu};
        const uint64_t physical[]={b[0].dma,b[1].dma,c.batch.dma,b[2].dma};
        for (unsigned i=0;i<4;i++) {
            uint64_t *table=c.tables.cpu,pte=0;
            for (unsigned shift=39;;shift-=9) {
                pte=table[(addresses[i]>>shift)&511]; CHECK(pte&1);
                if (shift==12) break;
                uint64_t page=(pte&0x000ffffffffff000ULL)-c.tables.dma;
                CHECK(page<c.tables.bytes); table=(void *)((unsigned char *)c.tables.cpu+page);
            }
            CHECK((pte&0x000ffffffffff000ULL)==physical[i]); CHECK(!!(pte&2)==(i==1));
        }
        intel_cs_state_t engine={.engine=INTEL_CS_RENDER,.enabled=true,.started=true,.status=INTEL_CS_OK};
        intel_bcs_queue_t q={.initialized=true,.next_context=1,.next_fence=1,.active=INTEL_BCS_QUEUE_JOBS,
            .engine=&engine,.io={context_read,context_write,context_now,context_relax}};
        CHECK(intel_bcs_context_attach(&c,&q,NULL));
        intel_rcs_queue_draw_t work={0}; memcpy(&work.program,reference_header,sizeof(work.program));
        work.program.code_offset=4096; work.program.allocation_bytes=4096+sizeof(reference_native);
        work.primitive=(intel_rcs_primitive_draw_t){
            {c.batch_gpu,c.program_gpu+4096,c.destination_gpu,b[1].bytes,16,16,64,0,0,16,16,3,{0}},
            c.source_gpu,b[0].bytes,0,16,3,INTEL_RCS_TRIANGLE_LIST,{0}};
        uint64_t descriptor=0; uint32_t tail=0;
        CHECK(!intel_bcs_context_prepare_draw(&c,&work,1,&descriptor,&tail));
        intel_gpu_cpu_access_t cpu={0};
        CHECK(intel_gpu_buffer_acquire_cpu(&b[2],7,&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_acquire(&c,1)==INTEL_BCS_BUSY);
        CHECK(c.source_access.self && c.destination_access.self && !c.program_access.self);
        CHECK(intel_bcs_context_retire(&c,1)==INTEL_BCS_OK);
        CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_bcs_context_acquire(&c,2)==INTEL_BCS_OK);
        CHECK(intel_gpu_buffer_acquire_cpu(&b[2],7,&cpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_bcs_context_rebind_draw(&c,&b[0],&b[1],&b[3])==INTEL_BCS_INVALID);
        if (mode==1) {
            work.primitive.pixel.program_gpu+=4096;
            CHECK(!intel_bcs_context_prepare_draw(&c,&work,2,&descriptor,&tail) && !descriptor && !tail);
            work.primitive.pixel.program_gpu-=4096;
        }
        CHECK(intel_bcs_context_prepare_draw(&c,&work,2,&descriptor,&tail));
        CHECK(descriptor && tail==192);
        if (mode==2) {
            b[2].busy=1; CHECK(intel_bcs_context_retire(&c,2)==INTEL_BCS_BUSY);
            CHECK(c.program_access.self && c.access_token==2); b[2].busy=0;
        }
        CHECK(intel_bcs_context_retire(&c,2)==INTEL_BCS_OK);
        if (mode==3) {
            b[3].busy=1;
            CHECK(intel_bcs_context_rebind_draw(&c,&b[0],&b[1],&b[3])==INTEL_BCS_BUSY);
            CHECK(c.program==&b[2] && b[2].residents==1 && !b[3].residents); b[3].busy=0;
        }
        if (mode==4) fail_tlb=1;
        if (mode==5) ram.fail_sync=ram.syncs+1;
        enum intel_bcs_status changed=intel_bcs_context_rebind_draw(&c,&b[0],&b[1],&b[3]);
        if (mode==4 || mode==5) {
            CHECK(changed==INTEL_BCS_QUARANTINED && !c.ready);
            CHECK(b[2].residents && b[3].residents && c.retained_program==&b[3]);
            fail_tlb=ram.fail_sync=0;
        } else {
            CHECK(changed==INTEL_BCS_OK && c.program==&b[3] && !b[2].residents && b[3].residents==1 && tlb_writes==1);
            if (mode==6) {
                CHECK(intel_bcs_context_rebind(&c,&b[0],&b[1])==INTEL_BCS_OK);
                CHECK(!c.program && !b[3].residents);
                CHECK(intel_bcs_context_rebind_draw(&c,&b[0],&b[1],&b[2])==INTEL_BCS_OK);
            }
            if (mode>=7) {
                uint64_t old=c.queue_id;
                CHECK(intel_bcs_queue_close_context(&q,7,old)==INTEL_BCS_OK);
                if (mode==8) fail_tlb=1;
                changed=intel_bcs_context_retarget_draw(&c,9,99,&b[4],&b[5],&b[6]);
                if (mode==8) {
                    CHECK(changed==INTEL_BCS_QUARANTINED && c.retained_owner==9 && c.owner==7);
                    CHECK(b[3].residents && b[6].residents); fail_tlb=0;
                } else CHECK(changed==INTEL_BCS_OK && c.queue_id!=old && c.owner==9 &&
                    c.epoch==99 && !b[3].residents && b[6].residents);
            }
        }
        CHECK(!intel_bcs_context_destroy(&c));
        CHECK(intel_bcs_queue_close_context(&q,c.queue_owner,c.queue_id)==INTEL_BCS_OK);
        /* Queue/engine reset ordering is covered by the executable queue suite. */
        q.stopped=true; engine.retired=true; engine.inflight=false;
        CHECK(intel_bcs_context_destroy(&c));
        for (unsigned i=0;i<7;i++) CHECK(intel_gpu_buffer_close(&b[i],i<4 ? 7 : 9)==INTEL_GPU_BUFFER_OK);
        CHECK(ram.allocated==ram.freed);
    }
}
int main(void)
{
    context_cases();
    printf("Intel RCS three-resource context: %u checks passed\n",checks);
    return 0;
}
