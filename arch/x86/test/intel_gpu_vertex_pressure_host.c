/* A completed closed draw must not retain the next process's page budget. */
#define NATIVE_VERTEX_CAPTURE_ENTRY vertex_capture_suite_main
#include "intel_gpu_vertex_shader_capture_host.c"
static void vertex_closed_pressure(void)
{
    vertex_fill_program("ps30-all16",false);
    OWNER_FIXTURE *b=&sample.render.base;CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
    uint64_t id=0;
    CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&id)==0);
    for (unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
    CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);
    CHECK(d3dkmt_destroy_context(7,b->jobs.base.context)==0);
    CHECK(d3dkmt_destroy_device(7,n->device)==0);
    for (unsigned i=0;i<8;i++){service_step();executor_step();}
    fprintf(stderr,"closed VS context retained pages=%u\n",n->memory.pages);
    CHECK(n->memory.pages==0);
    OKMT_HANDLE device=0;CHECK(d3dkmt_create_device(7,n->adapter,0,&device)==0);
    D3DKMT_ALLOCATION_DESCRIPTION sizes[23];OKMT_HANDLE handles[23];
    for(unsigned i=0;i<23;i++)sizes[i]=(D3DKMT_ALLOCATION_DESCRIPTION){i==2 || i>=21 ? 8192 : 4096};
    CHECK(d3dkmt_create_allocations(7,device,23,sizes,handles)==0);
    CHECK(n->memory.pages==26);
    owner_cleanup(b);CHECK(ram.allocated==ram.freed);
}
int main(int argc,char **argv)
{
    CHECK(vertex_capture_suite_main(argc,argv)==0);
    vertex_closed_pressure();
    printf("Native vertex closed-context pressure PASS checks=%u\n",checks);return 0;
}
