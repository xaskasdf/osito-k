/* Real queue/owner/capture code; synthetic MMIO/DMA and modeled NT admission. */
#define INTEL_EPOCH_SUITE_ENTRY epoch_suite_main
#include "intel_gpu_epoch_host.c"

static struct {
    unsigned entered,resume,calls;
    enum intel_bcs_status result,returned;
    uint64_t next;
} recycle_race;
static enum intel_bcs_status recycle_queue_update(void *backing,void *argument,
    const intel_gt_io_t *io,intel_bcs_state_t *engine)
{
    CHECK(backing==&queue_backing[0] && argument==&recycle_race && io==&queue.io && engine==&queue_engine);
    CHECK(queue.worker_busy && queue.contexts[0].updating && queue.contexts[0].references==1);
    CHECK(!queue_engine.inflight && queue_engine.submitted_jobs==queue_engine.completed_jobs);
    recycle_race.calls++;
    __atomic_store_n(&recycle_race.entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&recycle_race.resume,__ATOMIC_ACQUIRE)) sched_yield();
    return recycle_race.result;
}
static void *recycle_queue_thread(void *unused)
{
    (void)unused;
    recycle_race.returned=intel_bcs_queue_retarget_context(&queue,41,queue_ids[0],43,101,
        recycle_queue_update,&recycle_race,&recycle_race.next);
    return NULL;
}
static void recycle_queue_cases(void)
{
    for (unsigned round=0;round<16;round++) for (unsigned mode=0;mode<6;mode++) {
        bcs_queue_host_start(); memset(&recycle_race,0,sizeof(recycle_race));
        recycle_race.next=0xeeee; recycle_race.result=mode==5 ? INTEL_BCS_QUARANTINED : INTEL_BCS_OK;
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_retarget_context(&queue,41,queue_ids[0],43,101,
            recycle_queue_update,&recycle_race,&recycle_race.next)==INTEL_BCS_INVALID); /* still live */
        uint64_t fence=0;
        CHECK(intel_bcs_queue_enqueue(&queue,41,queue_ids[0],&queue_copy,&fence)==INTEL_BCS_OK);
        if (round&1) CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
        CHECK(intel_bcs_queue_close_context(&queue,41,queue_ids[0])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_retarget_context(&queue,41,queue_ids[0],43,101,
            recycle_queue_update,&recycle_race,&recycle_race.next)==INTEL_BCS_BUSY); /* unread result */
        if (round&1) {
            CHECK(queue_engine.inflight && queue.contexts[0].references==1);
            for (unsigned i=0;i<20 && !queue_engine.completed_jobs;i++) {
                enum intel_bcs_status status=intel_bcs_queue_step(&queue);
                CHECK(status==INTEL_BCS_OK || status==INTEL_BCS_PENDING);
            }
            CHECK(queue_engine.completed_jobs==1);
            CHECK(intel_bcs_queue_retarget_context(&queue,41,queue_ids[0],43,101,
                recycle_queue_update,&recycle_race,&recycle_race.next)==INTEL_BCS_BUSY);
        }
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK &&
            result.phase==((round&1) ? INTEL_BCS_JOB_COMPLETE : INTEL_BCS_JOB_CANCELLED));
        uint64_t next=queue.next_context; queue.next_context=UINT64_MAX;
        CHECK(intel_bcs_queue_retarget_context(&queue,41,queue_ids[0],43,101,
            recycle_queue_update,&recycle_race,&recycle_race.next)==INTEL_BCS_BUSY);
        queue.next_context=next;
        CHECK(!recycle_race.calls && recycle_race.next==0xeeee);
        pthread_t worker; CHECK(!pthread_create(&worker,NULL,recycle_queue_thread,NULL));
        while (!__atomic_load_n(&recycle_race.entered,__ATOMIC_ACQUIRE)) sched_yield();
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_BUSY);
        CHECK(intel_bcs_queue_release_context(&queue,41,queue_ids[0])==INTEL_BCS_BUSY);
        if (mode==1 || mode==3) intel_bcs_release_process(41);
        if (mode==2 || mode==3) intel_bcs_release_process(43);
        if (mode==4) CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_BUSY);
        __atomic_store_n(&recycle_race.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(worker,NULL));
        CHECK(recycle_race.returned==recycle_race.result && recycle_race.calls==1);
        CHECK(!queue.contexts[0].replacement_owner && !queue.contexts[0].updating && !queue.contexts[0].references);
        uint64_t owner=41,context=queue_ids[0];
        if (mode==5) {
            CHECK(queue.fault==INTEL_BCS_QUARANTINED && recycle_race.next==0xeeee);
            CHECK(queue.contexts[0].owner==41 && queue.contexts[0].id==context);
        } else {
            owner=43; context=recycle_race.next;
            CHECK(context>queue_ids[1] && queue.contexts[0].owner==43 && queue.contexts[0].epoch==101);
            CHECK(queue.contexts[0].closing==(mode>=2 && mode<=4));
            CHECK(queue.contexts[0].process_exited==(mode==2 || mode==3));
            CHECK(intel_bcs_queue_close_context(&queue,41,queue_ids[0])==INTEL_BCS_INVALID);
        }
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,owner,context)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,42,queue_ids[1])==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    }
}

static void recycle_owner_cases(void)
{
    OWNER_FIXTURE f; owner_setup(&f); f.jobs.jobs.done=owner_process_done;
    CHECK(intel_gpu_capture_release(&f.bootstrap)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&f.owner,1,8)==0 && intel_gpu_bcs_owner_start(&f.owner)==0);
    CONTEXT_FIXTURE *n=&f.jobs.base.service.native;
    uint64_t saved_ggtt[64]; memcpy(saved_ggtt,ggtt,sizeof(ggtt));
    void *saved_state=f.owner.slots[0].native.state.cpu,*saved_tables=f.owner.slots[0].native.tables.cpu;
    unsigned resets=hw.resets;
    for (unsigned round=0;round<24;round++) {
        DWORD pid=round%2 ? 8 : 7; model_pid=pid;
        OKMT_HANDLE adapter=n->adapter,device=n->device,context=f.jobs.base.context;
        OKMT_HANDLE allocations[2]={f.jobs.base.allocations[0],f.jobs.base.allocations[1]};
        if (round) {
            CHECK(d3dkmt_open_adapter(pid,n->luid,&adapter)==0);
            CHECK(d3dkmt_create_device(pid,adapter,0,&device)==0);
            D3DKMT_ALLOCATION_DESCRIPTION sizes[2]={{1000},{1000}};
            CHECK(d3dkmt_create_allocations(pid,device,2,sizes,allocations)==0);
            D3DKMT_CONTEXT_DESCRIPTION description={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
            memcpy(description.private_data,&command_layout,sizeof(command_layout));
            D3DKMT_CONTEXT_BUFFERS buffers;
            CHECK(d3dkmt_create_context(pid,device,&description,&context,&buffers)==0);
        }
        intel_gpu_kmt_context_lease_t lease={0};
        CHECK(intel_gpu_kmt_acquire_context(pid,context,&lease)==0);
        uint64_t native_id=lease.native.id; DWORD native_device=lease.native.device;
        CHECK(!intel_gpu_context_closed(&n->contexts,pid,native_device,native_id));
        CHECK(!intel_gpu_context_closed(&n->contexts,pid,native_device,UINT64_MAX));
        intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
            INTEL_UMD_COPY32,sizeof(command),16,8,128,128,0,0,{0}};
        OKMT_ALLOCATION_LIST list[2]={{allocations[0],0},{allocations[1],1}};
        OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
        memcpy((BYTE *)lease.native.commands+128,&command,sizeof(command));
        memcpy(lease.native.allocations,list,sizeof(list)); memcpy(lease.native.patches,patches,sizeof(patches));
        CHECK(intel_gpu_kmt_release_context(&lease)==0);
        unsigned allocated=ram.allocated; uint64_t job=0;
        CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,pid,context,128,64,2,2,&job)==0);
        for (unsigned i=0;i<12 && f.jobs.done_count<=round;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==round+1 && f.jobs.done_status[round]==0);
        CHECK(f.owner.count==1 && f.owner.slots[0].pid==pid && f.owner.slots[0].native.owner==pid);
        CHECK(f.owner.slots[0].native.state.cpu==saved_state && f.owner.slots[0].native.tables.cpu==saved_tables);
        /* Each previous owner is unmapped before its slot is retargeted. */
        CHECK(f.owner.slots[0].native.rebindings==2*round && f.owner.engine.tlb_invalidations==2*round);
        CHECK(!memcmp(saved_ggtt,ggtt,sizeof(ggtt)) && ram.allocated==allocated && hw.resets==resets && f.starts==1);
        CHECK(d3dkmt_destroy_context(pid,context)==0 && intel_gpu_context_closed(&n->contexts,pid,native_device,native_id));
        CHECK(d3dkmt_destroy_device(pid,device)==0);
        if (round) CHECK(d3dkmt_close_adapter(pid,adapter)==0);
        for (unsigned i=0;i<4;i++) service_step();
        CHECK(n->memory.pages<=8);
    }
    CHECK(f.owner.engine.completed_jobs==24);
    model_pid=7; owner_cleanup(&f);
}

static bool recycle_fail_tlb;
static bool recycle_fault_read(void *opaque,uint32_t reg,uint32_t *value)
{
    if (recycle_fail_tlb && reg==0x426c) return false;
    return owner_read(opaque,reg,value);
}
static void recycle_retarget_fault_cases(void)
{
    for (unsigned mode=0;mode<4;mode++) {
        OWNER_FIXTURE f; owner_setup(&f);
        CHECK(intel_gpu_capture_release(&f.bootstrap)==0);
        CHECK(intel_gpu_bcs_owner_reserve(&f.owner,1,8)==0 && intel_gpu_bcs_owner_start(&f.owner)==0);
        uint64_t job=0; CHECK(intel_gpu_jobs_submit(&f.jobs.jobs,7,f.jobs.base.context,128,64,2,2,&job)==0);
        for (unsigned i=0;i<8 && !f.jobs.done_count;i++) { executor_step(); service_step(); }
        CHECK(f.jobs.done_count==1 && !f.jobs.done_status[0]);
        intel_bcs_context_t *native=&f.owner.slots[0].native;
        CHECK(intel_bcs_queue_close_context(&f.owner.queue,7,native->queue_id)==INTEL_BCS_OK);
        intel_gpu_buffer_t replacements[2]={0};
        CHECK(intel_gpu_buffer_create(&replacements[0],&allocator,&ram,8,1000)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_create(&replacements[1],&allocator,&ram,8,1000)==INTEL_GPU_BUFFER_OK);
        intel_gpu_buffer_t *old=native->source;
        if (mode==1) ram.fail_sync=ram.syncs+1;
        if (mode==2) { recycle_fail_tlb=true; f.owner.config.io.read=recycle_fault_read; }
        if (mode==3) old->busy=1;
        enum intel_bcs_status status=intel_bcs_context_retarget(native,8,123,&replacements[0],&replacements[1]);
        CHECK(status==(mode ? INTEL_BCS_QUARANTINED : INTEL_BCS_OK));
        if (mode) CHECK(!native->ready && f.owner.queue.fault==INTEL_BCS_QUARANTINED);
        if (mode==3) CHECK(native->owner==8 && native->queue_owner==7 && native->retained_owner==7);
        if (!mode) CHECK(native->owner==8 && native->queue_owner==8 && native->epoch==123);
        old->busy=0; ram.fail_sync=0; recycle_fail_tlb=false;
        NTSTATUS closed=D3DKMT_STATUS_DEVICE_BUSY;
        for (unsigned i=0;i<16 && closed;i++) { closed=intel_gpu_bcs_owner_close(&f.owner); executor_step(); service_step(); }
        CHECK(closed==0);
        CHECK(intel_gpu_buffer_close(&replacements[0],8)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_close(&replacements[1],8)==INTEL_GPU_BUFFER_OK);
        owner_cleanup(&f);
    }
}

#ifndef INTEL_RECYCLE_SUITE_ENTRY
#define INTEL_RECYCLE_SUITE_ENTRY main
#endif
int INTEL_RECYCLE_SUITE_ENTRY(void)
{
    if (epoch_suite_main()) return 1;
    unsigned before=checks;
    recycle_queue_cases(); recycle_owner_cases(); recycle_retarget_fault_cases();
    printf("Intel native slot recycling: %u checks passed, 24 owners on one slot\n",checks-before);
    return 0;
}
