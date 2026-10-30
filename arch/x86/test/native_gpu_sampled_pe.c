/* Real public KMT sampled submissions. Original native compiler output;
 * expected solid-image sums and triangle coverage are a readback oracle. */
#define GPUCOPY_ROOT
#include "native_gpu_copy_pe.c"
#include "../lib/intel_d3d9/compiler_api.h"
#include "sampled-program.h"
#undef REQUIRE
#define REQUIRE(test,label) do { if (!(test)) { SAY("GPUSAMPLED FAIL " label "\n"); goto fail; } } while(0)
static intel_umd_draw_sampled_t sampled_packet;
static OKMT_ALLOCATION_INFO sampled_info[21];
static intel_umd_buffer_description_t sampled_descriptions[21];
static OKMT_HANDLE sampled_handles[21];
static OKMT_ALLOCATION_LIST sampled_list[19];
static OKMT_PATCH_LOCATION sampled_patches[19];

void native_gpu_sampled_entry(void)
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
    OKMT_ADAPTER_INFO adapters[16];OKMT_ENUM_ADAPTERS2 list={16,adapters};
    REQUIRE(enumerate(&list)==0 && list.count<=16,"enumerate");
    OKMT_HANDLE adapter=0;
    for (unsigned i=0;i<list.count;i++) {
        intel_umd_adapter_description_t info;
        OKMT_QUERY_ADAPTER_INFO q={adapters[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&info,sizeof(info)};
        if (!query(&q) && info.magic==INTEL_UMD_ADAPTER_MAGIC && info.vendor==0x8086 && info.device==0x3e9b &&
            info.copy_node==INTEL_UMD_NODE_RENDER) adapter=adapters[i].adapter;
    }
    if (!adapter) {SAY("GPUSAMPLED UNAVAILABLE native-prototype\n");ExitProcess(2);return;}
    OKMT_CREATE_DEVICE device;gpu_copy_zero(&device,sizeof(device));device.object.adapter=adapter;
    REQUIRE(create_device(&device)==0 && device.device,"device");
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,1,8192,64,64,0};
    OKMT_CREATE_CONTEXT context;gpu_copy_zero(&context,sizeof(context));
    context.device=device.device;context.node=INTEL_UMD_NODE_RENDER;context.private_data=&layout;context.private_bytes=sizeof(layout);
    REQUIRE(create_context(&context)==0 && context.context && context.command_bytes>=8192 &&
        context.allocation_count>=19 && context.patch_count>=19,"context");
    for (unsigned i=0;i<21;i++) {
        sampled_descriptions[i]=(intel_umd_buffer_description_t){INTEL_UMD_BUFFER_MAGIC,1,i==2 ? 8192 : 4096,0,0};
        sampled_info[i].private_data=&sampled_descriptions[i];sampled_info[i].private_bytes=sizeof(sampled_descriptions[i]);
    }
    OKMT_CREATE_ALLOCATION allocation;gpu_copy_zero(&allocation,sizeof(allocation));
    allocation.device=device.device;allocation.count=21;allocation.allocations=sampled_info;
    REQUIRE(allocate(&allocation)==0,"resources");
    for (unsigned i=0;i<21;i++) sampled_handles[i]=sampled_info[i].allocation;
    const unsigned vertices[]={
        0x3fa00000,0x3fa00000,0,0x3f800000,0x3f000000,0x3f000000,0,0x3f800000,
        0x41640000,0x3fa00000,0,0x3f800000,0x3f000000,0x3f000000,0,0x3f800000,
        0x3fa00000,0x41640000,0,0x3f800000,0x3f000000,0x3f000000,0,0x3f800000};
    for (unsigned i=0;i<21;i++) {
        OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
        access.allocation=sampled_handles[i];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
        REQUIRE(lock(&access)==0 && access.data,"upload-lock");
        gpu_copy_zero(access.data,(unsigned)sampled_descriptions[i].bytes);
        if (!i) gpu_copy_bytes(access.data,vertices,sizeof(vertices));
        else if (i==2) gpu_copy_bytes(access.data,sampled_program,sizeof(sampled_program));
        else if (i>=3 && i<20) {
            volatile unsigned *p=access.data;
            for (unsigned word=0;word<1024;word++) p[word]=i==19 ? 0xff020202u : 0xff010101u;
        }
        OKMT_UNLOCK release={device.device,1,&sampled_handles[i]};REQUIRE(unlock(&release)==0,"upload-unlock");
    }
    const struct intel_d3d9_program *program=(const void *)sampled_program;
    REQUIRE(program->texture_mask==0xffff && program->texcoord_mask==1 && program->code_offset==4096 &&
        program->allocation_bytes==sizeof(sampled_program) && !program->parameter_count,"program-contract");
    unsigned draws=0;
    for (unsigned mode=0;mode<4;mode++) for (unsigned dispatch=1;dispatch<8;dispatch++) {
        if (dispatch&~program->simd_mask) continue;
        unsigned bank=draws&1,target=bank ? 20 : 1,other=bank ? 1 : 20;
        for (unsigned i=0;i<2;i++) {
            unsigned index=i ? other : target;OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));
            access.device=device.device;access.allocation=sampled_handles[index];
            access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(lock(&access)==0 && access.data,"target-lock");
            volatile unsigned *p=access.data;for (unsigned word=0;word<1024;word++) p[word]=i ? 0x5a5a5a5a : 0xa6a6a6a6;
            OKMT_UNLOCK release={device.device,1,&sampled_handles[index]};REQUIRE(unlock(&release)==0,"target-unlock");
        }
        gpu_copy_zero(&sampled_packet,sizeof(sampled_packet));
        intel_umd_draw32_t *d=&sampled_packet.draw;
        d->magic=INTEL_UMD_COMMAND_MAGIC;d->version=INTEL_UMD_DRAW_SAMPLED_VERSION;d->opcode=INTEL_UMD_DRAW32;
        d->bytes=__builtin_offsetof(intel_umd_draw_sampled_t,values)+32;
        d->destination_width=d->destination_height=d->width=d->height=16;d->destination_pitch=64;
        d->vertex_stride=32;d->vertex_count=3;d->topology=4;d->program_bytes=sizeof(sampled_program);d->dispatch_mask=dispatch;
        d->reserved2[0]=INTEL_UMD_BLEND_VALID|INTEL_UMD_BLEND_WRITE_ALL;
        d->reserved2[1]=d->reserved2[2]=2|(1<<5)|(1<<10);
        sampled_packet.rectangle_count=2;sampled_packet.bound_mask=0xffff;sampled_packet.texcoord_offset[0]=16;
        intel_umd_rectangle_t *rect=(void *)sampled_packet.values;
        rect[0]=(intel_umd_rectangle_t){0,0,8,16};rect[1]=(intel_umd_rectangle_t){8,0,8,16};
        sampled_list[0]=(OKMT_ALLOCATION_LIST){sampled_handles[0],0};
        sampled_list[1]=(OKMT_ALLOCATION_LIST){sampled_handles[target],1};
        sampled_list[2]=(OKMT_ALLOCATION_LIST){sampled_handles[2],0};
        for (unsigned i=0;i<3;i++) sampled_patches[i]=(OKMT_PATCH_LOCATION){i,i,0,0,128+80+i*8,0};
        for (unsigned slot=0;slot<16;slot++) {
            unsigned resource=mode>=2 ? 3 : 3+slot;
            unsigned image=mode==3 || (mode==1 && !slot) ? 19 : mode>=2 ? 3 : 3+slot;
            sampled_list[resource]=(OKMT_ALLOCATION_LIST){sampled_handles[image],0};
            sampled_packet.textures[slot]=(intel_umd_texture_t){.width=4,.height=4,.levels=3,.format=21,.count=3,
                .sampler={1,1,0,3,3,3,0,0,0,0}};
            sampled_patches[3+slot]=(OKMT_PATCH_LOCATION){resource,3+slot,0,0,
                128+__builtin_offsetof(intel_umd_draw_sampled_t,textures)+slot*sizeof(intel_umd_texture_t),0};
        }
        unsigned count=mode>=2 ? 4 : 19;
        gpu_copy_bytes((unsigned char *)context.command_buffer+128,&sampled_packet,d->bytes);
        gpu_copy_bytes(context.allocation_list,sampled_list,count*sizeof(*sampled_list));
        gpu_copy_bytes(context.patch_list,sampled_patches,sizeof(sampled_patches));
        OKMT_RENDER submission;gpu_copy_zero(&submission,sizeof(submission));submission.context=context.context;
        submission.command_offset=128;submission.command_length=d->bytes;submission.allocation_count=count;submission.patch_count=19;
        REQUIRE(render(&submission)==0,"Render-admission");
        gpu_copy_zero((unsigned char *)context.command_buffer+128,d->bytes);
        gpu_copy_zero(context.allocation_list,count*sizeof(*sampled_list));gpu_copy_zero(context.patch_list,sizeof(sampled_patches));
        unsigned level=mode==1 ? 17 : mode==3 ? 32 : 16;
        unsigned color=0xff000000u|level|(level<<8)|(level<<16),equal=0;
        for (unsigned attempt=0;attempt<300 && !equal;attempt++) {
            Sleep(10);OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
            access.allocation=sampled_handles[target];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            OKMT_S32 status=lock(&access);if (status==GPUCOPY_ALLOCATION_BUSY) continue;
            REQUIRE(status==0 && access.data,"result-lock");equal=1;
            const volatile unsigned *p=access.data;
            for (unsigned word=0;word<1024;word++) {
                unsigned x=word%16,y=word/16,covered=x>=2 && y>=2 && x+y<=15;
                if (p[word]!=(covered ? color : 0xa6a6a6a6u)) equal=0;
            }
            OKMT_UNLOCK release={device.device,1,&sampled_handles[target]};REQUIRE(unlock(&release)==0,"result-unlock");
        }
        REQUIRE(equal,"GPU-sampled-pixels-and-padding");
        for (unsigned index=1;index<21;index++) if (index==other || (index>=3 && index<20)) {
            OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
            access.allocation=sampled_handles[index];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(lock(&access)==0 && access.data,"read-only-resource-lock");
            unsigned expected=index==other ? 0x5a5a5a5au : index==19 ? 0xff020202u : 0xff010101u;
            for (unsigned word=0;word<1024;word++) REQUIRE(((const volatile unsigned *)access.data)[word]==expected,"read-only-resource-and-padding");
            OKMT_UNLOCK release={device.device,1,&sampled_handles[index]};REQUIRE(unlock(&release)==0,"read-only-resource-unlock");
        }
        draws++;SAY("GPUSAMPLED PASS 16 samplers, target, inactive target and all image bytes\n");
    }
    REQUIRE(draws>=4,"draw-count");
    OKMT_DESTROY_CONTEXT close_context={context.context};REQUIRE(destroy_context(&close_context)==0,"destroy-context");
    OKMT_DESTROY_DEVICE close_device={device.device};REQUIRE(destroy_device(&close_device)==0,"destroy-device");
    SAY("GPUSAMPLED DONE failures=0\n");ExitProcess(0);return;
fail:
    ExitProcess(1);
}
