/* D3D9 vertex declarations -> native VF, through real UMD/KMT callbacks. */
static void vertex_verify(const void *,unsigned);
#define NATIVE_UMD_VERIFY_PROGRAM vertex_verify
#include "native_umd_draw_pe.c"
#include "native_vertex_cases.h"
#include "native_vertex_oracle.h"
int _fltused;
static unsigned vertex_draws,vertex_type,vertex_layout,vertex_parameters,vertex_map[512],vertex_hash;
static void vertex_verify(const void *data,unsigned bytes)
{
    const struct intel_d3d9_program *p=data;
    CHECK(bytes==8192 && p->abi==3 && p->header_bytes==128 && p->texcoord_mask==1 && !p->texture_mask && !p->sampler_mask);
    CHECK(p->bytecode_bytes==sizeof(native_vertex_shader) && p->parameter_count==8 && p->parameter_map_offset<4096 && p->code_offset==4096);
    for (unsigned i=0;i<sizeof(native_vertex_shader);i++)
        CHECK(((const BYTE *)data)[p->bytecode_offset+i]==((const BYTE *)native_vertex_shader)[i]);
    vertex_parameters=p->parameter_count;memcpy(vertex_map,(const BYTE *)data+p->parameter_map_offset,vertex_parameters*4);
    for (unsigned i=0;i<vertex_parameters;i++) CHECK(vertex_map[i]<8);
    vertex_hash=2166136261u;
    for (unsigned i=0;i<p->code_bytes;i++) vertex_hash=(vertex_hash^((const BYTE *)data)[4096+i])*16777619u;
    report("VERTEX native code hash=",vertex_hash);
}
static HRESULT APIENTRY vertex_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);CHECK(args->CommandLength==1456+vertex_parameters*4 && args->NumAllocations==3 && args->NumPatchLocations==3);
#ifdef UMD_MODEL
    const native_vertex_case_t *c=&native_vertex_cases[vertex_type];
    unsigned stride=16+c->bytes,pos=vertex_layout ? c->bytes : 0,coord=vertex_layout ? 0 : 16;
    unsigned typed=vertex_layout || vertex_type!=3;
    const intel_umd_draw_sampled_t *p=current_commands;
    CHECK(p->draw.version==(typed ? 6 : 5) && p->draw.vertex_stride==stride && p->draw.vertex_offset==256-3*stride);
    CHECK(p->draw.vertex_count==3 && !p->bound_mask && p->position_offset==(typed ? pos : 0));
    CHECK(p->texcoord_offset[0]==(coord|(typed ? vertex_type<<16 : 0)));
    for (unsigned i=1;i<8;i++) CHECK(!p->texcoord_offset[i]);
    for (unsigned i=0;i<sizeof(p->textures);i++) CHECK(!((const BYTE *)p->textures)[i]);
    unsigned constants[8];memcpy(constants,c->scale,16);memcpy(constants+4,c->bias,16);
    CHECK(p->parameter_count==vertex_parameters && !p->rectangle_count);
    for (unsigned i=0;i<vertex_parameters;i++) CHECK(p->values[i]==constants[vertex_map[i]]);
    for (unsigned i=0;i<3;i++) {
        CHECK(current_patches[i].AllocationIndex==i && current_patches[i].SlotId==i && current_patches[i].PatchOffset==80+8*i);
        CHECK(current_allocations[i].Value==(i==1));
    }
    unsigned bank=vertex_draws&1;
    current_commands=draw_commands[bank];current_allocations=draw_allocations[bank];current_patches=draw_patches[bank];
    memset(current_commands,0xa5,4096);memset(current_allocations,0xa5,sizeof(draw_allocations[0]));
    memset(current_patches,0xa5,sizeof(draw_patches[0]));
    args->pNewCommandBuffer=current_commands;args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations;args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches;args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0};q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength;q.allocation_count=args->NumAllocations;q.patch_count=args->NumPatchLocations;
    OKMT_S32 status=krender(&q);
    if(status==(OKMT_S32)0x80000011)return D3DERR_WASSTILLDRAWING;
    if(status)report("VERTEX render status=",status);CHECK(status==0);
    args->pNewCommandBuffer=q.commands;args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations;args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches;args->NewPatchLocationListSize=q.patch_capacity;
#endif
    vertex_draws++;return S_OK;
}
static void vertex_check_pixels(HANDLE d,HANDLE target)
{
#ifndef UMD_MODEL
    D3DDDIARG_LOCK lock=draw_lock(d,target,1,0,0);
    unsigned want=native_vertex_pixels[vertex_type*2+vertex_layout];
    for(unsigned i=0;i<256;i++) {
        unsigned x=i%16,y=i/16,covered=x>=2 && y>=2 && x+y<=15;
        unsigned pixel=((const volatile unsigned *)lock.pSurfData)[i];
        if(!covered){CHECK(pixel==0xa6a6a6a6u);continue;}
        /* One UNORM8 step allows native interpolation/RT rounding differences
         * between the Windows HAL and Intel. No coverage/padding tolerance. */
        for(unsigned channel=0;channel<4;channel++) {
            int delta=(int)((pixel>>(channel*8))&255)-(int)((want>>(channel*8))&255);
            if(delta< -1 || delta>1){report("VERTEX actual=",pixel);report("VERTEX expected=",want);}
            CHECK(delta>=-1 && delta<=1);
        }
    }
    D3DDDIARG_UNLOCK unlock={target,0,{0}};CHECK(functions.pfnUnlock(d,&unlock)==S_OK);
#else
    (void)d;(void)target;
#endif
}
static void vertex_exercise(HANDLE device,HANDLE adapter_handle,const D3DDDI_ADAPTERFUNCS *af,
                            const D3DDDIARG_CREATEDEVICE *primary)
{
    (void)adapter_handle;(void)af;(void)primary;
    HANDLE vertex=draw_resource(device,0x9000,TRUE),target=draw_resource(device,0x9100,FALSE);
    D3DDDIARG_CREATEPIXELSHADER shader={sizeof(native_vertex_shader),0};
    CHECK(functions.pfnCreatePixelShader(device,&shader,native_vertex_shader)==S_OK);
    CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0};CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    const unsigned positions[3][4]={{0x40000000,0x40000000,0,0x3f800000},{0x41600000,0x40000000,0,0x3f800000},
        {0x40000000,0x41600000,0,0x3f800000}};
    for(vertex_type=0;vertex_type<17;vertex_type++)for(vertex_layout=0;vertex_layout<2;vertex_layout++) {
        report("VERTEX type=",vertex_type);report("VERTEX layout=",vertex_layout);
        const native_vertex_case_t *c=&native_vertex_cases[vertex_type];
        unsigned stride=16+c->bytes,pos=vertex_layout ? c->bytes : 0,coord=vertex_layout ? 0 : 16,base=256-3*stride;
        D3DDDIVERTEXELEMENT elements[2]={{0,(WORD)pos,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0},
            {0,(WORD)coord,(BYTE)vertex_type,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}};
        D3DDDIARG_CREATEVERTEXSHADERDECL decl={2,0};
        CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,elements)==S_OK);
        CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
        D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,base,stride};CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
        D3DDDIARG_LOCK access=draw_lock(device,vertex,2,0,0);memset(access.pSurfData,0xa5,256);
        for(unsigned i=0;i<3;i++) {
            memcpy((BYTE *)access.pSurfData+base+i*stride+pos,positions[i],16);
            memcpy((BYTE *)access.pSurfData+base+i*stride+coord,c->data,c->bytes);
        }
        D3DDDIARG_UNLOCK unlock={vertex,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        D3DDDIARG_SETPIXELSHADERCONST constants={0,2};float values[8];
        memcpy(values,c->scale,16);memcpy(values+4,c->bias,16);
        CHECK(functions.pfnSetPixelShaderConst(device,&constants,values)==S_OK);
        D3DDDIARG_DRAWPRIMITIVE primitive={D3DPT_TRIANGLELIST,0,1};
        stream.Offset=base+4;CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
        CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==D3DERR_INVALIDCALL);
        stream.Offset=base;CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
        access=draw_lock(device,target,2,0,0);memset(access.pSurfData,0xa6,1024);
        unlock=(D3DDDIARG_UNLOCK){target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        HRESULT status=D3DERR_WASSTILLDRAWING;
        for(unsigned attempt=0;attempt<300 && status==D3DERR_WASSTILLDRAWING;attempt++) {
            status=functions.pfnDrawPrimitive(device,&primitive,NULL);
            if(status==D3DERR_WASSTILLDRAWING)Sleep(10);
        }
        CHECK(status==S_OK);vertex_check_pixels(device,target);
        CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    }
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);
    CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
}

void native_umd_vertex_entry(void)
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
    if (!adapter) {report("VERTEX UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=lock_allocation;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=vertex_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    vertex_exercise(d.hDevice,a.hAdapter,&af,&d);
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
    report("VERTEX checks=",checks); report("VERTEX draws=",vertex_draws); report("VERTEX failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
