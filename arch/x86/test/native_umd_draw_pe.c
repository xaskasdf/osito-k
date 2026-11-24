/* Real D3D9 driver callbacks. Model builds verify packets and ownership only;
 * physical builds compare GPU output with an independent integer oracle. */
#define native_umd_entry native_umd_copy_entry
#include "native_umd_pe.c"
#undef native_umd_entry

static unsigned draw_count;
static unsigned resource_null_mapping;
static intel_umd_draw32_t expected_draw;
#ifdef UMD_MODEL
static unsigned char draw_commands[2][4096];
static D3DDDI_ALLOCATIONLIST draw_allocations[2][64];
static D3DDDI_PATCHLOCATIONLIST draw_patches[2][64];
static void *current_commands=model_commands;
static D3DDDI_ALLOCATIONLIST *current_allocations=model_allocations;
static D3DDDI_PATCHLOCATIONLIST *current_patches=model_patches;
#endif

static HRESULT APIENTRY draw_lock_callback(HANDLE h,D3DDDICB_LOCK *args)
{
    HRESULT result=lock_allocation(h,args);
    if (SUCCEEDED(result) && resource_null_mapping) {
        resource_null_mapping=0; args->pData=NULL;
    }
    return result;
}

static HRESULT APIENTRY draw_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    CHECK(args->CommandLength==128 && args->NumAllocations==3 && args->NumPatchLocations==3 &&
          !args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    const unsigned char *packet=current_commands,*want=(void *)&expected_draw;
    for (unsigned i=0;i<sizeof(expected_draw);i++) CHECK(packet[i]==want[i]);
    unsigned slots[3];
    for (unsigned i=0;i<3;i++) {
        D3DDDI_PATCHLOCATIONLIST *p=&current_patches[i];
        CHECK(p->AllocationIndex==i && p->SlotId==i && p->PatchOffset==80+8*i &&
              !p->DriverId && !p->AllocationOffset && !p->SplitOffset);
        CHECK(current_allocations[i].Value==(i==1));
        unsigned slot=0; while(slot<8 && backing[slot].allocation!=current_allocations[i].hAllocation) slot++;
        CHECK(slot<8 && backing[slot].allocation && !backing[slot].locked); slots[i]=slot;
    }
    CHECK(slots[0]!=slots[1] && slots[0]!=slots[2] && slots[1]!=slots[2]);
    CHECK(backing[slots[0]].bytes==256 && backing[slots[1]].bytes==4096 && backing[slots[2]].bytes==8192);
    /* Return different staging buffers so the next call must use replacements. */
    unsigned next=draw_count&1;
    current_commands=draw_commands[next]; current_allocations=draw_allocations[next]; current_patches=draw_patches[next];
    memset(current_commands,0xa5,4096); memset(current_allocations,0xa5,sizeof(draw_allocations[0]));
    memset(current_patches,0xa5,sizeof(draw_patches[0]));
    args->pNewCommandBuffer=current_commands; args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations; args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches; args->NewPatchLocationListSize=64;
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
    draw_count++; return S_OK;
}
static HANDLE draw_resource(HANDLE device,unsigned id,BOOL vertex)
{
    D3DDDI_SURFACEINFO surface={vertex ? 256 : 16,vertex ? 1 : 16,1,NULL,0,0};
    D3DDDIARG_CREATERESOURCE r={0}; r.hResource=(HANDLE)(UINT_PTR)id;
    r.Format=vertex ? D3DDDIFMT_VERTEXDATA : D3DDDIFMT_A8R8G8B8;
    r.Pool=D3DDDIPOOL_VIDEOMEMORY; r.pSurfList=&surface; r.SurfCount=1; r.MipLevels=1;
    r.Rotation=D3DDDI_ROTATION_IDENTITY;
    if (vertex) r.Flags.VertexBuffer=1; else r.Flags.RenderTarget=1;
    surface.Width=0; CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);
    surface.Width=vertex ? 256 : 16;
    callback_fault=4; CHECK(functions.pfnCreateResource(device,&r)==E_ABORT && r.hResource==(HANDLE)(UINT_PTR)id);
    CHECK(functions.pfnCreateResource(device,&r)==S_OK); return r.hResource;
}
static D3DDDIARG_LOCK draw_lock(HANDLE device,HANDLE resource,UINT flags,UINT offset,UINT bytes)
{
    D3DDDIARG_LOCK access={0}; access.hResource=resource; access.Flags.Value=flags|0x200;
    if (bytes) {access.Flags.RangeValid=1;access.Range.Offset=offset;access.Range.Size=bytes;}
    HRESULT status=D3DERR_WASSTILLDRAWING;
    for (unsigned i=0;i<300 && status==D3DERR_WASSTILLDRAWING;i++) {
        status=functions.pfnLock(device,&access);
        if (status==D3DERR_WASSTILLDRAWING) Sleep(10);
    }
    CHECK(status==S_OK && access.pSurfData); return access;
}
static void draw_state(HANDLE device,D3DDDIRENDERSTATETYPE state,UINT value)
{
    D3DDDIARG_RENDERSTATE s={state,value}; CHECK(functions.pfnSetRenderState(device,&s)==S_OK);
}
static void draw_check_pixels(HANDLE device,HANDLE target,HANDLE inactive,BOOL square)
{
#ifndef UMD_MODEL
    unsigned equal=0;
    for (unsigned attempt=0;attempt<300 && !equal;attempt++) {
        Sleep(10); D3DDDIARG_LOCK access=draw_lock(device,target,1,0,0);
        CHECK(access.Pitch==64); const volatile unsigned *pixels=access.pSurfData; equal=1;
        for (unsigned i=0;i<256;i++) {
            unsigned x=i%16,y=i/16;
            unsigned covered=x>=2 && y>=2 && (square ? x<=14 && y<=14 : x+y<=15);
            covered &= x>=expected_draw.x && y>=expected_draw.y &&
                x<expected_draw.x+expected_draw.width && y<expected_draw.y+expected_draw.height;
            if (pixels[i]!=(covered ? 0xffff0000u : 0xa6a6a6a6u)) equal=0;
        }
        D3DDDIARG_UNLOCK unlock={target,0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    }
    CHECK(equal);
    D3DDDIARG_LOCK access=draw_lock(device,inactive,1,0,0);
    for (unsigned i=0;i<256;i++) CHECK(((volatile unsigned *)access.pSurfData)[i]==0x5a5a5a5au);
    D3DDDIARG_UNLOCK unlock={inactive,0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
#else
    (void)device; (void)target; (void)inactive; (void)square;
#endif
}
static void draw_exercise(HANDLE device,HANDLE adapter_handle,const D3DDDI_ADAPTERFUNCS *af,
                          const D3DDDIARG_CREATEDEVICE *primary)
{
    CHECK(functions.pfnDrawPrimitive && functions.pfnDrawPrimitive2 && functions.pfnCreateVertexShaderDecl &&
          functions.pfnSetStreamSource && functions.pfnSetRenderTarget && functions.pfnSetViewport &&
          functions.pfnSetScissorRect && functions.pfnSetRenderState);
    D3DDDIARG_DRAWPRIMITIVE primitive={D3DPT_TRIANGLELIST,1,1};
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_NOTAVAILABLE);
    draw_state(device,D3DDDIRS_ZENABLE,0); draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    draw_state(device,D3DDDIRS_FILLMODE,D3DFILL_SOLID); draw_state(device,D3DDDIRS_COLORWRITEENABLE,15);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    D3DDDIARG_RENDERSTATE unsupported={D3DDDIRS_ALPHATESTENABLE,1};
    CHECK(functions.pfnSetRenderState(device,&unsupported)==D3DERR_NOTAVAILABLE);
    D3DDDIVERTEXELEMENT element={0,4,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0};
    D3DDDIARG_CREATEVERTEXSHADERDECL decl={1,(HANDLE)0xdead};
    element.Usage=D3DDECLUSAGE_POSITION;
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    element.Usage=D3DDECLUSAGE_POSITIONT; decl.NumVertexElements=2;
    D3DDDIVERTEXELEMENT duplicate[2]={element,element};
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,duplicate)==D3DERR_NOTAVAILABLE);
    decl.NumVertexElements=1; CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    D3DDDI_DEVICEFUNCS second_funcs; D3DDDIARG_CREATEDEVICE second=*primary;
    second.hDevice=runtime_device; second.pDeviceFuncs=&second_funcs;
    CHECK(af->pfnCreateDevice(adapter_handle,&second)==S_OK);
    CHECK(second_funcs.pfnSetVertexShaderDecl(second.hDevice,decl.ShaderHandle)==D3DERR_INVALIDCALL);
    CHECK(second_funcs.pfnDeleteVertexShaderDecl(second.hDevice,decl.ShaderHandle)==D3DERR_INVALIDCALL);
    CHECK(second_funcs.pfnDestroyDevice(second.hDevice)==S_OK);
    HANDLE vertex[2],target[2];
    for (unsigned i=0;i<2;i++) {vertex[i]=draw_resource(device,0x9000+i,TRUE);target[i]=draw_resource(device,0x9100+i,FALSE);}
    D3DDDIARG_LOCK missing={0}; missing.hResource=vertex[0]; missing.Flags.DoNotWait=1;
    resource_null_mapping=1; callback_fault=7;
    CHECK(functions.pfnLock(device,&missing)==(HRESULT)0x88760827 && !missing.pSurfData);
    callback_fault=7; CHECK(functions.pfnLock(device,&missing)==E_ABORT && !missing.pSurfData);
    CHECK(functions.pfnLock(device,&missing)==S_OK && missing.pSurfData);
    D3DDDIARG_UNLOCK recovered={vertex[0],0,{0}}; CHECK(functions.pfnUnlock(device,&recovered)==S_OK);
    D3DDDIARG_SETSTREAMSOURCE stream={0,target[0],16,20};
    CHECK(functions.pfnSetStreamSource(device,&stream)==D3DERR_INVALIDCALL);
    stream.hVertexBuffer=vertex[0]; stream.Stride=0;
    CHECK(functions.pfnSetStreamSource(device,&stream)==D3DERR_NOTAVAILABLE);
    stream.Stride=20; stream.Offset=257; CHECK(functions.pfnSetStreamSource(device,&stream)==D3DERR_INVALIDCALL);
    D3DDDIARG_LOCK invalid={0}; invalid.hResource=vertex[0]; invalid.Flags.RangeValid=1;
    invalid.Range.Offset=250; invalid.Range.Size=16; CHECK(functions.pfnLock(device,&invalid)==D3DERR_INVALIDCALL);
    D3DDDIARG_SETRENDERTARGET rt={0,vertex[0],0}; CHECK(functions.pfnSetRenderTarget(device,&rt)==D3DERR_INVALIDCALL);
    const unsigned code[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
                           0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    D3DDDIARG_CREATEPIXELSHADER shader={sizeof(code),NULL};
    CHECK(functions.pfnCreatePixelShader(device,&shader,code)==S_OK);
    CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
    for (unsigned round=0;round<6;round++) {
        unsigned bank=round&1,square=round>=2;
        stream=(D3DDDIARG_SETSTREAMSOURCE){0,vertex[bank],16,20};
        CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
        rt=(D3DDDIARG_SETRENDERTARGET){0,target[bank],0}; CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
        D3DDDIARG_VIEWPORTINFO viewport=round<4 ? (D3DDDIARG_VIEWPORTINFO){0,0,16,16} : (D3DDDIARG_VIEWPORTINFO){3,2,8,9};
        CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
        RECT rect={4,3,11,8}; CHECK(functions.pfnSetScissorRect(device,&rect)==S_OK);
        draw_state(device,D3DDDIRS_SCISSORTESTENABLE,bank);
        D3DDDIARG_LOCK access=draw_lock(device,vertex[bank],2,40,square ? 76 : 56);
        CHECK(access.Pitch==256);
        static const unsigned tri[4][4]={{0x3fa00000,0x3fa00000,0,0x3f800000},{0x41640000,0x3fa00000,0,0x3f800000},
            {0x3fa00000,0x41640000,0,0x3f800000},{0x41640000,0x41640000,0,0x3f800000}};
        for (unsigned i=0;i<(square ? 4u : 3u);i++) {
            unsigned index=round>=4 && i>=2 ? 5-i : i;
            memcpy((BYTE *)access.pSurfData+i*20,tri[index],16);
        }
        CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
        D3DDDIARG_UNLOCK unlock={vertex[bank],0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        for (unsigned j=0;j<2;j++) {
            access=draw_lock(device,target[j],2,0,0);
            for (unsigned i=0;i<256;i++) ((unsigned *)access.pSurfData)[i]=j==bank ? 0xa6a6a6a6u : 0x5a5a5a5au;
            unlock=(D3DDDIARG_UNLOCK){target[j],0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        }
        primitive=(D3DDDIARG_DRAWPRIMITIVE){(D3DPRIMITIVETYPE)(D3DPT_TRIANGLELIST+round/2),1,square ? 2 : 1};
        D3DDDIARG_DRAWPRIMITIVE bad=primitive; bad.VStart=0xffffffff;
        CHECK(functions.pfnDrawPrimitive(device,&bad,NULL)==D3DERR_INVALIDCALL);
        bad=primitive; bad.PrimitiveCount=0xffffffff;
        CHECK(functions.pfnDrawPrimitive(device,&bad,NULL)==D3DERR_INVALIDCALL);
        unsigned flags=7; CHECK(functions.pfnDrawPrimitive(device,&primitive,&flags)==D3DERR_NOTAVAILABLE);
        expected_draw=(intel_umd_draw32_t){0}; expected_draw.magic=INTEL_UMD_COMMAND_MAGIC; expected_draw.version=1;
        expected_draw.opcode=INTEL_UMD_DRAW32; expected_draw.bytes=128;
        expected_draw.destination_width=16; expected_draw.destination_height=16; expected_draw.destination_pitch=64;
        expected_draw.x=bank ? 4 : viewport.X; expected_draw.y=bank ? 3 : viewport.Y;
        expected_draw.width=bank ? 7 : viewport.Width; expected_draw.height=bank ? 5 : viewport.Height;
        expected_draw.vertex_offset=40; expected_draw.vertex_stride=20; expected_draw.vertex_count=square ? 4 : 3;
        expected_draw.topology=primitive.PrimitiveType; expected_draw.program_bytes=4096+sizeof(reference_native);
        expected_draw.dispatch_mask=3;
        callback_fault=8; CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==E_ABORT && draw_count==round);
        reenter=1;
        if (bank) {
            D3DDDIARG_DRAWPRIMITIVE2 bytes={primitive.PrimitiveType,20,primitive.PrimitiveCount};
            CHECK(functions.pfnDrawPrimitive2(device,&bytes)==S_OK);
        } else CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==S_OK);
        reenter=0; CHECK(draw_count==round+1);
        draw_check_pixels(device,target[bank],target[bank^1],square);
    }
    /* Empty clipping publishes no job. Deleting bound objects clears state. */
    RECT empty={0,0,0,0}; CHECK(functions.pfnSetScissorRect(device,&empty)==S_OK);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==S_OK && draw_count==6);
    draw_state(device,D3DDDIRS_SCISSORTESTENABLE,0);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    for (unsigned i=0;i<2;i++) {
        missing=(D3DDDIARG_LOCK){0}; missing.hResource=vertex[i]; missing.Flags.DoNotWait=1;
        resource_null_mapping=1; callback_fault=7;
        CHECK(functions.pfnLock(device,&missing)==(HRESULT)0x88760827);
        callback_fault=7; CHECK(functions.pfnDestroyResource(device,vertex[i])==E_ABORT);
        callback_fault=5; CHECK(functions.pfnDestroyResource(device,vertex[i])==E_ABORT);
        CHECK(functions.pfnDestroyResource(device,vertex[i])==S_OK);
        CHECK(functions.pfnDestroyResource(device,target[i])==S_OK);
    }
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
    /* The second declaration is intentionally released by device teardown. */
}

void native_umd_draw_entry(void)
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
    if (!adapter) {report("UMDDRAW UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=draw_lock_callback;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=draw_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    draw_exercise(d.hDevice,a.hAdapter,&af,&d);
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==S_OK);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks && verified_programs==1);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    FreeLibrary(module);
    report("UMDDRAW checks=",checks); report("UMDDRAW draws=",draw_count); report("UMDDRAW failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
