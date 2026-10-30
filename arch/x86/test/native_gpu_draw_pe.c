/* PE client of public KMT, with native compiler output and an independent
 * integer coverage oracle. Does not provide a Direct3D runtime. */
#define GPUCOPY_ROOT
#include "native_gpu_copy_pe.c"
#include "../lib/intel_d3d9/compiler_api.h"
#include "native_umd_shader_reference.h"
#undef REQUIRE
#define REQUIRE(test,label) do { if (!(test)) { SAY("GPUDRAW FAIL " label "\n"); goto fail; } } while(0)

void native_gpu_draw_entry(void)
{
    void *gdi=LoadLibraryA("gdi32.dll");
    OKMT_S32 (CALL *enumerate)(OKMT_ENUM_ADAPTERS2 *)=GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (CALL *query)(OKMT_QUERY_ADAPTER_INFO *)=GetProcAddress(gdi,"D3DKMTQueryAdapterInfo");
    OKMT_S32 (CALL *create_device)(OKMT_CREATE_DEVICE *)=GetProcAddress(gdi,"D3DKMTCreateDevice");
    OKMT_S32 (CALL *create_context)(OKMT_CREATE_CONTEXT *)=GetProcAddress(gdi,"D3DKMTCreateContext");
    OKMT_S32 (CALL *allocate)(OKMT_CREATE_ALLOCATION *)=GetProcAddress(gdi,"D3DKMTCreateAllocation");
    OKMT_S32 (CALL *lock)(OKMT_LOCK *)=GetProcAddress(gdi,"D3DKMTLock");
    OKMT_S32 (CALL *unlock)(OKMT_UNLOCK *)=GetProcAddress(gdi,"D3DKMTUnlock");
    OKMT_S32 (CALL *render)(OKMT_RENDER *)=GetProcAddress(gdi,"D3DKMTRender");
    OKMT_S32 (CALL *destroy_context)(OKMT_DESTROY_CONTEXT *)=GetProcAddress(gdi,"D3DKMTDestroyContext");
    OKMT_S32 (CALL *destroy_device)(OKMT_DESTROY_DEVICE *)=GetProcAddress(gdi,"D3DKMTDestroyDevice");
    REQUIRE(gdi && enumerate && query && create_device && create_context && allocate && lock && unlock &&
        render && destroy_context && destroy_device,"public-exports");
    OKMT_ADAPTER_INFO adapters[16]; OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(enumerate(&list)==0 && list.count<=16,"enumerate");
    OKMT_HANDLE adapter=0;
    for (OKMT_U32 i=0;i<list.count;i++) {
        intel_umd_adapter_description_t info;
        OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&info,sizeof(info)};
        if (!query(&q) && info.magic==INTEL_UMD_ADAPTER_MAGIC && info.vendor==0x8086 && info.device==0x3e9b &&
            info.copy_node==INTEL_UMD_NODE_RENDER) adapter=adapters[i].adapter;
    }
    if (!adapter) { SAY("GPUDRAW UNAVAILABLE native-prototype\n"); ExitProcess(2); return; }
    OKMT_CREATE_DEVICE device; gpu_copy_zero(&device,sizeof(device)); device.object.adapter=adapter;
    REQUIRE(create_device(&device)==0 && device.device,"device");
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,4096,64,64,0};
    OKMT_CREATE_CONTEXT context; gpu_copy_zero(&context,sizeof(context));
    context.device=device.device; context.node=INTEL_UMD_NODE_RENDER;
    context.private_data=&layout; context.private_bytes=sizeof(layout);
    REQUIRE(create_context(&context)==0 && context.context && context.command_bytes>=256 &&
        context.allocation_count>=3 && context.patch_count>=3,"context");
    intel_umd_buffer_description_t descriptions[3]={
        {INTEL_UMD_BUFFER_MAGIC,1,48,0,0},{INTEL_UMD_BUFFER_MAGIC,1,1024,0,0},
        {INTEL_UMD_BUFFER_MAGIC,1,4096+sizeof(reference_native),0,0}};
    OKMT_ALLOCATION_INFO info[6]; OKMT_CREATE_ALLOCATION allocation;
    gpu_copy_zero(info,sizeof(info)); gpu_copy_zero(&allocation,sizeof(allocation));
    for (unsigned i=0;i<6;i++) { info[i].private_data=&descriptions[i%3]; info[i].private_bytes=sizeof(descriptions[0]); }
    allocation.device=device.device; allocation.count=6; allocation.allocations=info;
    REQUIRE(allocate(&allocation)==0,"resources");
    OKMT_HANDLE handles[6]; for (unsigned i=0;i<6;i++) handles[i]=info[i].allocation;
    const unsigned vertices[]={0x3fa00000,0x3fa00000,0,0x3f800000,
        0x41640000,0x3fa00000,0,0x3f800000,0x3fa00000,0x41640000,0,0x3f800000};
    const unsigned bytecode[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
        0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    for (unsigned i=0;i<6;i++) {
        OKMT_LOCK access; gpu_copy_zero(&access,sizeof(access));
        access.device=device.device; access.allocation=handles[i];
        access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
        REQUIRE(lock(&access)==0 && access.data,"upload-lock");
        gpu_copy_zero(access.data,(unsigned)descriptions[i%3].bytes);
        if (i%3==0) gpu_copy_bytes(access.data,vertices,sizeof(vertices));
        if (i%3==2) {
            struct intel_d3d9_program program;
            gpu_copy_bytes(&program,reference_header,sizeof(program));
            program.code_offset=4096; program.allocation_bytes=4096+sizeof(reference_native);
            gpu_copy_bytes(access.data,&program,sizeof(program));
            gpu_copy_bytes((unsigned char *)access.data+128,bytecode,sizeof(bytecode));
            gpu_copy_bytes((unsigned char *)access.data+4096,reference_native,sizeof(reference_native));
        }
        OKMT_UNLOCK release={device.device,1,&handles[i]}; REQUIRE(unlock(&release)==0,"upload-unlock");
    }
    for (unsigned round=0;round<6;round++) {
        unsigned bank=round&1;
        for (unsigned target=0;target<2;target++) {
            OKMT_LOCK access; gpu_copy_zero(&access,sizeof(access)); access.device=device.device;
            access.allocation=handles[target*3+1]; access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(lock(&access)==0 && access.data,"target-lock");
            volatile unsigned *p=access.data; for (unsigned i=0;i<256;i++) p[i]=target==bank ? 0xa6a6a6a6u : 0x5a5a5a5au;
            OKMT_UNLOCK release={device.device,1,&handles[target*3+1]}; REQUIRE(unlock(&release)==0,"target-unlock");
        }
        intel_umd_draw32_t command; gpu_copy_zero(&command,sizeof(command));
        command.magic=INTEL_UMD_COMMAND_MAGIC; command.version=1; command.opcode=INTEL_UMD_DRAW32; command.bytes=sizeof(command);
        command.destination_width=16; command.destination_height=16; command.destination_pitch=64;
        command.x=bank ? 4 : 0; command.y=bank ? 3 : 0; command.width=bank ? 7 : 16; command.height=bank ? 5 : 16;
        command.vertex_stride=16; command.vertex_count=3; command.topology=4;
        command.program_bytes=4096+sizeof(reference_native); command.dispatch_mask=1+round%3;
        OKMT_ALLOCATION_LIST resources[3]={{handles[bank*3],0},{handles[bank*3+1],1},{handles[bank*3+2],0}};
        OKMT_PATCH_LOCATION patches[3]={{0,0,0,0,208,0},{1,1,0,0,216,0},{2,2,0,0,224,0}};
        gpu_copy_bytes((unsigned char *)context.command_buffer+128,&command,sizeof(command));
        gpu_copy_bytes(context.allocation_list,resources,sizeof(resources)); gpu_copy_bytes(context.patch_list,patches,sizeof(patches));
        OKMT_RENDER submission; gpu_copy_zero(&submission,sizeof(submission)); submission.context=context.context;
        submission.command_offset=128; submission.command_length=sizeof(command); submission.allocation_count=3; submission.patch_count=3;
        REQUIRE(render(&submission)==0,"Render-admission");
        gpu_copy_zero((unsigned char *)context.command_buffer+128,sizeof(command));
        unsigned equal=0;
        for (unsigned attempt=0;attempt<300 && !equal;attempt++) {
            Sleep(10); OKMT_LOCK access; gpu_copy_zero(&access,sizeof(access));
            access.device=device.device; access.allocation=handles[bank*3+1];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            OKMT_S32 status=lock(&access); if (status==GPUCOPY_ALLOCATION_BUSY) continue;
            REQUIRE(status==0 && access.data,"result-lock"); equal=1;
            const volatile unsigned *p=access.data;
            for (unsigned i=0;i<256;i++) {
                unsigned x=i%16,y=i/16;
                /* Quarter-pixel vertices avoid integer edge ties. */
                unsigned covered=x>=2 && y>=2 && x+y<=15 && x>=command.x && y>=command.y &&
                    x<command.x+command.width && y<command.y+command.height;
                if (p[i]!=(covered ? 0xffff0000u : 0xa6a6a6a6u)) equal=0;
            }
            OKMT_UNLOCK release={device.device,1,&handles[bank*3+1]}; REQUIRE(unlock(&release)==0,"result-unlock");
        }
        REQUIRE(equal,"GPU-pixels-and-padding");
        OKMT_LOCK inactive; gpu_copy_zero(&inactive,sizeof(inactive)); inactive.device=device.device;
        inactive.allocation=handles[(bank^1)*3+1]; inactive.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
        REQUIRE(lock(&inactive)==0 && inactive.data,"inactive-lock");
        for (unsigned i=0;i<256;i++) REQUIRE(((const volatile unsigned *)inactive.data)[i]==0x5a5a5a5au,"inactive-target");
        OKMT_UNLOCK release={device.device,1,&handles[(bank^1)*3+1]}; REQUIRE(unlock(&release)==0,"inactive-unlock");
        SAY("GPUDRAW PASS pixels-and-inactive-target\n");
    }
    OKMT_DESTROY_CONTEXT close_context={context.context}; REQUIRE(destroy_context(&close_context)==0,"destroy-context");
    OKMT_DESTROY_DEVICE close_device={device.device}; REQUIRE(destroy_device(&close_device)==0,"destroy-device");
    SAY("GPUDRAW DONE draws=6 checked_pixels=3072 failures=0\n"); ExitProcess(0); return;
fail:
    ExitProcess(1);
}
