/* Public D3D9 texture callbacks to immutable KMT draws. The model inspects
 * packets/ownership only; physical builds read pixels produced by the GPU. */
static void sampled_verify(const void *,unsigned);
#define NATIVE_UMD_VERIFY_PROGRAM sampled_verify
#include "native_umd_draw_pe.c"
#include "sampled-program.h"

static UINT sampled_draws,expected_unique=1,expected_mode,expected_level,expected_srgb;
static UINT expected_color,expected_last;
static void sampled_verify(const void *data,unsigned bytes)
{
    const BYTE *got=data,*want=(const void *)sampled_program;
    CHECK(bytes==8192);
    for (UINT i=0;i<sizeof(sampled_program);i++) CHECK(got[i]==want[i]);
    for (UINT i=sizeof(sampled_program);i<bytes;i++) CHECK(!got[i]);
}
static HRESULT APIENTRY sampled_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    CHECK(args->CommandLength==1456 && args->NumAllocations==3+expected_unique &&
        args->NumPatchLocations==19 && !args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    const intel_umd_draw_sampled_t *p=current_commands;
    CHECK(p->draw.magic==INTEL_UMD_COMMAND_MAGIC && p->draw.version==5 && p->draw.opcode==INTEL_UMD_DRAW32);
    CHECK(p->draw.bytes==1456 && p->draw.vertex_offset==4 && p->draw.vertex_stride==36 &&
        p->draw.vertex_count==3 && p->draw.topology==D3DPT_TRIANGLELIST && p->draw.dispatch_mask==3);
    CHECK(p->draw.destination_width==16 && p->draw.destination_height==16 && p->draw.destination_pitch==64);
    CHECK(p->draw.x==0 && p->draw.y==0 && p->draw.width==16 && p->draw.height==16);
    CHECK(p->draw.reserved2[0]==(INTEL_UMD_BLEND_VALID|0xf0) && p->draw.reserved2[3]==0xffffffffu);
    CHECK(p->bound_mask==0xffff && !p->parameter_count && !p->rectangle_count && !p->reserved);
    CHECK(p->texcoord_offset[0]==16);
    for (UINT i=1;i<8;i++) CHECK(!p->texcoord_offset[i]);
    for (UINT i=0;i<args->NumAllocations;i++) {
        UINT slot=0;
        while (slot<8 && backing[slot].allocation!=current_allocations[i].hAllocation) slot++;
        CHECK(slot<8 && !backing[slot].locked);
        CHECK(current_allocations[i].Value==(i==1));
        for (UINT j=0;j<i;j++) CHECK(current_allocations[i].hAllocation!=current_allocations[j].hAllocation);
    }
    for (UINT i=0;i<19;i++) {
        const D3DDDI_PATCHLOCATIONLIST *patch=&current_patches[i];
        CHECK(patch->SlotId==i && !patch->DriverId && !patch->AllocationOffset && !patch->SplitOffset);
        CHECK(patch->AllocationIndex==(i<3 ? i : i==18 && expected_unique==2 ? 4 : 3));
        CHECK(patch->PatchOffset==(i<3 ? 80+8*i : 176+(i-3)*80));
    }
    for (UINT i=0;i<16;i++) {
        const intel_umd_texture_t *t=&p->textures[i];
        CHECK(!t->image && t->width==16 && t->height==16 && t->levels==2 && t->pitch==64 && t->format==21);
        CHECK(!t->first && t->count==(expected_mode ? 2 : 1));
        CHECK(t->srgb==expected_srgb && t->sampler.min_filter==1 && t->sampler.mag_filter==1);
        CHECK(t->sampler.mip_filter==expected_mode && t->sampler.address_u==1 && t->sampler.address_v==1 && t->sampler.address_w==1);
        CHECK(t->sampler.min_lod==(expected_mode ? expected_level*256 : 0));
        CHECK(t->sampler.max_lod==(expected_mode ? 256 : 0) && !t->sampler.lod_bias && !t->sampler.border_argb);
    }
    UINT bank=sampled_draws&1;
    current_commands=draw_commands[bank]; current_allocations=draw_allocations[bank]; current_patches=draw_patches[bank];
    memset(current_commands,0xa5,4096); memset(current_allocations,0xa5,sizeof(draw_allocations[0]));
    memset(current_patches,0xa5,sizeof(draw_patches[0]));
    args->pNewCommandBuffer=current_commands; args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations; args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches; args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0}; q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength; q.allocation_count=args->NumAllocations; q.patch_count=args->NumPatchLocations;
    OKMT_S32 status=krender(&q);
    if (status==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    if (status) report("SAMPLED render status=",status);
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands; args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations; args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches; args->NewPatchLocationListSize=q.patch_capacity;
#endif
    sampled_draws++; return S_OK;
}
static void sampled_state(HANDLE d,UINT slot,D3DDDITEXTURESTAGESTATETYPE state,UINT value,HRESULT expected)
{
    D3DDDIARG_TEXTURESTAGESTATE s={slot,state,value};
    CHECK(functions.pfnSetTextureStageState(d,&s)==expected);
}
static HANDLE sampled_texture(HANDLE d,UINT id,UINT color)
{
    D3DDDI_SURFACEINFO surfaces[2]={{16,16,1,NULL,0,0},{8,8,1,NULL,0,0}};
    D3DDDIARG_CREATERESOURCE r={0}; r.hResource=(HANDLE)(UINT_PTR)id; r.Format=D3DDDIFMT_A8R8G8B8;
    r.Pool=D3DDDIPOOL_VIDEOMEMORY; r.pSurfList=surfaces; r.SurfCount=r.MipLevels=2;
    r.Rotation=D3DDDI_ROTATION_IDENTITY; r.Flags.Texture=1;
    CHECK(functions.pfnCreateResource(d,&r)==S_OK);
    for (UINT level=0;level<2;level++) {
        D3DDDIARG_LOCK lock={0}; lock.hResource=r.hResource; lock.SubResourceIndex=level; lock.Flags.WriteOnly=1;
        CHECK(functions.pfnLock(d,&lock)==S_OK && lock.pSurfData && lock.Pitch==64);
        for (UINT y=0;y<surfaces[level].Height;y++) for (UINT x=0;x<surfaces[level].Width;x++)
            ((UINT *)((BYTE *)lock.pSurfData+y*lock.Pitch))[x]=level ? 0xff030303u : color;
        D3DDDIARG_UNLOCK unlock={r.hResource,level,{0}}; CHECK(functions.pfnUnlock(d,&unlock)==S_OK);
    }
    return r.hResource;
}
static void sampled_check(HANDLE d,HANDLE target)
{
#ifndef UMD_MODEL
    D3DDDIARG_LOCK lock=draw_lock(d,target,1,0,0);
    CHECK(lock.Pitch==64);
    for (UINT i=0;i<256;i++) {
        UINT x=i%16,y=i/16,covered=x>=2 && y>=2 && x+y<=15;
        UINT actual=((const volatile UINT *)lock.pSurfData)[i],want=covered ? expected_color : 0xa6a6a6a6u;
        if (actual!=want) {report("SAMPLED mismatch pixel=",i);report("SAMPLED actual=",actual);report("SAMPLED expected=",want);}
        CHECK(actual==want);
    }
    D3DDDIARG_UNLOCK unlock={target,0,{0}}; CHECK(functions.pfnUnlock(d,&unlock)==S_OK);
#else
    (void)d; (void)target;
#endif
}
static void sampled_exercise(HANDLE device,HANDLE adapter_handle,const D3DDDI_ADAPTERFUNCS *af,
                             const D3DDDIARG_CREATEDEVICE *primary)
{
    CHECK(functions.pfnSetTexture && functions.pfnSetTextureStageState);
    CHECK(functions.pfnSetTextureStageState(device,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetTexture(device,16,NULL)==D3DERR_NOTAVAILABLE);
    CHECK(functions.pfnSetTexture(device,0,(HANDLE)0xbad)==D3DERR_INVALIDCALL);
    for (UINT slot=0;slot<16;slot++) {
        sampled_state(device,slot,D3DDDITSS_ADDRESSU,0,D3DERR_INVALIDCALL);
        sampled_state(device,slot,D3DDDITSS_ADDRESSV,6,D3DERR_INVALIDCALL);
        sampled_state(device,slot,D3DDDITSS_MINFILTER,0,D3DERR_INVALIDCALL);
        sampled_state(device,slot,D3DDDITSS_MAGFILTER,3,D3DERR_NOTAVAILABLE);
        sampled_state(device,slot,D3DDDITSS_MIPMAPLODBIAS,0x7fc00000,D3DERR_INVALIDCALL);
        sampled_state(device,slot,D3DDDITSS_MIPMAPLODBIAS,0x7f800000,D3DERR_INVALIDCALL);
        sampled_state(device,slot,D3DDDITSS_MIPMAPLODBIAS,0x41800000,D3DERR_NOTAVAILABLE);
        sampled_state(device,slot,D3DDDITSS_MAXANISOTROPY,2,D3DERR_NOTAVAILABLE);
        sampled_state(device,slot,D3DDDITSS_SRGBTEXTURE,7,D3DERR_NOTAVAILABLE);
        sampled_state(device,slot,D3DDDITSS_COLOROP,D3DTOP_ADD,D3DERR_NOTAVAILABLE);
    }
    HANDLE vertex=draw_resource(device,0x9000,TRUE),target=draw_resource(device,0x9100,FALSE);
    HANDLE textures[2]={sampled_texture(device,0x9200,0xff010101),sampled_texture(device,0x9201,0xff020202)};
    CHECK(functions.pfnSetTexture(device,0,vertex)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetTexture(device,0,target)==D3DERR_INVALIDCALL);
    D3DDDI_DEVICEFUNCS other; D3DDDIARG_CREATEDEVICE second=*primary;
    second.hDevice=runtime_device; second.pDeviceFuncs=&other;
    CHECK(af->pfnCreateDevice(adapter_handle,&second)==S_OK);
    CHECK(other.pfnSetTexture(second.hDevice,0,textures[0])==D3DERR_INVALIDCALL);
    CHECK(other.pfnDestroyDevice(second.hDevice)==S_OK);
    const struct intel_d3d9_program *reference=(const void *)sampled_program;
    D3DDDIARG_CREATEPIXELSHADER shader={reference->bytecode_bytes,0};
    CHECK(functions.pfnCreatePixelShader(device,&shader,(const UINT *)((const BYTE *)reference+reference->bytecode_offset))==S_OK);
    CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
    D3DDDIVERTEXELEMENT elements[2]={{0,20,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},
        {0,4,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0}};
    D3DDDIARG_CREATEVERTEXSHADERDECL decl={2,0};
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,elements)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,0,36}; CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0}; CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16}; CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    draw_state(device,D3DDDIRS_ZENABLE,0); draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    const UINT tri[3][4]={{0x40000000,0x40000000,0,0x3f800000},{0x41600000,0x40000000,0,0x3f800000},
        {0x40000000,0x41600000,0,0x3f800000}},uv[4]={0x3f000000,0x3f000000,0,0x3f800000};
    D3DDDIARG_LOCK access=draw_lock(device,vertex,2,0,0);
    for (UINT i=0;i<3;i++) {
        memcpy((BYTE *)access.pSurfData+4+i*36,tri[i],16);
        memcpy((BYTE *)access.pSurfData+20+i*36,uv,16);
    }
    D3DDDIARG_UNLOCK unlock={vertex,0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    D3DDDIARG_DRAWPRIMITIVE primitive={D3DPT_TRIANGLELIST,0,1};
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    for (UINT i=0;i<16;i++) CHECK(functions.pfnSetTexture(device,i,textures[0])==S_OK);
    access=draw_lock(device,textures[0],1,0,0);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    unlock=(D3DDDIARG_UNLOCK){textures[0],0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    stream.Stride=20; CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    stream.Stride=36; CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    for (UINT round=0;round<7;round++) {
        expected_unique=round==1 ? 2 : 1; expected_mode=round==4 ? 1 : round==5 ? 2 : 0;
        expected_level=round>=3 && round<=5; expected_srgb=round==6;
        expected_color=0xff000000u|(round==0 || round==3 ? 0x101010 : round==1 ? 0x111111 : round==2 ? 0x202020 :
            round==6 ? 0x010101 : 0x303030);
        for (UINT i=0;i<16;i++) {
            CHECK(functions.pfnSetTexture(device,i,textures[round==2 || (round==1 && i==15)])==S_OK);
            sampled_state(device,i,D3DDDITSS_MAXMIPLEVEL,expected_level,S_OK);
            sampled_state(device,i,D3DDDITSS_MIPFILTER,expected_mode,S_OK);
            sampled_state(device,i,D3DDDITSS_SRGBTEXTURE,expected_srgb,S_OK);
        }
        access=draw_lock(device,target,2,0,0); memset(access.pSurfData,0xa6,1024);
        unlock=(D3DDDIARG_UNLOCK){target,0,{0}}; CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        callback_fault=8; CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==E_ABORT);
        HRESULT status=D3DERR_WASSTILLDRAWING;
        for (UINT attempt=0;attempt<300 && status==D3DERR_WASSTILLDRAWING;attempt++) {
            status=functions.pfnDrawPrimitive(device,&primitive,NULL);
            if (status==D3DERR_WASSTILLDRAWING) Sleep(10);
        }
        CHECK(status==S_OK); sampled_check(device,target);
    }
    CHECK(functions.pfnDestroyResource(device,textures[0])==S_OK);
    CHECK(functions.pfnSetTexture(device,0,textures[0])==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDestroyResource(device,textures[1])==S_OK);
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);
    CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
}

void native_umd_sampled_entry(void)
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
    if (!adapter) {report("SAMPLED UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=lock_allocation;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=sampled_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    sampled_exercise(d.hDevice,a.hAdapter,&af,&d);
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
    report("SAMPLED checks=",checks); report("SAMPLED draws=",sampled_draws); report("SAMPLED failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
