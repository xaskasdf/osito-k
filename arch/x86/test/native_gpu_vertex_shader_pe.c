/* Real public KMT VS2/VS3 submissions using owned native compiler output.
 * Expected texture sums and transformed rectangles are a readback oracle. */
#define GPUCOPY_ROOT
#include "native_gpu_copy_pe.c"
#include "../lib/intel_d3d9/compiler_api.h"
#include "sampled-program.h"
#include "vertex-program.h"
#undef REQUIRE
#define REQUIRE(test,label) do { if (!(test)) { SAY("GPUVERTEX FAIL " label "\n"); goto fail; } } while(0)
static intel_umd_draw_vertex_shader_t sampled_packet;
static OKMT_ALLOCATION_INFO sampled_info[23];
static intel_umd_buffer_description_t sampled_descriptions[23];
static OKMT_HANDLE sampled_handles[23];
static OKMT_ALLOCATION_LIST sampled_list[20];
static OKMT_PATCH_LOCATION sampled_patches[20];

void native_gpu_vertex_shader_entry(void)
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
    if (!adapter) {SAY("GPUVERTEX UNAVAILABLE native-prototype\n");ExitProcess(2);return;}
    OKMT_CREATE_DEVICE device;gpu_copy_zero(&device,sizeof(device));device.object.adapter=adapter;
    REQUIRE(create_device(&device)==0 && device.device,"device");
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,1,8192,64,64,0};
    OKMT_CREATE_CONTEXT context;gpu_copy_zero(&context,sizeof(context));
    context.device=device.device;context.node=INTEL_UMD_NODE_RENDER;context.private_data=&layout;context.private_bytes=sizeof(layout);
    REQUIRE(create_context(&context)==0 && context.context && context.command_bytes>=8192 &&
        context.allocation_count>=20 && context.patch_count>=20,"context");
    for (unsigned i=0;i<23;i++) {
        sampled_descriptions[i]=(intel_umd_buffer_description_t){INTEL_UMD_BUFFER_MAGIC,1,i==2 || i>=21 ? 8192 : 4096,0,0};
        sampled_info[i].private_data=&sampled_descriptions[i];sampled_info[i].private_bytes=sizeof(sampled_descriptions[i]);
    }
    OKMT_CREATE_ALLOCATION allocation;gpu_copy_zero(&allocation,sizeof(allocation));
    allocation.device=device.device;allocation.count=23;allocation.allocations=sampled_info;
    OKMT_S32 allocation_status=allocate(&allocation);
    if (allocation_status) {
        char code[9];OKMT_U32 written=0;
        for (unsigned i=0;i<8;i++)code[i]="0123456789abcdef"[((unsigned)allocation_status>>(28-i*4))&15];
        code[8]='\n';SAY("GPUVERTEX allocation status=");
        WriteFile(GetStdHandle((OKMT_U32)-11),code,sizeof(code),&written,0);
    }
    REQUIRE(allocation_status==0,"resources");
    for (unsigned i=0;i<23;i++) sampled_handles[i]=sampled_info[i].allocation;
    const unsigned vertices[]={
        0xbf800000,0x3f800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000,
        0x3f800000,0x3f800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000,
        0xbf800000,0xbf800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000,
        0xbf800000,0xbf800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000,
        0x3f800000,0x3f800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000,
        0x3f800000,0xbf800000,0x3f000000,0x3f800000,0x3f200000,0x3ec00000,0,0x3f800000};
    for (unsigned i=0;i<23;i++) {
        OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
        access.allocation=sampled_handles[i];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_WAIT;
        REQUIRE(lock(&access)==0 && access.data,"upload-lock");
        gpu_copy_zero(access.data,(unsigned)sampled_descriptions[i].bytes);
        if (!i) gpu_copy_bytes(access.data,vertices,sizeof(vertices));
        else if (i==2) gpu_copy_bytes(access.data,sampled_program,sizeof(sampled_program));
        else if (i==21) gpu_copy_bytes(access.data,vertex_program2,sizeof(vertex_program2));
        else if (i==22) gpu_copy_bytes(access.data,vertex_program3,sizeof(vertex_program3));
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
    for (unsigned profile=2;profile<=3;profile++) for (unsigned mode=0;mode<4;mode++) for (unsigned dispatch=1;dispatch<8;dispatch++) {
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
        d->magic=INTEL_UMD_COMMAND_MAGIC;d->version=INTEL_UMD_DRAW_VERTEX_SHADER_VERSION;d->opcode=INTEL_UMD_DRAW32;
        d->bytes=__builtin_offsetof(intel_umd_draw_vertex_shader_t,values)+32;
        d->destination_width=d->destination_height=d->width=d->height=16;d->destination_pitch=64;
        d->vertex_stride=32;d->vertex_count=6;d->topology=4;d->program_bytes=sizeof(sampled_program);d->dispatch_mask=dispatch;
        d->reserved2[0]=INTEL_UMD_BLEND_VALID|INTEL_UMD_BLEND_WRITE_ALL;
        d->reserved2[1]=d->reserved2[2]=2|(1<<5)|(1<<10);
        sampled_packet.rectangle_count=2;sampled_packet.bound_mask=0xffff;
        const struct intel_d3d9_vertex_program *vs=(const void *)(profile==2 ? vertex_program2 : vertex_program3);
        REQUIRE(vs->base.parameter_count==16 && vs->input_mask==0x8001 && vs->base.code_offset==4096,"vertex-program-contract");
        sampled_packet.vertex_program_bytes=vs->base.allocation_bytes;
        sampled_packet.vertex_parameter_count=vs->base.parameter_count;
        sampled_packet.viewport_width=sampled_packet.viewport_height=16;
        sampled_packet.attributes[0]=3u<<16;sampled_packet.attributes[15]=(3u<<16)|16;
        const unsigned *map=(const void *)((const unsigned char *)vs+vs->base.parameter_map_offset);
        unsigned matrix[16]={0x3f800000,0,0,0,0,0x3f800000,0,0,0,0,0x3f800000,0,0,0,0,0x3f800000};
        if (mode==1){matrix[0]=matrix[5]=0x3f000000;matrix[3]=0xbe800000;matrix[7]=0x3e800000;}
        if (mode==2){matrix[0]=matrix[5]=0x40000000;}
        if (mode==3){matrix[15]=0x40000000;}
        for (unsigned i=0;i<16;i++){REQUIRE(map[i]<16,"vertex-parameter-map");sampled_packet.values[i]=matrix[map[i]];}
        d->bytes+=16*4;
        intel_umd_rectangle_t *rect=(void *)(sampled_packet.values+16);
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
                128+__builtin_offsetof(intel_umd_draw_vertex_shader_t,textures)+slot*sizeof(intel_umd_texture_t),0};
        }
        unsigned vertex_index=mode>=2 ? 4 : 19;
        sampled_list[vertex_index]=(OKMT_ALLOCATION_LIST){sampled_handles[19+profile],0};
        sampled_patches[19]=(OKMT_PATCH_LOCATION){vertex_index,INTEL_UMD_VERTEX_PROGRAM_ROLE,0,0,
            128+__builtin_offsetof(intel_umd_draw_vertex_shader_t,vertex_program),0};
        unsigned count=vertex_index+1;
        gpu_copy_bytes((unsigned char *)context.command_buffer+128,&sampled_packet,d->bytes);
        gpu_copy_bytes(context.allocation_list,sampled_list,count*sizeof(*sampled_list));
        gpu_copy_bytes(context.patch_list,sampled_patches,sizeof(sampled_patches));
        OKMT_RENDER submission;gpu_copy_zero(&submission,sizeof(submission));submission.context=context.context;
        submission.command_offset=128;submission.command_length=d->bytes;submission.allocation_count=count;submission.patch_count=20;
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
                unsigned x=word%16,y=word/16;
                unsigned left=mode==1 ? 2 : mode==3 ? 4 : 0;
                unsigned extent=mode==1 || mode==3 ? 8 : 16;
                unsigned covered=x>=left && x<left+extent && y>=left && y<left+extent;
                if (p[word]!=(covered ? color : 0xa6a6a6a6u)) equal=0;
            }
            OKMT_UNLOCK release={device.device,1,&sampled_handles[target]};REQUIRE(unlock(&release)==0,"result-unlock");
        }
        REQUIRE(equal,"GPU-sampled-pixels-and-padding");
        for (unsigned index=1;index<23;index++) if (index==other || (index>=3 && index<20)) {
            OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
            access.allocation=sampled_handles[index];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(lock(&access)==0 && access.data,"read-only-resource-lock");
            unsigned expected=index==other ? 0x5a5a5a5au : index==19 ? 0xff020202u : 0xff010101u;
            for (unsigned word=0;word<1024;word++) REQUIRE(((const volatile unsigned *)access.data)[word]==expected,"read-only-resource-and-padding");
            OKMT_UNLOCK release={device.device,1,&sampled_handles[index]};REQUIRE(unlock(&release)==0,"read-only-resource-unlock");
        }
        for (unsigned index=0;index<23;index++) if (index==0 || index==2 || index>=21) {
            OKMT_LOCK access;gpu_copy_zero(&access,sizeof(access));access.device=device.device;
            access.allocation=sampled_handles[index];access.flags=OKMT_LOCK_ENTIRE|OKMT_LOCK_READ_ONLY|OKMT_LOCK_DO_NOT_WAIT;
            REQUIRE(lock(&access)==0 && access.data,"immutable-input-lock");
            const unsigned char *expected=index==0 ? (const void *)vertices : index==2 ? (const void *)sampled_program : index==21 ? vertex_program2 : vertex_program3;
            unsigned bytes=index==0 ? sizeof(vertices) : index==2 ? sizeof(sampled_program) : index==21 ? sizeof(vertex_program2) : sizeof(vertex_program3);
            for (unsigned byte=0;byte<sampled_descriptions[index].bytes;byte++)
                REQUIRE(((const volatile unsigned char *)access.data)[byte]==(byte<bytes ? expected[byte] : 0),"immutable-input-bytes-and-padding");
            OKMT_UNLOCK release={device.device,1,&sampled_handles[index]};REQUIRE(unlock(&release)==0,"immutable-input-unlock");
        }
        draws++;SAY("GPUVERTEX PASS 16 samplers, target, inactive target and all image bytes\n");
    }
    REQUIRE(draws==24,"draw-count");
    OKMT_DESTROY_CONTEXT close_context={context.context};REQUIRE(destroy_context(&close_context)==0,"destroy-context");
    OKMT_DESTROY_DEVICE close_device={device.device};REQUIRE(destroy_device(&close_device)==0,"destroy-device");
    SAY("GPUVERTEX DONE failures=0\n");ExitProcess(0);return;
fail:
    ExitProcess(1);
}
