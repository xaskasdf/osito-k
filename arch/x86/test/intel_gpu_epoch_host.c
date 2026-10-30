/* A PID can return with a new NT incarnation while old DMA backing retires. */
#define INTEL_DRIVER_SUITE_ENTRY driver_suite_main
#include "intel_gfx_driver_host.c"

static void epoch_memory_cases(void)
{
    for (unsigned round=0;round<32;round++) {
        res_reset(); intel_gpu_memory_t memory={0};
        uint32_t old=0,next=0,resource=0,current=0,rejected=0xeeee;
        uint64_t epoch=101+round*2,sampled=0;
        CHECK(intel_gpu_memory_init(&memory,&allocator,&ram,8,8));
        CHECK(intel_gpu_memory_publish(&memory)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_open_device_epoch(&memory,1,0,&rejected,true)==INTEL_GPU_BUFFER_INVALID && rejected==0xeeee);
        CHECK(intel_gpu_memory_open_device_epoch(&memory,1,epoch,&old,true)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_device_epoch(&memory,1,old,&sampled)==INTEL_GPU_BUFFER_OK && sampled==epoch);
        CHECK(intel_gpu_memory_create(&memory,1,old,4096,&resource)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_lease_t held={0},stale={0};
        CHECK(intel_gpu_memory_acquire(&memory,1,old,resource,&held)==INTEL_GPU_BUFFER_OK);
        intel_gpu_memory_release_process(1);
        CHECK(intel_gpu_memory_open_device_epoch(&memory,1,epoch,&rejected,true)==INTEL_GPU_BUFFER_INVALID && rejected==0xeeee);
        sampled=0xeeee;
        CHECK(intel_gpu_memory_device_epoch(&memory,1,old,&sampled)==INTEL_GPU_BUFFER_INVALID && sampled==0xeeee);
        CHECK(intel_gpu_memory_open_device_epoch(&memory,1,epoch+1,&next,true)==INTEL_GPU_BUFFER_OK && old!=next);
        CHECK(intel_gpu_memory_device_epoch(&memory,1,next,&sampled)==INTEL_GPU_BUFFER_OK && sampled==epoch+1);
        CHECK(intel_gpu_memory_acquire(&memory,1,old,resource,&stale)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_acquire(&memory,1,next,resource,&stale)==INTEL_GPU_BUFFER_INVALID);
        CHECK(intel_gpu_memory_create(&memory,1,next,4096,&current)==INTEL_GPU_BUFFER_OK && current!=resource);
        CHECK(memory.pages==2);
        CHECK(intel_gpu_memory_release_device(&memory,1,old)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_memory_release(&held)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && memory.pages==1);
        CHECK(intel_gpu_memory_acquire(&memory,1,next,current,&held)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release(&held)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_close_device(&memory,1,next)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_release_device(&memory,1,next)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_memory_collect(&memory)==INTEL_GPU_BUFFER_OK && !memory.pages);
        intel_gpu_memory_shutdown(&memory);
        CHECK(intel_gpu_memory_unpublish(&memory)==INTEL_GPU_BUFFER_OK && ram.allocated==ram.freed);
    }
}

static void epoch_queue_cases(void)
{
    for (unsigned round=0;round<32;round++) {
        memset(&queue,0,sizeof(queue)); memset(&queue_engine,0,sizeof(queue_engine));
        bcs_model_reset(); prepares=0; prepare_failure=false;
        CHECK(intel_bcs_enable(&queue_io,NULL,0x1003000,&queue_engine)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_init(&queue,&queue_engine,&queue_io,NULL,&queue_ops,token)==INTEL_BCS_OK);
        uint64_t old=0,next=0,fence=0,rejected=0xeeee,epoch=101+round*2;
        queue_backing[0]=0; queue_backing[1]=1;
        CHECK(intel_bcs_queue_attach_epoch(&queue,41,epoch,&queue_backing[0],&old)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_publish(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_enqueue(&queue,41,old,&queue_copy,&fence)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_step(&queue)==INTEL_BCS_PENDING);
        intel_bcs_release_process(41); /* old incarnation exits during DMA */
        CHECK(queue.contexts[0].process_exited && queue.contexts[0].references==1);
        CHECK(intel_bcs_queue_attach_epoch(&queue,41,epoch,&queue_backing[1],&rejected)==INTEL_BCS_INVALID && rejected==0xeeee);
        CHECK(intel_bcs_queue_attach_epoch(&queue,41,epoch+1,&queue_backing[1],&next)==INTEL_BCS_OK && next!=old);
        CHECK(queue.contexts[0].process_exited && !queue.contexts[1].process_exited);
        CHECK(intel_bcs_queue_enqueue(&queue,41,old,&queue_copy,&rejected)==INTEL_BCS_INVALID && rejected==0xeeee);
        CHECK(intel_bcs_queue_enqueue(&queue,41,next,&queue_copy,&fence)==INTEL_BCS_OK);
        for (unsigned step=0;step<20 && queue_engine.completed_jobs<2;step++) {
            enum intel_bcs_status status=intel_bcs_queue_step(&queue);
            CHECK(status==INTEL_BCS_OK || status==INTEL_BCS_PENDING);
        }
        CHECK(queue_engine.completed_jobs==2 && !hw.resets);
        intel_bcs_job_result_t result;
        CHECK(intel_bcs_queue_result(&queue,41,fence,true,&result)==INTEL_BCS_OK && result.phase==INTEL_BCS_JOB_COMPLETE);
        CHECK(intel_bcs_queue_shutdown(&queue)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,41,old)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_release_context(&queue,41,next)==INTEL_BCS_OK);
        CHECK(intel_bcs_queue_unpublish(&queue)==INTEL_BCS_OK);
    }
}

#ifndef INTEL_EPOCH_SUITE_ENTRY
#define INTEL_EPOCH_SUITE_ENTRY main
#endif
int INTEL_EPOCH_SUITE_ENTRY(void)
{
    if (driver_suite_main()) return 1;
    unsigned before=checks;
    epoch_memory_cases(); epoch_queue_cases();
    printf("Intel GPU process incarnations: %u checks passed\n",checks-before);
    return 0;
}
