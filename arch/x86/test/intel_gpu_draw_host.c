/* Native KMT capture and persistent executor. Only hardware/VM are modeled. */
#define INTEL_RCS_OWNER_SUITE_ENTRY rcs_owner_suite_main
#include "intel_gpu_rcs_owner_host.c"
#include "native_umd_shader_reference.h"
typedef struct {
    RENDER_OWNER_FIXTURE render;
    OKMT_HANDLE allocations[2][3];
    intel_gpu_buffer_t *programs[2];
} DRAW_FIXTURE;
static void draw_fill(DRAW_FIXTURE *f,unsigned bank)
{
    CONTEXT_FIXTURE *n=&f->render.base.jobs.base.service.native;
    intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
    intel_umd_draw32_t command={.magic=INTEL_UMD_COMMAND_MAGIC,.version=INTEL_UMD_COMMAND_VERSION,
        .opcode=INTEL_UMD_DRAW32,.bytes=sizeof(command),.destination_width=16,.destination_height=16,
        .destination_pitch=64,.width=16,.height=16,.vertex_stride=16,.vertex_count=3,
        .topology=INTEL_RCS_TRIANGLE_LIST,.program_bytes=4096+sizeof(reference_native),.dispatch_mask=3};
    OKMT_ALLOCATION_LIST list[3]={{f->allocations[bank][0],0},{f->allocations[bank][1],1},{f->allocations[bank][2],0}};
    OKMT_PATCH_LOCATION patches[3]={{0,0,0,0,128+80,0},{1,1,0,0,128+88,0},{2,2,0,0,128+96,0}};
    memcpy((BYTE *)ctx->cpu.cpu+128,&command,sizeof(command));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,list,sizeof(list));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->patch_offset,patches,sizeof(patches));
}
static void draw_setup(DRAW_FIXTURE *f)
{
    memset(f,0,sizeof(*f)); OWNER_FIXTURE *b=&f->render.base;
    jobs_setup(&b->jobs); bcs_model_reset(); b->pat=0x03112233;
    f->render.cache[0]=0x11; f->render.cache[1]=0x22; f->render.cache[2]=0x33445566;
    b->jobs.jobs.ready=intel_gpu_executor_notify;
    CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
    CHECK(d3dkmt_destroy_context(7,b->jobs.base.context)==0); service_step();
    D3DKMT_CONTEXT_DESCRIPTION desc={.node=INTEL_UMD_NODE_RENDER,.private_bytes=sizeof(command_layout)};
    memcpy(desc.private_data,&command_layout,sizeof(command_layout));
    D3DKMT_CONTEXT_BUFFERS views;
    CHECK(d3dkmt_create_context(7,n->device,&desc,&b->jobs.base.context,&views)==0);
    CHECK(d3dkmt_destroy_allocations(7,n->device,2,b->jobs.base.allocations)==0); service_step();
    for (unsigned bank=0;bank<2;bank++) {
        D3DKMT_ALLOCATION_DESCRIPTION sizes[3]={{48},{1024},{4096+sizeof(reference_native)}};
        CHECK(d3dkmt_create_allocations(7,n->device,3,sizes,f->allocations[bank])==0);
        intel_gpu_kmt_cpu_lease_t cpu={0};
        CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,f->allocations[bank][2],&cpu)==0);
        f->programs[bank]=cpu.access.buffer;
        struct intel_d3d9_program p; memcpy(&p,reference_header,sizeof(p));
        p.code_offset=4096; p.allocation_bytes=4096+sizeof(reference_native);
        memcpy(cpu.access.cpu,&p,sizeof(p));
        const unsigned bytecode[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
            0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
        memcpy((BYTE *)cpu.access.cpu+sizeof(p),bytecode,sizeof(bytecode));
        memcpy((BYTE *)cpu.access.cpu+4096,reference_native,sizeof(reference_native));
        CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    }
    b->jobs.base.allocations[0]=f->allocations[0][0]; b->jobs.base.allocations[1]=f->allocations[0][1];
    const intel_ggtt_io_t ggtt_io={res_ggtt_read,res_ggtt_write,res_ggtt_sync};
    const intel_ggtt_layout_t layout={.ggtt_bytes=8ULL<<20,.gpu_va_bytes=1ULL<<32};
    CHECK(intel_ggtt_space_init(&b->space,&layout,1,63,f->render.bitmap,sizeof(f->render.bitmap),&ggtt_io,ggtt)==INTEL_GGTT_BIND_OK);
    intel_gpu_bcs_config_t config={.space=&b->space,.allocator=allocator,.allocator_context=&ram,
        .io={render_owner_read,render_owner_write,owner_now,owner_relax},.hardware_context=&f->render,
        .token_read=render_owner_token,.mapping_stopped=owner_mapping,.ggtt_limit=0x10000000,
        .source_gpu=0x10000000,.destination_gpu=0x20000000,.batch_gpu=0x30000000,.program_gpu=0x40000000,
        .render=true,.first_fence=0x11223344,.first_hardware_id=0x1001};
    CHECK(intel_gpu_bcs_owner_init(&b->owner,&b->jobs.jobs,&config)==0);
    CHECK(intel_gpu_bcs_owner_reserve(&b->owner,2,8)==0);
    CHECK(intel_gpu_bcs_owner_start(&b->owner)==0); draw_fill(f,0);
}
static void draw_capture_cases(void)
{
    for (unsigned mode=0;mode<33;mode++) {
        DRAW_FIXTURE f; draw_setup(&f); OWNER_FIXTURE *b=&f.render.base;
        CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        intel_umd_draw32_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        OKMT_ALLOCATION_LIST *list=(void *)((BYTE *)ctx->cpu.cpu+ctx->allocation_offset);
        OKMT_PATCH_LOCATION *patch=(void *)((BYTE *)ctx->cpu.cpu+ctx->patch_offset);
        unsigned count=3,patches=3,bytes=128;
        intel_gpu_cpu_access_t cpu={0};
        switch(mode) {
        case 0: command->destination_width=0;break;
        case 1: command->destination_height=4097;break;
        case 2: command->destination_pitch=63;break;
        case 3: command->destination_pitch=68;break;
        case 4: command->width=0;break;
        case 5: command->height=17;break;
        case 6: command->x=UINT32_MAX;break;
        case 7: command->y=1;break;
        case 8: command->vertex_stride=15;break;
        case 9: command->vertex_stride=20;break;
        case 10: command->vertex_offset=4;break;
        case 11: command->vertex_count=4;break;
        case 12: command->topology=3;break;
        case 13: command->program_bytes--;break;
        case 14: command->dispatch_mask=4;break;
        case 15: command->vertices=1;break;
        case 16: command->program=1;break;
        case 17: command->reserved2[5]=1;break;
        case 18: command->reserved1[1]=1;break;
        case 19: command->reserved0=1;break;
        case 20: count=2;break;
        case 21: patches=2;break;
        case 22: bytes=127;break;
        case 23: patch[2].allocation_index=3;break;
        case 24: patch[2].slot=1;break;
        case 25: patch[2].allocation_offset=4;break;
        case 26: patch[2].patch_offset=128+88;break;
        case 27: list[2].allocation=list[1].allocation;break;
        case 28: list[2].flags=1;break;
        case 29: ((uint32_t *)f.programs[0]->cpu)[1024]^=0x20;break;
        case 30: CHECK(intel_gpu_buffer_acquire_cpu(f.programs[0],7,&cpu)==INTEL_GPU_BUFFER_OK);break;
        case 31: ctx->node=INTEL_UMD_NODE_COPY;break;
        case 32: break;
        }
        intel_gpu_capture_t capture={0};
        NTSTATUS status=intel_gpu_capture_begin(7,b->jobs.base.context,128,bytes,count,patches,&capture);
        CHECK((status==0)==(mode==32));
        if (mode==30) { CHECK(status==D3DKMT_STATUS_DEVICE_BUSY); CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK); }
        CHECK(!capture.program_cpu.self);
        CHECK(intel_gpu_capture_release(&capture)==0);
        owner_cleanup(b); CHECK(ram.allocated==ram.freed);
    }
}
static void draw_cases(void)
{
    for (unsigned mode=0;mode<9;mode++) {
        DRAW_FIXTURE f; draw_setup(&f); OWNER_FIXTURE *b=&f.render.base;
        CONTEXT_FIXTURE *n=&b->jobs.base.service.native;
        intel_gpu_cpu_access_t cpu={0};
        for (unsigned round=0;round<(mode==0 ? 70u : 1u);round++) {
            unsigned bank=mode==0 ? round&1 : 0;
            b->jobs.done_count=0; draw_fill(&f,bank);
            uint64_t id;
            CHECK(intel_gpu_jobs_submit(&b->jobs.jobs,7,b->jobs.base.context,128,128,3,3,&id)==0);
            memset((BYTE *)n->contexts.contexts[0].cpu.cpu+128,0xa5,128);
            if (mode==1) ((uint32_t *)f.programs[0]->cpu)[1024]^=0x20;
            if (mode==2) ((uint32_t *)f.programs[0]->cpu)[9]=1;
            if (mode==3) CHECK(intel_gpu_buffer_acquire_cpu(f.programs[0],7,&cpu)==INTEL_GPU_BUFFER_OK);
            if (mode==4 || mode==8) hw.delay=1000;
            if (mode==5) CHECK(d3dkmt_destroy_allocations(7,n->device,1,&f.allocations[0][2])==0);
            if (mode==6) CHECK(intel_gpu_jobs_cancel(&b->jobs.jobs,7,id)==0);
            if (mode==7) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
            for (unsigned i=0;i<8 && !b->jobs.done_count;i++) { executor_step(); service_step(); }
            intel_bcs_context_t *native=&b->owner.slots[0].native;
            if (mode==3) {
                CHECK(!b->jobs.done_count && !b->owner.engine.submitted_jobs && cpu.self);
                CHECK(intel_gpu_buffer_release_cpu(&cpu)==INTEL_GPU_BUFFER_OK);
                for (unsigned i=0;i<8 && !b->jobs.done_count;i++) { executor_step(); service_step(); }
            }
            if (mode==4 || mode==8) {
                CHECK(!b->jobs.done_count && b->owner.engine.inflight && native->program_access.self);
                CHECK(intel_gpu_buffer_acquire_cpu(f.programs[0],7,&cpu)==INTEL_GPU_BUFFER_BUSY);
                if (mode==8) { vm_model_exit(7); process_closing=true; d3dkmt_release_process(7); }
                hw.stall_stop=true;
                CHECK(intel_gpu_bcs_owner_close(&b->owner)==D3DKMT_STATUS_DEVICE_BUSY);
                executor_step(); service_step();
                CHECK(!b->owner.engine.retired && native->program_access.self && f.programs[0]->live);
            } else {
                CHECK(b->jobs.done_count==1);
                bool valid=mode==0 || mode==3;
                CHECK((b->jobs.done_status[0]==0)==valid);
                CHECK(b->owner.engine.submitted_jobs==(valid ? round+1 : 0));
                if (valid) CHECK(native->program==f.programs[bank] && !native->program_access.self && !native->access_token);
            }
        }
        if (mode==0) CHECK(b->owner.engine.completed_jobs==70 && f.render.invalidations==69);
        owner_cleanup(b); CHECK(ram.allocated==ram.freed);
    }
}
#ifndef NATIVE_DRAW_SUITE_ENTRY
#define NATIVE_DRAW_SUITE_ENTRY main
#endif
int NATIVE_DRAW_SUITE_ENTRY(void)
{
    SERVICE_FIXTURE bootstrap;
    service_setup(&bootstrap); service_cleanup(&bootstrap,false);
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(intel_gpu_executor_start()==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(intel_gpu_executor_start()==0);
    unsigned before=checks;
    draw_capture_cases(); draw_cases();
    printf("Intel native KMT primitive work: %u checks passed\n",checks-before); return 0;
}
