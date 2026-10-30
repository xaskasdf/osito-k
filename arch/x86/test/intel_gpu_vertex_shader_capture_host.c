/* Real public-capture/queue/owner code; MMIO completion is modeled. */
#define NATIVE_SAMPLED_SUITE_ENTRY vertex_shader_sampled_baseline
#include "intel_gpu_sampled_capture_host.c"
static intel_umd_draw_vertex_shader_t vertex_packet;
static OKMT_HANDLE vertex_handle;
static intel_gpu_buffer_t *vertex_buffer;
static unsigned vertex_jobs;
static void vertex_stage(void)
{
    intel_gpu_command_context_t *ctx=&sample.render.base.jobs.base.service.native.contexts.contexts[0];
    memcpy((BYTE *)ctx->cpu.cpu+128,&vertex_packet,sample.bytes);
    memcpy((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,sample.list,sample.count*sizeof(*sample.list));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->patch_offset,sample.patches,sample.patch_count*sizeof(*sample.patches));
}
static void vertex_fill_program(const char *pixel,bool alias)
{
    sample_setup();sample_fill(pixel,alias,0,false);
    CONTEXT_FIXTURE *n=&sample.render.base.jobs.base.service.native;
    D3DKMT_ALLOCATION_DESCRIPTION size={8192};
    CHECK(d3dkmt_create_allocations(7,n->device,1,&size,&vertex_handle)==0);
    intel_gpu_kmt_cpu_lease_t cpu={0};CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,vertex_handle,&cpu)==0);
    vertex_buffer=cpu.access.buffer;
    FILE *file=fopen("build/native-vertex-pipeline/fixtures/vs30-transform.program","rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long bytes=ftell(file);rewind(file);CHECK(bytes>256 && bytes<4096);
    BYTE blob[4096];CHECK(fread(blob,1,bytes,file)==(size_t)bytes);fclose(file);
    struct intel_d3d9_vertex_program *vs=(void *)blob;unsigned old=vs->base.code_offset;
    vs->base.code_offset=4096;vs->base.allocation_bytes=4096+vs->base.code_bytes;
    memcpy(cpu.access.cpu,blob,old);memcpy((BYTE *)cpu.access.cpu+4096,blob+old,vs->base.code_bytes);
    CHECK(intel_gen9_vertex_shader_validate(vs,cpu.access.cpu,vs->base.allocation_bytes));
    CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    memset(&vertex_packet,0,sizeof(vertex_packet));memcpy(&vertex_packet,&sample.packet,1456);
    memset(vertex_packet.texcoord_offset,0,sizeof(vertex_packet.texcoord_offset));
    vertex_packet.draw.version=INTEL_UMD_DRAW_VERTEX_SHADER_VERSION;
    vertex_packet.draw.vertex_stride=32;
    vertex_packet.vertex_program_bytes=vs->base.allocation_bytes;vertex_packet.vertex_parameter_count=vs->base.parameter_count;
    vertex_packet.viewport_width=vertex_packet.viewport_height=16;
    vertex_packet.attributes[0]=3u<<16;vertex_packet.attributes[15]=(3u<<16)|16;
    for (unsigned i=0;i<vertex_packet.parameter_count+vertex_packet.vertex_parameter_count;i++)vertex_packet.values[i]=0x3f000000u+i;
    intel_umd_rectangle_t *rect=(void *)(vertex_packet.values+vertex_packet.parameter_count+vertex_packet.vertex_parameter_count);
    rect[0]=(intel_umd_rectangle_t){0,0,8,16};rect[1]=(intel_umd_rectangle_t){8,0,8,16};
    sample.list[sample.count]=(OKMT_ALLOCATION_LIST){vertex_handle,0};
    sample.patches[sample.patch_count++]=(OKMT_PATCH_LOCATION){.allocation_index=sample.count++,
        .slot=INTEL_UMD_VERTEX_PROGRAM_ROLE,.patch_offset=128+1456};
    sample.bytes=1552+(vertex_packet.parameter_count+vertex_packet.vertex_parameter_count)*4+32;
    vertex_packet.draw.bytes=sample.bytes;vertex_stage();
}
static void vertex_fill(void)
{
    vertex_fill_program("ps30-tint",false);
}
static void vertex_capture_capacity(void)
{
    for (unsigned alias=0;alias<2;alias++) {
        vertex_fill_program("ps30-all16",alias!=0);
        OWNER_FIXTURE *b=&sample.render.base;
        unsigned resources=alias ? 5 : 20;
        CHECK(sample.count==resources && sample.patch_count==20);
        intel_gpu_capture_t capture={0};
        CHECK(intel_gpu_capture_begin(7,b->jobs.base.context,128,sample.bytes,
            sample.count,sample.patch_count,&capture)==0);
        CHECK(capture.resource_count==resources && capture.resources[3].native.buffer==vertex_buffer);
        for (unsigned slot=0;slot<16;slot++) CHECK(capture.texture_resource[slot]==(alias ? 4 : 4+slot));
        CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        uint64_t id=0;
        CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,
            sample.count,sample.patch_count,&id)==0);
        for (unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);vertex_jobs++;
        CHECK(b->owner.slots[0].native.texture_count==(alias ? 1u : 16u));
        CHECK(b->owner.slots[0].native.vertex_program.buffer==vertex_buffer);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void vertex_capture_negative(void)
{
    for (unsigned mode=0;mode<26;mode++) {
        vertex_fill();OWNER_FIXTURE *b=&sample.render.base;
        switch(mode) {
        case 0:break;
        case 1:vertex_packet.vertex_program=4096;break;
        case 2:vertex_packet.vertex_program_bytes=8193;break;
        case 3:vertex_packet.vertex_parameter_count=513;break;
        case 4:vertex_packet.vertex_parameter_count++;break;
        case 5:vertex_packet.viewport_width=0;break;
        case 6:vertex_packet.viewport_x=1;break;
        case 7:vertex_packet.viewport_height=17;break;
        case 8:vertex_packet.attributes[0]=17u<<16;break;
        case 9:vertex_packet.attributes[0]|=1;break;
        case 10:vertex_packet.attributes[14]=3u<<16;break;
        case 11:vertex_packet.draw.vertex_stride=16;break;
        case 12:vertex_packet.reserved=4;break;
        case 13:vertex_packet.texcoord_offset[0]=16;break;
        case 14:sample.patches[4].slot=18;break;
        case 15:sample.patches[4].patch_offset++;break;
        case 16:sample.list[4].allocation=sample.list[2].allocation;break;
        case 17:sample.list[4].flags=1;break;
        case 18:sample.patches[4].allocation_index=3;break;
        case 19:sample.patches[3].allocation_index=4;break;
        case 20:((DWORD *)vertex_buffer->cpu)[1024]^=0x20;break;
        case 21:((struct intel_d3d9_vertex_program *)vertex_buffer->cpu)->base.code_offset=4095;break;
        case 22:vertex_packet.draw.vertex_offset=4096-64;break;
        case 23:vertex_packet.draw.version=6;break;
        case 24:sample.bytes-=4;vertex_packet.draw.bytes=sample.bytes;break;
        case 25:vertex_packet.vertex_program_bytes=128;break;
        }
        vertex_stage();intel_gpu_capture_t capture={0};
        NTSTATUS result=intel_gpu_capture_begin(7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&capture);
        if ((result==0)!=(mode==0))fprintf(stderr,"vertex mode=%u status=%08x resources=%u\n",mode,(unsigned)result,capture.resource_count);
        CHECK((result==0)==(mode==0));
        if (!mode) {
            CHECK(capture.resource_count==5 && capture.resources[3].native.buffer==vertex_buffer && capture.texture_resource[0]==4);
            CHECK(!memcmp(&capture.vertex,&vertex_packet,sample.bytes));
        }
        CHECK(intel_gpu_capture_release(&capture)==0 && !capture.self);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
static void vertex_capture_execution(void)
{
    for (unsigned mode=0;mode<8;mode++) {
        vertex_fill();OWNER_FIXTURE *b=&sample.render.base;CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        uint64_t id=0;CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,sample.bytes,sample.count,sample.patch_count,&id)==0);
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        memset((BYTE *)ctx->cpu.cpu+128,0xa5,sample.bytes);
        memset((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,0xa5,sample.count*sizeof(*sample.list));
        memset((BYTE *)ctx->cpu.cpu+ctx->patch_offset,0xa5,sample.patch_count*sizeof(*sample.patches));
        intel_gpu_cpu_access_t cpu={0};
        if (mode==1)((DWORD *)vertex_buffer->cpu)[1024]^=0x20;
        if (mode==2)CHECK(d3dkmt_destroy_allocations(7,n->device,1,&vertex_handle)==0);
        if (mode==3)CHECK(intel_gpu_buffer_acquire_cpu(vertex_buffer,7,&cpu)==0);
        if (mode==4 || mode==5)hw.delay=1000;
        if (mode==6)CHECK(intel_gpu_jobs_cancel(&b->jobs.jobs,7,id)==0);
        if (mode==7){vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
        for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        intel_bcs_context_t *native=&b->owner.slots[0].native;
        if(mode==3){
            CHECK(!b->jobs.done_count && !b->owner.engine.submitted_jobs && cpu.self);
            CHECK(intel_gpu_buffer_release_cpu(&cpu)==0);
            for(unsigned i=0;i<8 && !b->jobs.done_count;i++){executor_step();service_step();}
        }
        if(mode==4 || mode==5){
            CHECK(!b->jobs.done_count && b->owner.engine.inflight && native->vertex_program_access.self);
            CHECK(intel_gpu_buffer_acquire_cpu(vertex_buffer,7,&cpu)==INTEL_GPU_BUFFER_BUSY);
            if(mode==5){vm_model_exit(7);process_closing=true;d3dkmt_release_process(7);}
            hw.stall_stop=true;CHECK(intel_gpu_bcs_owner_close(&b->owner)==D3DKMT_STATUS_DEVICE_BUSY);
            CHECK(native->vertex_program_access.self && vertex_buffer->residents);
        } else if(mode==0 || mode==3){
            CHECK(b->jobs.done_count==1 && !b->jobs.done_status[0]);vertex_jobs++;
            const DWORD *batch=native->batch.cpu;
            CHECK(!memcmp(batch+0x1800/4,vertex_packet.values,vertex_packet.parameter_count*4));
            CHECK(!memcmp(batch+INTEL_RCS_VERTEX_CONSTANTS/4,vertex_packet.values+vertex_packet.parameter_count,vertex_packet.vertex_parameter_count*4));
            unsigned found=0;
            for(unsigned at=1;at<1024;){
                DWORD op=batch[at];if(op==0x05000000)break;
                if(!op){at++;continue;}
                if((op&0xffff0000u)==0x78100000u){CHECK(batch[at+1]==INTEL_GPU_VERTEX_PROGRAM_VA_OFFSET);CHECK(batch[at+7]&1);found++;}
                at+=(op&255)+2;
            }
            CHECK(found==1 && native->vertex_program.buffer==vertex_buffer);
        } else CHECK(b->jobs.done_count==1 && b->jobs.done_status[0]!=0 && !b->owner.engine.submitted_jobs);
        owner_cleanup(b);CHECK(ram.allocated==ram.freed);
    }
}
#ifndef NATIVE_VERTEX_CAPTURE_ENTRY
#define NATIVE_VERTEX_CAPTURE_ENTRY main
#endif
int NATIVE_VERTEX_CAPTURE_ENTRY(int argc,char **argv)
{
    CHECK(vertex_shader_sampled_baseline(argc,argv)==0);
    vertex_capture_negative();vertex_capture_execution();vertex_capture_capacity();
    printf("Native vertex shader capture PASS checks=%u completed_jobs=%u retained-VS-PS\n",checks,vertex_jobs);return 0;
}
