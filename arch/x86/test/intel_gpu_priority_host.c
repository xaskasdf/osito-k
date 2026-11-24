#define NATIVE_JOBS_SUITE_ENTRY priority_jobs_regression
#include "intel_gpu_jobs_host.c"

static OKMT_HANDLE priority_context_create(JOBS_FIXTURE *f,unsigned slot)
{
    CONTEXT_FIXTURE *n=&f->base.service.native;
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));D3DKMT_CONTEXT_BUFFERS buffers={0};OKMT_HANDLE context;
    CHECK(d3dkmt_create_context(7,n->device,&desc,&context,&buffers)==0);
    intel_gpu_command_context_t *source=&n->contexts.contexts[0],*target=&n->contexts.contexts[slot];
    memcpy((BYTE *)target->cpu.cpu+128,(BYTE *)source->cpu.cpu+128,sizeof(intel_umd_copy32_t));
    memcpy((BYTE *)target->cpu.cpu+target->allocation_offset,(BYTE *)source->cpu.cpu+source->allocation_offset,2*sizeof(OKMT_ALLOCATION_LIST));
    memcpy((BYTE *)target->cpu.cpu+target->patch_offset,(BYTE *)source->cpu.cpu+source->patch_offset,2*sizeof(OKMT_PATCH_LOCATION));
    return context;
}
static void priority_set(OKMT_HANDLE context,LONG value)
{CHECK(d3dkmt_context_priority(7,context,TRUE,&value)==0);}
static uint64_t priority_submit(JOBS_FIXTURE *f,OKMT_HANDLE context)
{uint64_t id=0;CHECK(intel_gpu_jobs_submit(&f->jobs,7,context,128,64,2,2,&id)==0);return id;}
static uint64_t priority_take(JOBS_FIXTURE *f)
{
    intel_gpu_job_lease_t lease={0};CHECK(intel_gpu_jobs_take(&f->jobs,&lease)==0);uint64_t id=lease.id;
    CHECK(intel_gpu_jobs_retire(&lease,0)==0);service_step();return id;
}
static void native_priority_cases(void)
{
    for(unsigned mode=0;mode<7;mode++) {
        JOBS_FIXTURE f;jobs_setup(&f);OKMT_HANDLE a=f.base.context,b=priority_context_create(&f,1);
        priority_set(a,-7);priority_set(b,7);
        if(mode<3) {
            if(mode==1) {priority_set(a,7);priority_set(b,-7);}
            if(mode==2) {priority_set(a,7);priority_set(b,-7);}
            uint64_t first=priority_submit(&f,a),second=mode==1?priority_submit(&f,a):0,third=priority_submit(&f,b);
            if(mode==2){priority_set(a,-7);priority_set(b,7);} /* queued work sees live changes */
            if(mode==1){CHECK(priority_take(&f)==first);CHECK(priority_take(&f)==second);CHECK(priority_take(&f)==third);}
            else {CHECK(priority_take(&f)==third);CHECK(priority_take(&f)==first);}
        } else if(mode==3) {
            uint64_t low=priority_submit(&f,a);unsigned bypassed=0;
            for(;;) {
                (void)priority_submit(&f,b);
                uint64_t chosen=priority_take(&f);
                if(chosen==low)break;
                CHECK(++bypassed<=15);
            }
            CHECK(bypassed>0 && bypassed<=15);
        } else if(mode==6) {
            OKMT_HANDLE high[3]={b,priority_context_create(&f,2),priority_context_create(&f,3)};
            uint64_t ticket[3];
            for(unsigned i=0;i<3;i++)priority_set(high[i],7);
            uint64_t low=priority_submit(&f,a);
            for(unsigned i=0;i<3;i++)ticket[i]=priority_submit(&f,high[i]);
            unsigned bypassed=0;
            for(;;) {
                uint64_t selected=priority_take(&f);
                if(selected==low)break;
                /* Keep all three producers continuously runnable. Checking
                 * each completion separately avoids the fixture's log cap. */
                f.done_count=0;
                CHECK(++bypassed<24);
                unsigned i=0;while(i<3 && ticket[i]!=selected)i++;CHECK(i<3);
                ticket[i]=priority_submit(&f,high[i]);
            }
            CHECK(bypassed>0 && bypassed<24);
        } else {
            OKMT_HANDLE sem;D3DKMT_SYNC_DESCRIPTION description={OKMT_SYNC_SEMAPHORE,0,2};
            CHECK(d3dkmt_create_sync(7,f.base.service.native.device,&description,&sem)==0);
            D3DKMT_SYNC_COMMAND command={.count=1,.objects={sem}};uint64_t id;
            if(mode==4) {
                CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,b,&command,&id)==0);
                uint64_t low=priority_submit(&f,a);CHECK(priority_take(&f)==low);
                CHECK(intel_gpu_jobs_pending(&f.jobs,7,b)==1); /* blocked high priority cannot stall others */
            } else {
                OKMT_HANDLE c=priority_context_create(&f,2);priority_set(c,0);
                uint64_t normal=priority_submit(&f,c);
                command.signal=TRUE;command.extended=TRUE;command.broadcast_count=2;command.broadcast[0]=a;command.broadcast[1]=b;
                CHECK(intel_gpu_jobs_submit_sync(&f.jobs,7,a,&command,&id)==0);
                intel_gpu_job_lease_t lease={0};CHECK(intel_gpu_jobs_take(&f.jobs,&lease)==0 && lease.id==normal);
                CHECK(!intel_gpu_jobs_pending(&f.jobs,7,a) && !intel_gpu_jobs_pending(&f.jobs,7,b));
                CHECK(intel_gpu_jobs_retire(&lease,0)==0);service_step();
            }
        }
        jobs_cleanup(&f);
    }
}
int main(void)
{
    if(priority_jobs_regression())return 1;
    unsigned before=checks;native_priority_cases();
    printf("Native context priority: %u checks passed; live priority, FIFO, bounded bypass, blocked heads and broadcasts\n",checks-before);
    return 0;
}
