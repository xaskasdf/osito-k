/* Public capture -> retained native context -> queue. Only DMA/MMIO modeled. */
#define NATIVE_VERTEX_CAPTURE_ENTRY indexed_vertex_baseline
#include "intel_gpu_vertex_shader_capture_host.c"
static intel_umd_draw_indexed_t index_packet;
static OKMT_HANDLE index_handle;
static intel_gpu_buffer_t *index_buffer;
static unsigned index_jobs;
static void index_stage(void)
{
    intel_gpu_command_context_t *ctx=&sample.render.base.jobs.base.service.native.contexts.contexts[0];
    memcpy((BYTE *)ctx->cpu.cpu+128,&index_packet,sample.bytes);
    memcpy((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,sample.list,sample.count*sizeof(*sample.list));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->patch_offset,sample.patches,sample.patch_count*sizeof(*sample.patches));
}
static void index_fill(bool vertex,unsigned stride,bool all,bool alias)
{
    if (vertex) vertex_fill_program(all ? "ps30-all16" : "ps30-tint",alias);
    else {sample_setup();sample_fill(all ? "ps30-all16" : "ps30-tint",alias,0,false);}
    memset(&index_packet,0,sizeof(index_packet));
    if (vertex) memcpy(&index_packet,&vertex_packet,1552);
    else {
        memcpy(&index_packet,&sample.packet,1456);
        for(unsigned i=0;i<8;i++)if(index_packet.texcoord_offset[i])index_packet.texcoord_offset[i]|=3u<<16;
    }
    index_packet.draw.version=INTEL_UMD_DRAW_INDEXED_VERSION;
    index_packet.draw.vertex_count=4;
    index_packet.index_offset=3*stride;index_packet.index_stride=stride;
    index_packet.index_count=6;index_packet.min_index=2;index_packet.num_vertices=4;index_packet.base_vertex=-2;
    CONTEXT_FIXTURE *n=&sample.render.base.jobs.base.service.native;
    D3DKMT_ALLOCATION_DESCRIPTION size={4096};
    CHECK(d3dkmt_create_allocations(7,n->device,1,&size,&index_handle)==0);
    intel_gpu_kmt_cpu_lease_t cpu={0};CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,index_handle,&cpu)==0);
    index_buffer=cpu.access.buffer;memset(cpu.access.cpu,0xa5,4096);
    const unsigned entries[]={2,3,4,4,3,5};BYTE *raw=(BYTE *)cpu.access.cpu+index_packet.index_offset;
    for(unsigned i=0;i<6;i++)for(unsigned j=0;j<stride;j++)raw[i*stride+j]=(BYTE)(entries[i]>>(8*j));
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    sample.list[sample.count]=(OKMT_ALLOCATION_LIST){index_handle,0};
    sample.patches[sample.patch_count++]=(OKMT_PATCH_LOCATION){.allocation_index=sample.count++,
        .slot=INTEL_UMD_INDEX_ROLE,.patch_offset=128+1552};
    unsigned parameters=index_packet.parameter_count+index_packet.vertex_parameter_count;
    for(unsigned i=0;i<parameters;i++)index_packet.values[i]=0x3f000000u+i;
    intel_umd_rectangle_t *r=(void *)(index_packet.values+parameters);
    r[0]=(intel_umd_rectangle_t){0,0,8,16};r[1]=(intel_umd_rectangle_t){8,0,8,16};
    sample.bytes=1584+parameters*4+32;index_packet.draw.bytes=sample.bytes;index_stage();
}
static void index_capture_negative(void)
{
    for(unsigned vertex=0;vertex<2;vertex++)for(unsigned mode=0;mode<25;mode++) {
        index_fill(vertex,vertex ? 4 : 2,false,false);OWNER_FIXTURE *b=&sample.render.base;
        unsigned patch=sample.patch_count-1,allocation=sample.count-1;
        switch(mode) {
        case 0:break;
        case 1:index_packet.indices=4096;break;
        case 2:index_packet.index_offset++;break;
        case 3:index_packet.index_stride=1;break;
        case 4:index_packet.index_count=2;break;
        case 5:index_packet.index_count=65536;break;
        case 6:index_packet.index_offset=4096;break;
        case 7:index_packet.num_vertices=0;break;
        case 8:index_packet.min_index=0xffffffff;break;
        case 9:index_packet.base_vertex=INT32_MIN;break;
        case 10:index_packet.base_vertex=INT32_MAX;break;
        case 11:((BYTE *)index_buffer->cpu)[index_packet.index_offset]=6;break;
        case 12:index_packet.draw.vertex_count=3;break;
        case 13:index_packet.draw.vertex_count=0;break;
        case 14:index_packet.draw.topology=1;break;
        case 15:sample.patches[patch].slot=21;break;
        case 16:sample.patches[patch].patch_offset+=8;break;
        case 17:sample.list[allocation].allocation=sample.list[0].allocation;break;
        case 18:sample.list[allocation].flags=1;break;
        case 19:sample.patches[patch].allocation_index=0;break;
        case 20:sample.bytes-=4;index_packet.draw.bytes=sample.bytes;break;
        case 21:index_packet.draw.version=7;break;
        case 22:if(vertex)index_packet.vertex_program_bytes=0;else index_packet.vertex_parameter_count=1;break;
        case 23:if(vertex)index_packet.position_offset=4;else index_packet.viewport_width=16;break;
        case 24:sample.patches[patch].allocation_offset=2;break;
        }
        index_stage();intel_gpu_capture_t capture={0};
        NTSTATUS result=intel_gpu_capture_begin(7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&capture);
        if((result==0)!=(mode==0))fprintf(stderr,"indexed vertex=%u mode=%u status=%08x\n",vertex,mode,(unsigned)result);
        CHECK((result==0)==(mode==0));
        if(!mode) {
            CHECK(capture.resources[3+vertex].native.buffer==index_buffer);
            CHECK(capture.texture_resource[0]==4+vertex && !capture.index_cpu.self);
            CHECK(!memcmp(&capture.indexed,&index_packet,sample.bytes));
        }
        CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void index_capture_execution(void)
{
    for(unsigned vertex=0;vertex<2;vertex++)for(unsigned stride=2;stride<=4;stride+=2)for(unsigned mode=0;mode<9;mode++) {
        index_fill(vertex,stride,false,false);OWNER_FIXTURE *b=&sample.render.base;CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        uint64_t id=0;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&id)==0);
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        memset((BYTE *)ctx->cpu.cpu+128,0xa5,sample.bytes);
        memset((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,0xa5,sample.count*sizeof(*sample.list));
        memset((BYTE *)ctx->cpu.cpu+ctx->patch_offset,0xa5,sample.patch_count*sizeof(*sample.patches));
        intel_gpu_cpu_access_t cpu={0};
        if(mode==1)((BYTE *)index_buffer->cpu)[index_packet.index_offset]=6;
        if(mode==2)CHECK(d3dkmt_destroy_allocations(7,n->device,1,&index_handle)==0);
        if(mode==3)CHECK(intel_gpu_buffer_acquire_cpu(index_buffer,7,&cpu)==0);
        if(mode==4 || mode==5)hw.delay=1000;
        if(mode==6)CHECK(intel_gpu_jobs_cancel(&b->jobs.jobs,7,id)==0);
        if(mode==7){vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
        /* A valid edit after capture remains valid, but its original bytes
         * (not cached/expanded indices) are what the native buffer contains. */
        if(mode==8)((BYTE *)index_buffer->cpu)[index_packet.index_offset]=3;
        for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        intel_bcs_context_t *native=&b->owner.slots[0].native;
        if(mode==3) {
            CHECK(!b->jobs.done_count && !b->owner.engine.submitted_jobs && cpu.self);
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==0);
            for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        }
        if(mode==4 || mode==5) {
            CHECK(!b->jobs.done_count && b->owner.engine.inflight && native->index_access.self);
            CHECK(intel_gpu_buffer_acquire_cpu(index_buffer,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
            if(mode==5){vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
            hw.stall_stop=true;CHECK(intel_gpu_bcs_owner_close(&b->owner)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(native->index_access.self && index_buffer->residents);
        } else if(mode==0 || mode==3 || mode==8) {
            CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);index_jobs++;
            CHECK(native->indices.buffer==index_buffer && native->indices.gpu==INTEL_GPU_INDEX_VA_BASE);
            const DWORD *batch=native->batch.cpu;unsigned found=0;
            for(unsigned at=1;at<1024;) {
                DWORD op=batch[at];if(op==0x05000000)break;if(!op){at++;continue;}
                if((op&0xffff0000u)==0x780a0000u) {
                    uint64_t gpu=(uint64_t)batch[at+2]|((uint64_t)batch[at+3]<<32);
                    CHECK(gpu==INTEL_GPU_INDEX_VA_BASE+3*stride && batch[at+4]==6*stride);found|=1;
                }
                if((op&0xffff0000u)==0x7b000000u){CHECK(batch[at+1]&256);CHECK(batch[at+2]==6 && batch[at+6]==0xfffffffe);found|=2;}
                at+=(op&255)+2;
            }
            CHECK(found==3);
        } else CHECK(b->jobs.done_count==1 && b->jobs.done_status[0]!=0 && !b->owner.engine.submitted_jobs);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void index_capture_capacity(void)
{
    for(unsigned vertex=0;vertex<2;vertex++)for(unsigned alias=0;alias<2;alias++) {
        index_fill(vertex,4,true,alias);OWNER_FIXTURE *b=&sample.render.base;
        intel_gpu_capture_t capture={0};
        CHECK(sample.count==(alias ? 5u : 20u)+vertex && sample.patch_count==20+vertex);
        CHECK(intel_gpu_capture_begin(7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&capture)==0);
        CHECK(capture.resource_count==sample.count && capture.resources[3+vertex].native.buffer==index_buffer);
        for(unsigned i=0;i<16;i++)CHECK(capture.texture_resource[i]==4+vertex+(alias ? 0 : i));
        CHECK(intel_gpu_capture_release(&capture)==0);
        uint64_t id=0;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&id)==0);
        for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);index_jobs++;
        CHECK(b->owner.slots[0].native.texture_count==(alias ? 1u : 16u));
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
int main(int argc,char **argv)
{
    CHECK(indexed_vertex_baseline(argc,argv)==0);
    index_capture_negative();index_capture_execution();index_capture_capacity();
    printf("Native indexed capture PASS checks=%u completed_jobs=%u retained-indices\n",checks,index_jobs);return 0;
}
