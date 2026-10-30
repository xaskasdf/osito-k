#define NATIVE_SERVICE_SUITE_ENTRY service_suite_main
#include "intel_gpu_service_host.c"
#include "../drivers/intel_gpu_capture.h"

typedef struct {
    SERVICE_FIXTURE service;
    OKMT_HANDLE context,allocations[2];
    intel_gpu_capture_t capture;
} CAPTURE_FIXTURE;
static void capture_fill(CAPTURE_FIXTURE *f)
{
    intel_gpu_command_context_t *ctx=&f->service.native.contexts.contexts[0];
    intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,INTEL_UMD_COMMAND_VERSION,
        INTEL_UMD_COPY32,sizeof(command),16,8,128,128,0,0,{0}};
    OKMT_ALLOCATION_LIST allocations[2]={{f->allocations[0],0},{f->allocations[1],1}};
    OKMT_PATCH_LOCATION patches[2]={{0,0,0,0,160,0},{1,1,0,0,168,0}};
    memcpy((BYTE *)ctx->cpu.cpu+128,&command,sizeof(command));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->allocation_offset,allocations,sizeof(allocations));
    memcpy((BYTE *)ctx->cpu.cpu+ctx->patch_offset,patches,sizeof(patches));
}
static void capture_setup(CAPTURE_FIXTURE *f)
{
    memset(f,0,sizeof(*f)); service_setup(&f->service);
    CONTEXT_FIXTURE *n=&f->service.native;
    D3DKMT_CONTEXT_DESCRIPTION description={.node=INTEL_UMD_NODE_COPY,.private_bytes=sizeof(command_layout)};
    memcpy(description.private_data,&command_layout,sizeof(command_layout));
    D3DKMT_CONTEXT_BUFFERS buffers={0};
    CHECK(d3dkmt_create_context(7,n->device,&description,&f->context,&buffers)==0);
    D3DKMT_ALLOCATION_DESCRIPTION allocations[2]={{1000},{1000}};
    CHECK(d3dkmt_create_allocations(7,n->device,2,allocations,f->allocations)==0);
    capture_fill(f);
}
static void capture_cleanup(CAPTURE_FIXTURE *f)
{
    CHECK(intel_gpu_capture_release(&f->capture)==0 && !f->capture.self);
    CHECK(intel_gpu_capture_release(&f->capture)==0);
    service_cleanup(&f->service,false);
}
static void capture_cases(void)
{
    for (unsigned invalid=0;invalid<32;invalid++) {
        CAPTURE_FIXTURE f; capture_setup(&f);
        intel_gpu_command_context_t *ctx=&f.service.native.contexts.contexts[0];
        intel_umd_copy32_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        OKMT_ALLOCATION_LIST *allocations=(void *)((BYTE *)ctx->cpu.cpu+ctx->allocation_offset);
        OKMT_PATCH_LOCATION *patches=(void *)((BYTE *)ctx->cpu.cpu+ctx->patch_offset);
        DWORD offset=128,bytes=64,count=2,patch_count=2;
        switch (invalid) {
        case 0: command->magic=0; break;
        case 1: command->version=UINT32_MAX; break;
        case 2: command->opcode++; break;
        case 3: command->bytes--; break;
        case 4: command->width=0; break;
        case 5: command->height=0; break;
        case 6: command->width=UINT32_MAX; break;
        case 7: command->height=UINT32_MAX; break;
        case 8: command->source_pitch=127; break;
        case 9: command->destination_pitch=127; break;
        case 10: command->source_pitch=60; break;
        case 11: command->destination_pitch=60; break;
        case 12: command->source=0x100000; break;
        case 13: command->destination=0x200000; break;
        case 14: command->reserved[3]=1; break;
        case 15: command->height=9; break; /* fits rounded page, exceeds owned bytes */
        case 16: patches[0].allocation_index=2; break;
        case 17: patches[1].slot=0x1000001; break;
        case 18: patches[0].driver_id=1; break;
        case 19: patches[0].allocation_offset=4; break;
        case 20: patches[1].patch_offset=160; break;
        case 21: patches[1].split_offset=64; break;
        case 22: allocations[0].flags=1; break;
        case 23: allocations[1].flags=0; break;
        case 24: allocations[1].allocation=allocations[0].allocation; break;
        case 25: allocations[1].allocation=0xfeed; break;
        case 26: offset=UINT32_MAX-7; break;
        case 27: offset=129; break;
        case 28: bytes=63; break;
        case 29: count=3; break;
        case 30: patch_count=1; break;
        case 31: ctx->node=2; break;
        }
        CHECK(intel_gpu_capture_begin(7,f.context,offset,bytes,count,patch_count,&f.capture)!=0);
        CHECK(f.capture.self && !f.capture.valid && !ram.freed);
        intel_bcs_copy_t copy={.source=0xfeed}; intel_bcs_context_t engine={.ready=true};
        CHECK(intel_gpu_capture_copy(&f.capture,&engine,&copy)==STATUS_INVALID_PARAMETER && copy.source==0xfeed);
        capture_cleanup(&f);
    }
    for (unsigned round=0;round<4;round++) {
        CAPTURE_FIXTURE f; capture_setup(&f);
        CONTEXT_FIXTURE *n=&f.service.native;
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        OKMT_ALLOCATION_LIST *allocations=(void *)((BYTE *)ctx->cpu.cpu+ctx->allocation_offset);
        OKMT_PATCH_LOCATION *patches=(void *)((BYTE *)ctx->cpu.cpu+ctx->patch_offset);
        if (round&1) { OKMT_PATCH_LOCATION temp=patches[0]; patches[0]=patches[1]; patches[1]=temp; }
        if (round&2) {
            OKMT_ALLOCATION_LIST temp=allocations[0]; allocations[0]=allocations[1]; allocations[1]=temp;
            patches[0].allocation_index^=1; patches[1].allocation_index^=1;
        }
        CHECK(intel_gpu_capture_begin(8,f.context,128,64,2,2,&f.capture)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_capture_release(&f.capture)==0);
        CHECK(intel_gpu_capture_begin(7,f.context,128,64,2,2,&f.capture)==0);
        CHECK(f.capture.valid && f.capture.resources[0].reference.handle==f.allocations[0] &&
            f.capture.resources[1].reference.handle==f.allocations[1]);
        intel_gpu_capture_t copied=f.capture;
        CHECK(intel_gpu_capture_release(&copied)==STATUS_INVALID_PARAMETER);
        CHECK(intel_gpu_capture_begin(7,f.context,128,64,2,2,&f.capture)==STATUS_INVALID_PARAMETER);
        intel_gpu_kmt_cpu_lease_t cpu={0};
        CHECK(intel_gpu_kmt_acquire_cpu(7,n->device,f.allocations[0],&cpu)==0);
        intel_gpu_access_t gpu={0};
        CHECK(intel_gpu_buffer_acquire_gpu(f.capture.resources[0].native.buffer,7,&gpu)==INTEL_GPU_BUFFER_BUSY);
        CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
        CHECK(intel_gpu_buffer_acquire_gpu(f.capture.resources[0].native.buffer,7,&gpu)==INTEL_GPU_BUFFER_OK);
        CHECK(intel_gpu_buffer_release_gpu(&gpu)==INTEL_GPU_BUFFER_OK);
        memset((BYTE *)ctx->cpu.cpu+128,0xff,64); memset(allocations,0xff,16); memset(patches,0xff,48);
        intel_bcs_context_t engine={.ready=true,.owner=7,.source_gpu=0x10000000,.destination_gpu=0x20000000,
            .source=f.capture.resources[0].native.buffer,.destination=f.capture.resources[1].native.buffer};
        intel_bcs_copy_t copy={0};
        CHECK(intel_gpu_capture_copy(&f.capture,&engine,&copy)==0);
        CHECK(copy.source==engine.source_gpu && copy.destination==engine.destination_gpu &&
            copy.source_bytes==1000 && copy.destination_bytes==1000 && copy.width==16 && copy.height==8);
        engine.source=engine.destination;
        CHECK(intel_gpu_capture_copy(&f.capture,&engine,&copy)==STATUS_INVALID_PARAMETER);
        CHECK(d3dkmt_destroy_device(7,n->device)==0);
        service_step(); CHECK(!ram.freed && n->memory.pages==4);
        intel_gpu_buffer_t *held=f.capture.resources[0].native.buffer;
        __atomic_store_n(&held->busy,1,__ATOMIC_RELEASE);
        CHECK(intel_gpu_capture_release(&f.capture)==D3DKMT_STATUS_DEVICE_BUSY && f.capture.self && !f.capture.valid);
        service_step(); CHECK(n->memory.pages>=3);
        __atomic_store_n(&held->busy,0,__ATOMIC_RELEASE);
        capture_cleanup(&f);
    }
}
typedef struct { OKMT_HANDLE device,allocation; NTSTATUS status; } CAPTURE_CPU_RACE;
static void *capture_cpu_thread(void *argument)
{
    CAPTURE_CPU_RACE *race=argument;
    intel_gpu_kmt_cpu_lease_t cpu={0};
    race->status=intel_gpu_kmt_acquire_cpu(7,race->device,race->allocation,&cpu);
    if (race->status==0) CHECK(intel_gpu_kmt_release_cpu(&cpu)==0);
    CHECK(!cpu.reference.self && !cpu.access.self);
    return NULL;
}
static void capture_races(void)
{
    for (unsigned round=0;round<32;round++) {
        CAPTURE_FIXTURE f; capture_setup(&f);
        CHECK(intel_gpu_capture_begin(7,f.context,128,64,2,2,&f.capture)==0);
        CAPTURE_CPU_RACE race={f.service.native.device,f.allocations[0],0};
        ram.hold=1;
        pthread_t thread; CHECK(!pthread_create(&thread,NULL,capture_cpu_thread,&race));
        while (!__atomic_load_n(&ram.entered,__ATOMIC_ACQUIRE)) sched_yield();
        if (round&1) { vm_model_exit(7); intel_gpu_memory_release_process(7); d3dkmt_release_process(7); }
        else CHECK(d3dkmt_destroy_device(7,f.service.native.device)==0);
        CHECK(intel_gpu_capture_release(&f.capture)==D3DKMT_STATUS_DEVICE_BUSY && f.capture.context.native.self);
        CHECK(intel_gpu_service_detach(f.service.ticket)==D3DKMT_STATUS_DEVICE_BUSY);
        __atomic_store_n(&ram.resume,1,__ATOMIC_RELEASE);
        CHECK(!pthread_join(thread,NULL)); ram.hold=0;
        CHECK(race.status==((round&1) ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS));
        capture_cleanup(&f);
    }
}
static void capture_rectangles(void)
{
    for (unsigned render=0;render<2;render++) for (unsigned bad=0;bad<18;bad++) {
        CAPTURE_FIXTURE f;capture_setup(&f);
        CONTEXT_FIXTURE *n=&f.service.native;
        intel_gpu_command_context_t *ctx=&n->contexts.contexts[0];
        if (render) ctx->node=INTEL_UMD_NODE_RENDER;
        intel_umd_copy32_t *command=(void *)((BYTE *)ctx->cpu.cpu+128);
        command->version=INTEL_UMD_COPY_RECT_VERSION;command->width=5;command->height=3;
        command->reserved[0]=3;command->reserved[1]=2;
        command->reserved[2]=7;command->reserved[3]=4;
        switch (bad) {
        case 1:command->reserved[0]=UINT32_MAX;break;
        case 2:command->reserved[1]=UINT32_MAX;break;
        case 3:command->reserved[2]=UINT32_MAX;break;
        case 4:command->reserved[3]=UINT32_MAX;break;
        case 5:command->reserved[0]=28;break;
        case 6:command->reserved[2]=28;break;
        case 7:command->reserved[1]=32765;break;
        case 8:command->reserved[3]=32765;break;
        case 9:command->reserved[1]=6;break; /* exceeds logical 1000, fits backing page */
        case 10:command->reserved[3]=6;break;
        case 11:command->version=INTEL_UMD_COMMAND_VERSION;break;
        case 12:command->source=0x10000;break;
        case 13:command->destination=0x20000;break;
        case 14:command->width=UINT32_MAX;break;
        case 15:command->height=UINT32_MAX;break;
        case 16:command->source_pitch=0;break;
        case 17:command->destination_pitch=0;break;
        }
        NTSTATUS status=intel_gpu_capture_begin(7,f.context,128,64,2,2,&f.capture);
        CHECK(bad ? status!=0 : status==0);
        if (!bad) {
            /* Subsequent writes cannot change the queued command or its handles. */
            memset(command,0xff,sizeof(*command));
            intel_bcs_context_t engine={.ready=true,.owner=7,.render=render,
                .source_gpu=0x10000000,.destination_gpu=0x20000000,
                .source=f.capture.resources[0].native.buffer,.destination=f.capture.resources[1].native.buffer};
            intel_bcs_copy_t copy={0};
            CHECK(intel_gpu_capture_copy(&f.capture,&engine,&copy)==0);
            CHECK(copy.source_x==3 && copy.source_y==2 && copy.destination_x==7 && copy.destination_y==4 &&
                copy.source_bytes==1000 && copy.destination_bytes==1000 && copy.width==5 && copy.height==3);
            CHECK(d3dkmt_destroy_device(7,n->device)==0);
            service_step();CHECK(!ram.freed && n->memory.pages==4);
        }
        capture_cleanup(&f);
    }
}
#ifndef NATIVE_CAPTURE_SUITE_ENTRY
#define NATIVE_CAPTURE_SUITE_ENTRY main
#endif
int NATIVE_CAPTURE_SUITE_ENTRY(void)
{
    if (service_suite_main()) return 1;
    unsigned before=checks;
    capture_cases(); capture_races(); capture_rectangles();
    printf("Intel native command capture: %u checks passed, 32 forced CPU/close races\n",checks-before);
    return 0;
}
