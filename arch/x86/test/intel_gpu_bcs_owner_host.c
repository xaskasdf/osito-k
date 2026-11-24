#define NATIVE_EXECUTOR_SUITE_ENTRY executor_suite_main
#include "intel_gpu_executor_host.c"
#include "../drivers/intel_gpu_bcs_owner.h"

typedef struct {
    JOBS_FIXTURE jobs;
    intel_gpu_bcs_owner_t owner;
    intel_gpu_capture_t bootstrap;
    intel_ggtt_space_t space;
    uint8_t bitmap[4];
    uint32_t pat,pat4,hwsp;
    unsigned running,starts,stops,pat_fail,token_fail;
} OWNER_FIXTURE;
static bool owner_read(void *opaque,uint32_t reg,uint32_t *value)
{
    OWNER_FIXTURE *f=opaque;
    if (reg==0x40e0) { *value=f->pat; return !f->pat_fail; }
    if (reg==0x40e4) { *value=f->pat4; return !f->pat_fail; }
    if (reg==0x426c) { *value=0; return true; }
    bool result=bcs_model_read(&hw,reg,value);
    if (reg==0x22080) *value=f->hwsp;
    if (hw.emitted) for (unsigned i=0;i<f->owner.count;i++) {
        intel_bcs_context_t *n=&f->owner.slots[i].native;
        if (n->hardware_id!=hw.elsp[2] || !n->state.cpu) continue;
        uint32_t *saved=(uint32_t *)n->state.cpu+1024;
        saved[5]=hw.tail; saved[7]=hw.tail;
    }
    return result;
}
static bool owner_write(void *opaque,uint32_t reg,uint32_t value)
{
    OWNER_FIXTURE *f=opaque;
    if (reg==0x40e0) { f->pat=value; return !f->pat_fail; }
    if (reg==0x40e4) { f->pat4=value; return !f->pat_fail; }
    if (reg==0x426c) { CHECK(value==1); return true; }
    if (reg==0x22080) {
        CHECK(value==0 || value==f->owner.hwsp_binding.gpu_address);
        f->hwsp=value; return bcs_model_write(&hw,reg,value ? 0x1003000 : 0);
    }
    if (reg==0x22230) {
        CHECK(f->running && f->owner.queue.active<INTEL_BCS_QUEUE_JOBS);
        intel_bcs_queue_job_t *j=&f->owner.queue.jobs[f->owner.queue.active];
        intel_bcs_context_t *n=NULL;
        for (unsigned i=0;i<f->owner.count;i++)
            if (f->owner.slots[i].native.queue_id==j->context) n=&f->owner.slots[i].native;
        CHECK(n); hw.expected_descriptor=n->descriptor; hw.next_token=j->fence; hw.tail=n->tail;
    }
    return bcs_model_write(&hw,reg,value);
}
static uint64_t owner_now(void *opaque) { (void)opaque; return bcs_model_now(&hw); }
static void owner_relax(void *opaque) { (void)opaque; bcs_model_relax(&hw); }
static bool owner_token(void *opaque,uint64_t gpu,uint64_t *value)
{
    OWNER_FIXTURE *f=opaque;
    bool found=false;
    for (unsigned i=0;i<f->owner.count;i++)
        if (gpu==f->owner.slots[i].native.binding.gpu_address+INTEL_BCS_TOKEN_OFFSET) found=true;
    CHECK(found);
    return !f->token_fail && bcs_model_token(&hw,value);
}
static void owner_mapping(void *opaque,bool stopped)
{
    OWNER_FIXTURE *f=opaque;
    if (stopped) { CHECK(!f->owner.engine.referenced || f->owner.engine.retired); f->stops++; }
    else { CHECK(!f->running); f->starts++; }
    f->running=!stopped;
}
static void owner_setup(OWNER_FIXTURE *f)
{
    memset(f,0,sizeof(*f)); jobs_setup(&f->jobs); bcs_model_reset();
    f->jobs.jobs.ready=intel_gpu_executor_notify; f->pat=0x03112233;f->pat4=0x03221155;
    const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&f->space,&layout,16,32,f->bitmap,sizeof(f->bitmap),&ggtt_io,ggtt)==INTEL_GGTT_BIND_OK);
    CHECK(intel_gpu_capture_begin(7,f->jobs.base.context,128,64,2,2,&f->bootstrap)==0);
    intel_gpu_bcs_config_t config={.space=&f->space,.allocator=allocator,.allocator_context=&ram,
        .io={owner_read,owner_write,owner_now,owner_relax},.hardware_context=f,
        .token_read=owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
        .source_gpu=0x10000000,.destination_gpu=0x20000000,.first_fence=0x11223344,.first_hardware_id=0x1001};
    service_cpu=1; CHECK(intel_gpu_bcs_owner_init(&f->owner,&f->jobs.jobs,&config)==STATUS_NOT_SUPPORTED); service_cpu=0;
    CHECK(intel_gpu_bcs_owner_init(&f->owner,&f->jobs.jobs,&config)==0);
}
static void owner_cleanup(OWNER_FIXTURE *f)
{
    uint32_t pat4_original=f->owner.coherent_pat.started ? f->owner.coherent_pat.original : f->pat4;
    hw.stall_stop=false; f->pat_fail=0; ram.fail_sync=ram.fail_release=0; ggtt_fail=false;
    NTSTATUS closed=STATUS_UNSUCCESSFUL;
    for (unsigned i=0;i<16;i++) {
        closed=intel_gpu_bcs_owner_close(&f->owner);
        if (!closed) break;
        executor_step(); service_step();
    }
    CHECK(closed==0 && f->owner.closed && !f->owner.executor && !f->running && f->pat==0x03112233);
    CHECK(f->pat4==pat4_original);
    CHECK(intel_gpu_bcs_owner_close(&f->owner)==0);
    CHECK(intel_gpu_capture_release(&f->bootstrap)==0);
    service_step(); jobs_cleanup(&f->jobs);
    CHECK(intel_gpu_executor_quiescent());
}
static void owner_cases(void)
{
    for (unsigned mode=0;mode<13;mode++) {
        OWNER_FIXTURE f; owner_setup(&f);
        if (mode==1) ram.fail_sync=ram.syncs+1;
        if (mode==2) ggtt_fail=true;
        NTSTATUS prepared=intel_gpu_bcs_owner_prepare(&f.owner,&f.bootstrap);
        if (mode==1 || mode==2) { CHECK(prepared!=0); owner_cleanup(&f); continue; }
        CHECK(prepared==0);
        CHECK(intel_gpu_bcs_owner_prepare(&f.owner,&f.bootstrap)==STATUS_INVALID_PARAMETER);
        if (mode==3) f.pat_fail=1;
        if (mode==4) hw.fail_at=hw.ops+1;
        NTSTATUS started=intel_gpu_bcs_owner_start(&f.owner);
        if (mode==3 || mode==4) { CHECK(started!=0); owner_cleanup(&f); continue; }
        CHECK(started==0 && f.owner.executor && f.running && f.pat==0x00112233);
        CHECK(intel_gpu_bcs_owner_prepare(&f.owner,&f.bootstrap)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_bcs_owner_start(&f.owner)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_capture_release(&f.bootstrap)==0);
        uint64_t id;
        intel_gpu_kmt_cpu_lease_t cpu={0};
        if (mode>=9) CHECK(intel_gpu_kmt_acquire_cpu(7,f.jobs.base.service.native.device,
            f.jobs.base.allocations[1],&cpu)==0);
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&id)==0);
        if (mode>=9) {
            for (unsigned retry=0;retry<16;retry++) {
                unsigned before=executor_ap_calls;
                executor_step(); service_step();
                CHECK(!f.jobs.done_count && !f.owner.engine.submitted_jobs && executor_ticks==1);
                CHECK(executor_ap_calls-before<=2); /* initial enqueue hint, then one attempt per timer */
            }
            if (mode==10) CHECK(intel_gpu_jobs_cancel(&f.jobs.jobs,7,id)==D3DKMT_STATUS_DEVICE_BUSY);
            if (mode==11) CHECK(intel_gpu_executor_stop(f.owner.executor)==0);
            if (mode==12) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
            if (mode>=10) {
                executor_step(); service_step();
                CHECK(f.jobs.done_count==1 && f.jobs.done_status[0]!=0 && !f.owner.engine.submitted_jobs);
            }
            CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
            if (mode>=10) { owner_cleanup(&f); continue; }
        }
        if (mode==5) hw.delay=1000;
        if (mode==6) f.token_fail=1;
        for (unsigned i=0;i<8 && !f.jobs.done_count;i++) { executor_step(); service_step(); }
        if (mode==5) {
            CHECK(!f.jobs.done_count && f.owner.slots[0].native.access_token);
            hw.stall_stop=true;
            CHECK(intel_gpu_bcs_owner_close(&f.owner)==D3DKMT_STATUS_DEVICE_BUSY);
            executor_step(); service_step();
            CHECK(!f.owner.engine.retired && !f.owner.closed && f.running && f.owner.hwsp.live);
            CHECK(!f.jobs.done_count && !ram.freed);
        } else if (mode==6) CHECK(f.jobs.done_count==1 && f.jobs.done_status[0]==STATUS_UNSUCCESSFUL);
        else {
            CHECK(f.jobs.done_count==1 && f.jobs.done_status[0]==0 && f.owner.engine.completed_jobs==1);
            if (mode==7) {
                D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
                OKMT_HANDLE old[2]={f.jobs.base.allocations[0],f.jobs.base.allocations[1]};
                CHECK(d3dkmt_create_allocations(7,f.jobs.base.service.native.device,2,sizes,f.jobs.base.allocations)==0);
                capture_fill(&f.jobs.base);
                CHECK(d3dkmt_destroy_allocations(7,f.jobs.base.service.native.device,2,old)==0);
                CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&id)==0);
                for (unsigned i=0;i<8 && f.jobs.done_count<2;i++) { executor_step(); service_step(); }
                CHECK(f.jobs.done_count==2 && f.jobs.done_status[1]==0 && f.owner.slots[0].native.rebindings==1);
                CHECK(f.owner.engine.tlb_invalidations==1 && ram.freed==2);
            }
            if (mode==8) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        }
        owner_cleanup(&f);
    }
}
/* Two modeled NT owners share an engine, never a resource or logical handle. */
static void owner_process_done(void *opaque,uint64_t id,DWORD pid,NTSTATUS status)
{
    JOBS_FIXTURE *f=opaque;
    CHECK((pid==7 || pid==8) && id && f->done_count<32);
    for (unsigned i=0;i<f->done_count;i++) CHECK(f->done_ids[i]!=id);
    f->done_ids[f->done_count]=id; f->done_status[f->done_count++]=status;
}
static void owner_dynamic_cases(void)
{
    for (unsigned mode=0;mode<9;mode++) {
        OWNER_FIXTURE f; owner_setup(&f);
        CHECK(intel_gpu_capture_release(&f.bootstrap)==0);
        if (mode==1) ram.fail_allocate=ram.allocated+1;
        if (mode==2) ggtt_fail=true;
        NTSTATUS reserved=intel_gpu_bcs_owner_reserve(&f.owner,mode==5 ? 1 : 2,8);
        if (mode==1 || mode==2) {
            CHECK(reserved!=0);
            if (mode==1) {
                /* Fixture counts allocation attempts; no backing was returned. */
                CHECK(ram.allocated==ram.fail_allocate && !ram.cpu[ram.allocated-1]);
                ram.allocated--;
            }
            owner_cleanup(&f); continue;
        }
        CHECK(reserved==0 && !f.owner.slots[0].pid && f.owner.slots[0].native.reserved);
        CHECK(intel_gpu_bcs_owner_reserve(&f.owner,1,8)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_bcs_owner_start(&f.owner)==0);
        CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
        DWORD second_pid=mode==8 ? 8 : 7;
        OKMT_HANDLE second_device=n->device,second_adapter=0,second_allocations[2]={0};
        if (mode==8) {
            f.jobs.jobs.done=owner_process_done; model_pid=8;
            CHECK(d3dkmt_open_adapter(8,n->luid,&second_adapter)==0);
            CHECK(d3dkmt_create_device(8,second_adapter,0,&second_device)==0);
            D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
            CHECK(d3dkmt_create_allocations(8,second_device,2,sizes,second_allocations)==0);
        }
        D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
        memcpy(desc.private_data,&command_layout,sizeof(command_layout));
        D3DKMT_CONTEXT_BUFFERS buffers={0}; OKMT_HANDLE second=0;
        CHECK(d3dkmt_create_context(second_pid,second_device,&desc,&second,&buffers)==0);
        intel_gpu_command_context_t *first=&n->contexts.contexts[0],*next=&n->contexts.contexts[1];
        memcpy(next->cpu.cpu,first->cpu.cpu,(size_t)first->cpu.bytes);
        if (mode==8) {
            OKMT_ALLOCATION_LIST list[2]={{second_allocations[0],0},{second_allocations[1],1}};
            memcpy((BYTE *)next->cpu.cpu+next->allocation_offset,list,sizeof(list));
            model_pid=7;
        }
        uint64_t before_ggtt[64]; memcpy(before_ggtt,ggtt,sizeof(ggtt));
        unsigned allocated=ram.allocated;
        uint64_t id=0;
        if (mode==3) {
            intel_gpu_capture_t probe={0};
            CHECK(intel_gpu_capture_begin(7,f.jobs.base.context,128,64,2,2,&probe)==0);
            CHECK(intel_bcs_context_activate(&f.owner.slots[0].native,8,
                probe.resources[0].native.buffer,probe.resources[1].native.buffer)==INTEL_BCS_INVALID);
            CHECK(!f.owner.slots[0].native.owner && f.owner.slots[0].native.reserved);
            CHECK(intel_gpu_capture_release(&probe)==0);
        }
        intel_gpu_kmt_cpu_lease_t cpu={0};
        if (mode==7) CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,f.jobs.base.allocations[1],&cpu)==0);
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&id)==0);
        if (mode==4) ram.fail_sync=ram.syncs+1; /* first activation cannot publish its tables */
        if (mode==6) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
        if (mode==7) {
            for (unsigned retry=0;retry<16;retry++) {
                unsigned calls=executor_ap_calls; executor_step(); service_step();
                CHECK(!f.jobs.done_count && !f.owner.engine.submitted_jobs && executor_ticks==1);
                CHECK(executor_ap_calls-calls<=2);
            }
            CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
        }
        for (unsigned i=0;i<8 && !f.jobs.done_count;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==1);
        CHECK(ram.allocated==allocated && !memcmp(before_ggtt,ggtt,sizeof(ggtt)));
        if (mode==4 || mode==6) {
            CHECK(f.jobs.done_status[0]!=0 && !f.owner.engine.submitted_jobs);
            owner_cleanup(&f); continue;
        }
        CHECK(f.jobs.done_status[0]==0 && f.owner.slots[0].pid==7 && f.owner.slots[0].native.ready);
        CHECK(f.owner.slots[0].native.tables.owner==1 && f.owner.slots[0].native.owner==7);
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,second_pid,second,128,64,2,2,&id)==0);
        for (unsigned i=0;i<8 && f.jobs.done_count<2;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==2 && (mode==5 ? f.jobs.done_status[1]!=0 : f.jobs.done_status[1]==0));
        if (mode!=5) CHECK(f.owner.slots[1].context!=f.owner.slots[0].context && f.owner.slots[1].pid==second_pid);
        if (mode==8) {
            CHECK(f.owner.slots[1].native.source!=f.owner.slots[0].native.source &&
                f.owner.slots[1].native.destination!=f.owner.slots[0].native.destination);
            vm_model_exit(8); d3dkmt_release_process(8); service_step();
            CHECK(f.owner.slots[1].native.source->live && !f.owner.engine.retired);
        }
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&id)==0);
        for (unsigned i=0;i<8 && f.jobs.done_count<3;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==3 && f.jobs.done_status[2]==0);
        CHECK(ram.allocated==allocated && !memcmp(before_ggtt,ggtt,sizeof(ggtt)));
        CHECK(f.owner.slots[0].native.prepared && f.owner.engine.completed_jobs==(mode==5 ? 2u : 3u));
        owner_cleanup(&f);
    }
}

#ifndef INTEL_OWNER_SUITE_ENTRY
#define INTEL_OWNER_SUITE_ENTRY main
#endif
int INTEL_OWNER_SUITE_ENTRY(void)
{
    if (executor_suite_main()) return 1;
    unsigned before=checks;
    owner_cases(); owner_dynamic_cases();
    printf("Intel native BCS owner: %u checks passed, hardware lifetime, live rebind and dynamic activation covered\n",checks-before);
    return 0;
}
