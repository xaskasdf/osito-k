/* Persistent native render contexts; only MMIO/DMA and NT process state are
 * modeled. Pixel execution is qualified separately on physical Intel GT. */
#define INTEL_OWNER_SUITE_ENTRY owner_suite_main
#include "intel_gpu_bcs_owner_host.c"

typedef struct {
    OWNER_FIXTURE base;
    uint8_t bitmap[8];
    uint32_t cache[3];
    unsigned cache_fail,tlb_fail,invalidations;
} RENDER_OWNER_FIXTURE;
static bool render_owner_read(void *opaque,uint32_t reg,uint32_t *value)
{
    RENDER_OWNER_FIXTURE *f=opaque;
    const uint32_t cache[]={0xc800,0xc804,0xb020};
    for (unsigned i=0;i<3;i++) if (reg==cache[i]) { *value=f->cache[i]; return true; }
    if (reg==0x40e0) return owner_read(&f->base,reg,value);
    if (reg==0x4260) { *value=0; return !f->tlb_fail; }
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    bool result=owner_read(&f->base,reg==0x941c ? reg : reg+0x20000,value);
    if (reg==0x941c && *value==8) *value=2;
    return result;
}
static bool render_owner_write(void *opaque,uint32_t reg,uint32_t value)
{
    RENDER_OWNER_FIXTURE *f=opaque;
    const uint32_t cache[]={0xc800,0xc804,0xb020};
    for (unsigned i=0;i<3;i++) if (reg==cache[i]) {
        f->cache[i]=value; return f->cache_fail!=i+1;
    }
    if (reg==0x40e0) return owner_write(&f->base,reg,value);
    if (reg==0x4260) { CHECK(value==1); f->invalidations++; return !f->tlb_fail; }
    CHECK(reg==0x941c || (reg>=0x2000 && reg<0x2400));
    if (reg==0x941c) { CHECK(value==2); value=8; }
    return owner_write(&f->base,reg==0x941c ? reg : reg+0x20000,value);
}
static bool render_owner_token(void *opaque,uint64_t gpu,uint64_t *value)
{
    _Static_assert(INTEL_RCS_TOKEN_OFFSET==INTEL_BCS_TOKEN_OFFSET,"shared completion address");
    return owner_token(&((RENDER_OWNER_FIXTURE *)opaque)->base,gpu,value);
}

static void render_owner_cases(void)
{
    for (unsigned mode=0;mode<8;mode++) {
        RENDER_OWNER_FIXTURE f={0}; OWNER_FIXTURE *b=&f.base;
        jobs_setup(&b->jobs); bcs_model_reset(); b->pat=0x03112233;
        f.cache[0]=0x11; f.cache[1]=0x22; f.cache[2]=0x33445566;
        b->jobs.jobs.ready=intel_gpu_executor_notify;
        CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        CHECK(d3dkmt_destroy_context(7,b->jobs.base.context)==0); service_step();
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=mode==4 ? INTEL_UMD_NODE_COPY : INTEL_UMD_NODE_RENDER,
            .private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        D3DKMT_CONTEXT_BUFFERS buffers;
        CHECK(d3dkmt_create_context(7,n->device,&desc,&b->jobs.base.context,&buffers)==0);
        capture_fill(&b->jobs.base);
        intel_umd_copy32_t *command=(void *)((BYTE *)n->contexts.contexts[0].cpu.cpu+128);
        if (mode!=4) {
            intel_gpu_capture_t invalid={0};
            CHECK(intel_gpu_capture_begin(7,b->jobs.base.context,128,64,2,2,&invalid)==STATUS_INVALID_PARAMETER);
            CHECK(intel_gpu_capture_release(&invalid)==0); /* full render pitch exceeds 1000 bytes */
        }
        command->height=7;
        const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
        const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
        CHECK(intel_ggtt_space_init(&b->space,&layout,1,63,f.bitmap,sizeof(f.bitmap),&ggtt_io,ggtt)==INTEL_GGTT_BIND_OK);
        intel_gpu_bcs_config_t config={.space=&b->space,.allocator=allocator,.allocator_context=&ram,
            .io={render_owner_read,render_owner_write,owner_now,owner_relax},.hardware_context=&f,
            .token_read=render_owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
            .source_gpu=0x10000000,.destination_gpu=0x20000000,.batch_gpu=0x30000000,.render=true,
            .first_fence=0x11223344,.first_hardware_id=0x1001};
        CHECK(intel_gpu_bcs_owner_init(&b->owner,&b->jobs.jobs,&config)==0);
        if (mode==7) ram.fail_allocate=ram.allocated+4;
        NTSTATUS reserved=intel_gpu_bcs_owner_reserve(&b->owner,2,8);
        if (mode==7) {
            CHECK(reserved!=0 && b->owner.count==2 && b->owner.slots[0].native.batch.live);
            CHECK(ram.allocated==ram.fail_allocate && !ram.cpu[ram.allocated-1]); ram.allocated--;
        } else {
            CHECK(reserved==0);
            if (mode==1) f.cache_fail=2;
            NTSTATUS started=intel_gpu_bcs_owner_start(&b->owner);
            if (mode==1) CHECK(started!=0 && !b->owner.engine.started && b->owner.hwsp.live);
            else {
                CHECK(started==0 && b->owner.engine.engine==INTEL_CS_RENDER && b->owner.cache.ready);
                intel_bcs_context_t *native=&b->owner.slots[0].native;
                void *state=native->state.cpu,*tables=native->tables.cpu,*batch=native->batch.cpu;
                CHECK(native->state.bytes==23*4096 && native->batch.bytes==8192);
                uint64_t saved_ggtt[64]; memcpy(saved_ggtt,ggtt,sizeof(ggtt));
                unsigned resets=hw.resets;
                for (unsigned round=0;round<(mode==0 ? 70u : mode==5 ? 2u : 1u);round++) {
                    b->jobs.done_count=0;
                    if (round && round%10==0) {
                        OKMT_HANDLE old[2]={b->jobs.base.allocations[0],b->jobs.base.allocations[1]};
                        D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
                        CHECK(d3dkmt_create_allocations(7,n->device,2,sizes,b->jobs.base.allocations)==0);
                        capture_fill(&b->jobs.base); command->height=7;
                        CHECK(d3dkmt_destroy_allocations(7,n->device,2,old)==0);
                    }
                    if (round==35 || (mode==5 && round==1)) {
                        CHECK(d3dkmt_destroy_context(7,b->jobs.base.context)==0); service_step();
                        CHECK(d3dkmt_create_context(7,n->device,&desc,&b->jobs.base.context,&buffers)==0);
                        capture_fill(&b->jobs.base); command=(void *)((BYTE *)n->contexts.contexts[0].cpu.cpu+128); command->height=7;
                        if (mode==5) f.tlb_fail=1;
                    }
                    if (mode==2 || mode==3) hw.delay=1000;
                    if (mode==0 && round%2) {
                        command->version=INTEL_UMD_COPY_RECT_VERSION;command->width=5;command->height=3;
                        command->reserved[0]=3;command->reserved[1]=2;
                        command->reserved[2]=7;command->reserved[3]=4;
                    } else if (mode==0) {
                        command->version=INTEL_UMD_COMMAND_VERSION;command->width=16;command->height=7;
                        memset(command->reserved,0,sizeof(command->reserved));
                    }
                    unsigned allocated=ram.allocated;
                    uint64_t id=0;
                    CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,64,2,2,&id)==0);
                    if (mode==6) ram.fail_sync=ram.syncs+1;
                    for (unsigned i=0;i<8 && !b->jobs.done_count;i++) { executor_step(); service_step(); }
                    CHECK(ram.allocated==allocated && !memcmp(saved_ggtt,ggtt,sizeof(ggtt)));
                    if (mode==2 || mode==3) {
                        CHECK(!b->jobs.done_count && b->owner.engine.inflight && native->access_token);
                        if (mode==3) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
                        hw.stall_stop=true;
                        CHECK(intel_gpu_bcs_owner_close(&b->owner)==D3DKMT_STATUS_DEVICE_BUSY);
                        executor_step(); service_step();
                        CHECK(!b->owner.engine.retired && !b->owner.closed && native->batch.live && native->state.live);
                        CHECK(native->source_pinned && native->destination_pinned && b->running);
                    } else if (mode==4 || mode==6 || (mode==5 && round==1))
                        CHECK(b->jobs.done_count==1 && b->jobs.done_status[0]!=0);
                    else {
                        CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);
                        CHECK(native->state.cpu==state && native->tables.cpu==tables && native->batch.cpu==batch);
                        CHECK(native->tail==(192+round*128)%4096 && hw.resets==resets && b->starts==1);
                        uint32_t *saved=state,*ring=saved+INTEL_RCS_CONTEXT_BYTES/4;
                        if (!round) {
                            CHECK(ring[16]==0x18800101 && ring[17]==config.batch_gpu && !ring[10] && !ring[11]);
                            CHECK(ring[30]==(uint32_t)(config.first_fence)); saved[2100]=0x123abc;
                        } else CHECK(saved[2100]==0x123abc && saved[1027]==0xf0008);
                        CHECK(((uint32_t *)batch)[0x1100/4]==0x0080005a);
                        if (mode==0 && round%2) {
                            uint32_t *draw=batch;
                            CHECK(draw[0x1180/4]==(12u|(7u<<16)) && draw[0x1180/4+6]==(7u|(4u<<16)));
                            CHECK(draw[0x1080/4+2]==(7u|(4u<<16)) && draw[0x1040/4+2]==(11u|(6u<<16)));
                        }
                    }
                }
                /* Six surface replacements, one close/unmap and one retarget. */
                if (mode==0) CHECK(b->owner.engine.completed_jobs==70 && native->rebindings==8 && f.invalidations==8);
            }
        }
        f.cache_fail=f.tlb_fail=0;
        owner_cleanup(b);
        CHECK(f.cache[0]==0x11 && f.cache[1]==0x22 && f.cache[2]==0x33445566);
        CHECK(ram.allocated==ram.freed);
    }
}
#ifndef INTEL_RCS_OWNER_SUITE_ENTRY
#define INTEL_RCS_OWNER_SUITE_ENTRY main
#endif
int INTEL_RCS_OWNER_SUITE_ENTRY(void)
{
    if (owner_suite_main()) return 1;
    unsigned before=checks;
    render_owner_cases();
    printf("Intel persistent RCS owner: %u checks passed, 70 draws, wrap, reuse, cancellation, retirement and cache faults\n",checks-before);
    return 0;
}
