/* Native UMD constants, original WDK DDI; physical mode checks GPU pixels.
 * Fixture bytecode is assembled from our sources by the local MS compiler. */
static void uniform_verify(const void *,unsigned);
#define NATIVE_UMD_VERIFY_PROGRAM uniform_verify
#define native_umd_draw_entry unused_legacy_draw_entry
#include "native_umd_draw_pe.c"
#undef native_umd_draw_entry
#include <native_shader_fixtures.h>

static const void *uniform_codes[]={shader_fixture_0,shader_fixture_1,shader_fixture_2,
    shader_fixture_3,shader_fixture_4,shader_fixture_5,shader_fixture_6};
static const unsigned uniform_bytes[]={sizeof(shader_fixture_0),sizeof(shader_fixture_1),sizeof(shader_fixture_2),
    sizeof(shader_fixture_3),sizeof(shader_fixture_4),sizeof(shader_fixture_5),sizeof(shader_fixture_6)};
static const unsigned uniform_maps[][8]={{0,1,2,3},{892,893,894,895},{20,21,22,23,892,893,894,895},
    {4,5,6,7},{28,29,30,31},{40,41,42,43},{20,21,22,23,8,9,10,11}};
static unsigned uniform_index,uniform_values[224][4],uniform_draws;
static void uniform_verify(const void *memory,unsigned size)
{
    const BYTE *bytes=memory;
    const struct intel_d3d9_program *p=memory;
    unsigned count=uniform_index==2 || uniform_index==6 ? 8 : 4;
    CHECK(memory && size==8192 && p->abi==2 && p->header_bytes==128 && p->code_offset==4096);
    CHECK(p->bytecode_offset==128 && p->bytecode_bytes==uniform_bytes[uniform_index]);
    CHECK(p->parameter_count==count && p->curb_read_length==1 && p->simd_mask==3);
    CHECK(p->parameter_map_offset==128+p->bytecode_bytes && p->allocation_bytes==4096+p->code_bytes);
    for (unsigned i=0;i<p->bytecode_bytes;i++) CHECK(bytes[128+i]==((const BYTE *)uniform_codes[uniform_index])[i]);
    const unsigned *map=(const void *)(bytes+p->parameter_map_offset);
    for (unsigned i=0;i<count;i++) CHECK(map[i]==uniform_maps[uniform_index][i]);
    for (unsigned i=p->parameter_map_offset+count*4;i<4096;i++) CHECK(bytes[i]==0);
    for (unsigned i=p->allocation_bytes;i<size;i++) CHECK(bytes[i]==0);
}
static HRESULT APIENTRY uniform_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    unsigned count=uniform_index==2 || uniform_index==6 ? 8 : 4;
    CHECK(args->CommandLength==136+count*4 && args->NumAllocations==3 && args->NumPatchLocations==3);
#ifdef UMD_MODEL
    const intel_umd_draw_constants_t *q=(void *)model_commands;
    CHECK(q->draw.version==2 && q->draw.bytes==args->CommandLength && q->draw.opcode==2);
    CHECK(q->parameter_count==count && !q->reserved && !q->draw.vertices && !q->draw.destination && !q->draw.program);
    for (unsigned i=0;i<count;i++) {
        unsigned scalar=uniform_maps[uniform_index][i];
        CHECK(q->values[i]==uniform_values[scalar/4][scalar%4]);
    }
    for (unsigned i=0;i<3;i++) CHECK(model_patches[i].PatchOffset==80+i*8 && model_patches[i].SlotId==i);
    args->pNewCommandBuffer=model_commands; args->NewCommandBufferSize=sizeof(model_commands);
    args->pNewAllocationList=model_allocations; args->NewAllocationListSize=64;
    args->pNewPatchLocationList=model_patches; args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0}; q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength; q.allocation_count=3; q.patch_count=3;
    OKMT_S32 status=krender(&q);
    if (status==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands; args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations; args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches; args->NewPatchLocationListSize=q.patch_capacity;
#endif
    uniform_draws++; return S_OK;
}
static void uniform_pixels(HANDLE device,HANDLE target,unsigned color)
{
#ifndef UMD_MODEL
    unsigned equal=0,first=0;
    for (unsigned attempt=0;attempt<100 && !equal;attempt++) {
        Sleep(10); D3DDDIARG_LOCK access=draw_lock(device,target,1,0,0); CHECK(access.Pitch==64);
        const volatile unsigned *pixels=access.pSurfData; equal=1; first=pixels[3*16+3];
        for (unsigned i=0;i<256;i++) {
            unsigned x=i%16,y=i/16,covered=x>=2 && y>=2 && x+y<=15;
            if (pixels[i]!=(covered ? color : 0xa6a6a6a6u)) equal=0;
        }
        D3DDDIARG_UNLOCK unlock={target,0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    }
    if (!equal) {report("CONSTANT fixture=",uniform_index);report("CONSTANT wanted=",color);report("CONSTANT pixel=",first);}
    CHECK(equal);
#else
    (void)device;(void)target;(void)color;
#endif
}
static void uniform_set(HANDLE device,unsigned reg,unsigned x,unsigned y,unsigned z,unsigned w)
{
    uniform_values[reg][0]=x; uniform_values[reg][1]=y; uniform_values[reg][2]=z; uniform_values[reg][3]=w;
    D3DDDIARG_SETPIXELSHADERCONST args={reg,1};
    CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values[reg])==S_OK);
}
static void uniform_exercise(HANDLE device)
{
    CHECK(functions.pfnSetPixelShaderConst);
    D3DDDIARG_SETPIXELSHADERCONST args={224,0};
    CHECK(functions.pfnSetPixelShaderConst(device,&args,NULL)==S_OK);
    args.Count=1; CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values)==D3DERR_INVALIDCALL);
    args.Register=223; args.Count=2; CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values)==D3DERR_INVALIDCALL);
    args.Register=0; args.Count=1; CHECK(functions.pfnSetPixelShaderConst(device,&args,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetPixelShaderConst(device,NULL,(const FLOAT *)uniform_values)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetPixelShaderConst((HANDLE)0xdead,&args,(const FLOAT *)uniform_values)==D3DERR_INVALIDCALL);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    D3DDDIVERTEXELEMENT element={0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0};
    D3DDDIARG_CREATEVERTEXSHADERDECL decl={1,NULL};
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    HANDLE vertex=draw_resource(device,0xa000,TRUE),target=draw_resource(device,0xa001,FALSE);
    D3DDDIARG_LOCK access=draw_lock(device,vertex,2,0,0);
    const unsigned tri[3][4]={{0x3fa00000,0x3fa00000,0,0x3f800000},
        {0x41640000,0x3fa00000,0,0x3f800000},{0x3fa00000,0x41640000,0,0x3f800000}};
    memcpy(access.pSurfData,tri,sizeof(tri)); D3DDDIARG_UNLOCK unlock={vertex,0,{0}};
    CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,0,16}; CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0}; CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16}; CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    const unsigned one=0x3f800000,half=0x3f000000,quarter=0x3e800000,three=0x3f400000;
    static const unsigned colors[7][2]={{0xffff0000,0xff00ffff},{0xffff0000,0xff00ffff},
        {0xffffff00,0xff00ffff},{0x008040ff,0x00004000},{0xbfbf00bf,0xbf00bf00},
        {0x40ffbf80,0xffff0000},{0xff80ff40,0xff0000ff}};
    for (uniform_index=0;uniform_index<7;uniform_index++) {
        D3DDDIARG_CREATEPIXELSHADER shader={uniform_bytes[uniform_index],NULL};
        CHECK(functions.pfnCreatePixelShader(device,&shader,uniform_codes[uniform_index])==S_OK);
        CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
        for (unsigned round=0;round<2;round++) {
            memset(uniform_values,0,sizeof(uniform_values)); args=(D3DDDIARG_SETPIXELSHADERCONST){0,224};
            CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values)==S_OK);
            switch (uniform_index) {
            case 0: case 1: uniform_set(device,uniform_index ? 223 : 0,round ? 0 : one,round ? one : 0,round ? one : 0,one);break;
            case 2:
                uniform_set(device,5,round ? 0 : quarter,round ? quarter : 0,round ? half : 0,half);
                uniform_set(device,223,round ? 0 : three,round ? three : one,round ? half : 0,half);break;
            case 3: uniform_set(device,1,round ? 0 : one,one,round ? 0 : one,one);break;
            case 4: uniform_set(device,7,round ? 0 : quarter,round ? quarter : 0,round ? 0 : quarter,quarter);break;
            case 5: uniform_set(device,10,round ? one : quarter,round ? 0 : half,round ? 0 : three,one);break;
            case 6:
                uniform_set(device,5,round ? 0 : one,round ? 0 : half,0,0);
                uniform_set(device,2,0,0,round ? one : quarter,one);break;
            }
            /* A rejected range must not partially overwrite accepted state. */
            args=(D3DDDIARG_SETPIXELSHADERCONST){223,0xffffffff};
            CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values)==D3DERR_INVALIDCALL);
            access=draw_lock(device,target,2,0,0);
            for (unsigned i=0;i<256;i++) ((unsigned *)access.pSurfData)[i]=0xa6a6a6a6;
            unlock=(D3DDDIARG_UNLOCK){target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
            D3DDDIARG_DRAWPRIMITIVE draw={D3DPT_TRIANGLELIST,0,1};
            CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==S_OK);
            memset(uniform_values,0,sizeof(uniform_values));args=(D3DDDIARG_SETPIXELSHADERCONST){0,224};
            CHECK(functions.pfnSetPixelShaderConst(device,&args,(const FLOAT *)uniform_values)==S_OK);
            uniform_pixels(device,target,colors[uniform_index][round]);
        }
        CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
    }
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);CHECK(functions.pfnDestroyResource(device,target)==S_OK);
}

void native_pixel_constants_entry(void)
{
    SetErrorMode(0x8001);
    char path[512]; DWORD n=GetModuleFileNameA(NULL,path,sizeof(path)); CHECK(n && n<sizeof(path)-20);
    while(n && path[n-1]!='/' && path[n-1]!='\\') n--;
    const char name[]="osito-intel9.dll"; for (unsigned i=0;i<sizeof(name);i++) path[n+i]=name[i];
    HMODULE module=LoadLibraryA(path); CHECK(module);
    PFND3DDDI_OPENADAPTER open_adapter=(void *)GetProcAddress(module,"OpenAdapter"); CHECK(open_adapter);
#ifdef UMD_MODEL
    adapter=0x6001;
#else
    HMODULE gdi=LoadLibraryA("gdi32.dll"); CHECK(gdi);
    OKMT_S32 (WINAPI *enumerate)(OKMT_ENUM_ADAPTERS2 *)=(void *)GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (WINAPI *kdevice)(OKMT_CREATE_DEVICE *)=(void *)GetProcAddress(gdi,"D3DKMTCreateDevice");
    OKMT_S32 (WINAPI *kdestroy_device)(OKMT_DESTROY_DEVICE *)=(void *)GetProcAddress(gdi,"D3DKMTDestroyDevice");
    OKMT_S32 (WINAPI *kclose_adapter)(OKMT_CLOSE_ADAPTER *)=(void *)GetProcAddress(gdi,"D3DKMTCloseAdapter");
    LOAD(kquery,"D3DKMTQueryAdapterInfo"); LOAD(kcontext,"D3DKMTCreateContext"); LOAD(kdestroy_context,"D3DKMTDestroyContext");
    LOAD(kallocate,"D3DKMTCreateAllocation"); LOAD(kdeallocate,"D3DKMTDestroyAllocation");
    LOAD(klock,"D3DKMTLock"); LOAD(kunlock,"D3DKMTUnlock"); LOAD(krender,"D3DKMTRender");
    CHECK(enumerate && kdevice && kdestroy_device && kclose_adapter);
    OKMT_ADAPTER_INFO found[16]; OKMT_ENUM_ADAPTERS2 list={16,found}; CHECK(enumerate(&list)==0 && list.count<=16);
    for (unsigned i=0;i<list.count;i++) {
        intel_umd_adapter_description_t info={0};
        OKMT_QUERY_ADAPTER_INFO q={found[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&info,sizeof(info)};
        if (!kquery(&q) && info.magic==INTEL_UMD_ADAPTER_MAGIC && info.copy_node==0) adapter=found[i].adapter;
        else { OKMT_CLOSE_ADAPTER c={found[i].adapter}; CHECK(kclose_adapter(&c)==0); }
    }
    if (!adapter) {report("UNIFORM UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=draw_lock_callback;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=uniform_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    uniform_exercise(d.hDevice);
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==S_OK);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks && verified_programs==7);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    FreeLibrary(module);
    report("UNIFORM checks=",checks); report("UNIFORM draws=",uniform_draws); report("UNIFORM failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
