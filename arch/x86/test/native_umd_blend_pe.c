/* Native UMD command/lifetime checks and physical pixels compared with an
 * independently observed Microsoft D3D9 HAL oracle. No model renders pixels. */
#define native_umd_clear_entry unused_clear_entry
#include "native_umd_clear_pe.c"
#undef native_umd_clear_entry
#include "native_blend_cases.h"
#include "native_blend_oracle.h"

static unsigned blend_case,blend_format,blend_count,blend_pixels,blend_busy_once;
static HRESULT APIENTRY blend_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    CHECK(args->CommandLength==152 && args->NumAllocations==3 && args->NumPatchLocations==3 &&
        !args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    const native_blend_case_t *c=&native_blend_cases[blend_case];
    const intel_umd_draw_constants_t *q=current_commands;
    BOOL enabled=c->enable || c->mask!=15;
    CHECK(q->draw.magic==INTEL_UMD_COMMAND_MAGIC && q->draw.opcode==2 && q->draw.bytes==152);
    CHECK(q->draw.version==(enabled?4u:2u) && q->parameter_count==4 && !q->reserved);
    CHECK(q->draw.destination_width==16 && q->draw.destination_height==16 && q->draw.destination_pitch==64);
    CHECK(!q->draw.x && !q->draw.y && q->draw.width==16 && q->draw.height==16);
    CHECK(!q->draw.vertex_offset && q->draw.vertex_stride==16 && q->draw.vertex_count==4 && q->draw.topology==5);
    CHECK(!q->draw.vertices && !q->draw.destination && !q->draw.program && !q->draw.reserved0);
    CHECK(!q->draw.reserved1[0] && !q->draw.reserved1[1] && !q->draw.reserved2[4] && !q->draw.reserved2[5]);
    for(unsigned i=0;i<4;i++)CHECK(q->values[i]==native_blend_color_bits[i]);
    unsigned rgb=c->src|(c->dst<<5)|(c->op<<10),alpha=c->separate ? c->asrc|(c->adst<<5)|(c->aop<<10) : rgb;
    CHECK(q->draw.reserved2[0]==(enabled ? INTEL_UMD_BLEND_VALID|(c->mask<<4)|c->enable|(blend_format?2:0) : 0));
    CHECK(q->draw.reserved2[1]==(enabled ? rgb : 0) && q->draw.reserved2[2]==(enabled ? alpha : 0));
    CHECK(q->draw.reserved2[3]==(enabled ? NATIVE_BLEND_FACTOR : 0));
    for(unsigned i=0;i<3;i++) {
        const D3DDDI_PATCHLOCATIONLIST *patch=&current_patches[i];
        CHECK(patch->AllocationIndex==i && patch->SlotId==i && patch->PatchOffset==80+8*i &&
            !patch->DriverId && !patch->AllocationOffset && !patch->SplitOffset);
        CHECK(current_allocations[i].Value==(i==1));
    }
    if(blend_busy_once){blend_busy_once=0;return D3DERR_WASSTILLDRAWING;}
    unsigned next=blend_count&1;
    current_commands=draw_commands[next];current_allocations=draw_allocations[next];current_patches=draw_patches[next];
    memset(current_commands,0xa5,4096);
    args->pNewCommandBuffer=current_commands;args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations;args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches;args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0};q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength;q.allocation_count=q.patch_count=3;
    OKMT_S32 status=krender(&q);
    if(status==(OKMT_S32)0x80000011)return D3DERR_WASSTILLDRAWING;
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands;args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations;args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches;args->NewPatchLocationListSize=q.patch_capacity;
#endif
    blend_count++;return S_OK;
}
static void blend_exercise(HANDLE device)
{
    HANDLE vertex=draw_resource(device,0xd000,TRUE);
    D3DDDIARG_LOCK locked=draw_lock(device,vertex,2,0,0);
    const FLOAT quad[4][4]={{-0.5f,-0.5f,0,1},{15.5f,-0.5f,0,1},{-0.5f,15.5f,0,1},{15.5f,15.5f,0,1}};
    memcpy(locked.pSurfData,quad,sizeof(quad));
    D3DDDIARG_UNLOCK unlock={vertex,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    D3DDDIVERTEXELEMENT element={0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0};
    D3DDDIARG_CREATEVERTEXSHADERDECL decl={1,0};
    CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,0,16};CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    D3DDDIARG_CREATEPIXELSHADER shader={sizeof(native_blend_bytecode),0};
    CHECK(functions.pfnCreatePixelShader(device,&shader,(const UINT *)native_blend_bytecode)==S_OK);
    CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
    D3DDDIARG_SETPIXELSHADERCONST constants={0,1};
    CHECK(functions.pfnSetPixelShaderConst(device,&constants,(const FLOAT *)native_blend_color_bits)==S_OK);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    draw_state(device,D3DDDIRS_BLENDFACTOR,NATIVE_BLEND_FACTOR);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    for(blend_format=0;blend_format<2;blend_format++) {
        D3DDDI_SURFACEINFO surface={16,16,1,NULL,0,0};
        D3DDDIARG_CREATERESOURCE resource={0};resource.hResource=(HANDLE)(UINT_PTR)(0xd001+blend_format);
        resource.Format=blend_format?D3DDDIFMT_X8R8G8B8:D3DDDIFMT_A8R8G8B8;
        resource.Pool=D3DDDIPOOL_VIDEOMEMORY;resource.pSurfList=&surface;resource.SurfCount=resource.MipLevels=1;
        resource.Rotation=D3DDDI_ROTATION_IDENTITY;resource.Flags.RenderTarget=1;
        CHECK(functions.pfnCreateResource(device,&resource)==S_OK);
        HANDLE target=resource.hResource;
        D3DDDIARG_SETRENDERTARGET rt={.RenderTargetIndex=0,.hRenderTarget=target,.SubResourceIndex=0};
        HRESULT binding=functions.pfnSetRenderTarget(device,&rt);
        if(binding!=S_OK){report("BLEND bind format=",blend_format);report("BLEND bind result=",binding);report("BLEND bind target=",(UINT)(UINT_PTR)target);}
        CHECK(binding==S_OK);
        for(blend_case=0;blend_case<NATIVE_BLEND_CASE_COUNT;blend_case++) {
            const native_blend_case_t *c=&native_blend_cases[blend_case];
            draw_state(device,D3DDDIRS_ALPHABLENDENABLE,c->enable);
            draw_state(device,D3DDDIRS_SRCBLEND,c->src);draw_state(device,D3DDDIRS_DESTBLEND,c->dst);
            draw_state(device,D3DDDIRS_BLENDOP,c->op);draw_state(device,D3DDDIRS_SEPARATEALPHABLENDENABLE,c->separate);
            draw_state(device,D3DDDIRS_SRCBLENDALPHA,c->asrc);draw_state(device,D3DDDIRS_DESTBLENDALPHA,c->adst);
            draw_state(device,D3DDDIRS_BLENDOPALPHA,c->aop);draw_state(device,D3DDDIRS_COLORWRITEENABLE,c->mask);
            if(blend_case==1) {
                const D3DDDIARG_RENDERSTATE invalid[]={{D3DDDIRS_SRCBLEND,0},{D3DDDIRS_BLENDOP,6},
                    {D3DDDIRS_COLORWRITEENABLE,16},{D3DDDIRS_SEPARATEALPHABLENDENABLE,2}};
                for(unsigned i=0;i<4;i++)CHECK(functions.pfnSetRenderState(device,&invalid[i])==D3DERR_INVALIDCALL);
                const D3DDDIARG_RENDERSTATE unsupported={D3DDDIRS_SRCBLEND,12};
                CHECK(functions.pfnSetRenderState(device,&unsupported)==D3DERR_NOTAVAILABLE);
            }
            locked=draw_lock(device,target,2,0,0);CHECK(locked.Pitch==64);
            for(unsigned i=0;i<256;i++)((unsigned *)locked.pSurfData)[i]=NATIVE_BLEND_DESTINATION;
            unlock.hResource=target;CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
            D3DDDIARG_DRAWPRIMITIVE draw={.PrimitiveType=D3DPT_TRIANGLESTRIP,.PrimitiveCount=2,.VStart=0};
            callback_fault=8;CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==E_ABORT);
            blend_busy_once=1;HRESULT result=D3DERR_WASSTILLDRAWING;
            for(unsigned attempt=0;attempt<300 && result==D3DERR_WASSTILLDRAWING;attempt++) {
                result=functions.pfnDrawPrimitive(device,&draw,NULL);if(result==D3DERR_WASSTILLDRAWING)Sleep(10);
            }
            CHECK(result==S_OK);
#ifndef UMD_MODEL
            locked=draw_lock(device,target,1,0,0);
            unsigned expected=native_blend_pixels[blend_format][blend_case];
            for(unsigned i=0;i<256;i++) {
                unsigned actual=((const unsigned *)locked.pSurfData)[i];
                for(unsigned component=0;component<(blend_format?3u:4u);component++) {
                    int delta=(int)((actual>>(8*component))&255)-(int)((expected>>(8*component))&255);
                    if(delta>1 || delta<-1){report("BLEND format=",blend_format);report("BLEND case=",blend_case);report("BLEND actual=",actual);report("BLEND expected=",expected);}
                    CHECK(delta<=1 && delta>=-1);
                }
                blend_pixels++;
            }
            CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
#endif
        }
        CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    }
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);
    CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
}

void native_umd_blend_entry(void)
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
    if (!adapter) {report("BLEND UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=draw_lock_callback;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=blend_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    blend_exercise(d.hDevice);
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
    report("BLEND checks=",checks); report("BLEND draws=",blend_count); report("BLEND pixels=",blend_pixels); report("BLEND failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
