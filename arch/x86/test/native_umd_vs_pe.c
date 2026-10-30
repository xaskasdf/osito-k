/* Original VS2/VS3 bytecode through the WDK driver ABI and public KMT.
 * The model verifies callback contracts; only physical mode draws pixels. */
static void vs_verify(const void *,unsigned);
#define NATIVE_UMD_VERIFY_PROGRAM vs_verify
#include "native_umd_draw_pe.c"
#include "native_umd_vs_fixtures.h"
static unsigned vs_draws,vs_profile,vs_mode,vs_layout,vs_ps,vs_bypass;
static unsigned ps_count,vs_count,ps_map[512],vs_map[512],matrix[16];
static const unsigned ps_values[]={0x3e800000,0x3f000000,0x3f400000,0x3f800000};
static const unsigned ps_plain[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0xffff};
static const unsigned ps_input[]={0xffff0200,0x0200001f,0x80000000,0xb00f0000,
    0x03000005,0x800f0800,0xb0e40000,0xa0e40000,0xffff};
/* s0 and s15 deliberately alias one image, after the separate PS/VS roles. */
static const unsigned ps_sampled[]={0xffff0200,
    0x0200001f,0x80000000,0xb00f0000,
    0x0200001f,0x90000000,0xa00f0800,0x0200001f,0x90000000,0xa00f080f,
    0x03000042,0x800f0000,0xb0e40000,0xa0e40800,
    0x03000042,0x800f0001,0xb0e40000,0xa0e4080f,
    0x03000002,0x800f0000,0x80e40000,0x80e40001,
    0x03000005,0x800f0800,0x80e40000,0xa0e40000,0xffff};
static void vs_verify(const void *data,unsigned bytes)
{
    const struct intel_d3d9_program *p=data;
    CHECK(bytes==8192 && p->code_offset==4096 && p->allocation_bytes==4096+p->code_bytes);
    CHECK(p->header_bytes==(p->abi==4 ? 256u : 128u) && p->bytecode_offset==p->header_bytes);
    CHECK(p->parameter_count<=512 && p->parameter_map_offset==p->header_bytes+p->bytecode_bytes);
    const unsigned *map=(const void *)((const BYTE *)data+p->parameter_map_offset);
    for (unsigned i=p->parameter_map_offset+4*p->parameter_count;i<4096;i++) CHECK(!((const BYTE *)data)[i]);
    for (unsigned i=p->allocation_bytes;i<bytes;i++) CHECK(!((const BYTE *)data)[i]);
    if (p->abi==4) {
        const struct intel_d3d9_vertex_program *v=data;
        CHECK(v->input_mask==0x8001 && (v->output_mask&9)==9 && p->parameter_count==16);
        CHECK(v->input_semantics[0]==D3DDECLUSAGE_POSITION && v->input_semantics[15]==D3DDECLUSAGE_TEXCOORD);
        const unsigned *want=vs_profile==2 ? vs2_code : vs3_code;
        unsigned size=vs_profile==2 ? sizeof(vs2_code) : sizeof(vs3_code);
        CHECK(p->bytecode_bytes==size);
        for (unsigned i=0;i<size;i++) CHECK(((const BYTE *)data)[256+i]==((const BYTE *)want)[i]);
        vs_count=p->parameter_count;memcpy(vs_map,map,vs_count*4);
        for (unsigned i=0;i<vs_count;i++) CHECK(vs_map[i]<16);
    } else {
        CHECK(p->abi==2 || p->abi==3);
        CHECK(p->parameter_count==4);
        if (p->abi==3) {ps_count=p->parameter_count;memcpy(ps_map,map,ps_count*4);}
        for (unsigned i=0;i<p->parameter_count;i++) CHECK(map[i]<4);
    }
}
static HRESULT APIENTRY vs_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    unsigned textured=!vs_bypass && vs_ps==2,resources=vs_bypass ? 3 : textured ? 5 : 4;
    CHECK(args->CommandLength==(vs_bypass ? 152 : 1552+4*(ps_count+vs_count)) &&
        args->NumAllocations==resources && args->NumPatchLocations==resources+textured);
#ifdef UMD_MODEL
    const intel_umd_draw_vertex_shader_t *p=current_commands;
    if(vs_bypass) {
        const intel_umd_draw_constants_t *q=current_commands;
        CHECK(q->draw.version==2 && q->draw.vertex_offset==16 && q->parameter_count==4);
        for(unsigned i=0;i<4;i++) CHECK(q->values[i]==ps_values[i]);
    } else {
    CHECK(p->draw.version==7 && p->draw.bytes==args->CommandLength && p->draw.vertex_offset==16);
    CHECK(p->draw.vertex_count==6 && p->draw.vertex_stride==32 && p->draw.topology==4);
    CHECK(p->viewport_width==16 && p->viewport_height==16 && !p->viewport_x && !p->viewport_y);
    CHECK(p->parameter_count==ps_count && p->vertex_parameter_count==vs_count && p->bound_mask==(textured ? 0x8001u : 0));
    CHECK(!p->draw.vertices && !p->draw.destination && !p->draw.program && !p->vertex_program && !p->reserved);
    CHECK(p->attributes[0]==(vs_layout ? (2u<<16)|16 : 3u<<16));
    CHECK(p->attributes[15]==(vs_layout ? 3u<<16 : (3u<<16)|16));
    for (unsigned i=1;i<15;i++) CHECK(!p->attributes[i]);
    for (unsigned i=0;i<8;i++) CHECK(!p->texcoord_offset[i]);
    for (unsigned i=0;i<ps_count;i++) CHECK(p->values[i]==ps_values[ps_map[i]]);
    for (unsigned i=0;i<vs_count;i++) CHECK(p->values[ps_count+i]==matrix[vs_map[i]]);
    }
    for (unsigned i=0;i<resources;i++) {
        const D3DDDI_PATCHLOCATIONLIST *patch=&current_patches[i];
        CHECK(patch->AllocationIndex==i && patch->SlotId==(i==3 ? 19u : i==4 ? 3u : i));
        CHECK(patch->PatchOffset==(i==3 ? 1456u : i==4 ? 176u : 80+8*i) && !patch->AllocationOffset && !patch->DriverId && !patch->SplitOffset);
        CHECK(current_allocations[i].Value==(i==1));
        unsigned slot=0;while(slot<8 && backing[slot].allocation!=current_allocations[i].hAllocation) slot++;
        CHECK(slot<8 && !backing[slot].locked);
        for (unsigned j=0;j<i;j++) CHECK(current_allocations[i].hAllocation!=current_allocations[j].hAllocation);
    }
    if(textured) {
        const D3DDDI_PATCHLOCATIONLIST *alias=&current_patches[5];
        CHECK(alias->AllocationIndex==4 && alias->SlotId==18 && alias->PatchOffset==176+15*80);
        CHECK(!alias->AllocationOffset && !alias->DriverId && !alias->SplitOffset);
        for(unsigned i=0;i<16;i++) if(i==0 || i==15) {
            CHECK(!p->textures[i].image && p->textures[i].width==2 && p->textures[i].height==2);
        } else CHECK(!p->textures[i].width && !p->textures[i].height);
    }
    unsigned next=vs_draws&1;
    current_commands=draw_commands[next];current_allocations=draw_allocations[next];current_patches=draw_patches[next];
    memset(current_commands,0xa5,4096);
    args->pNewCommandBuffer=current_commands;args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations;args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches;args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0};q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength;q.allocation_count=args->NumAllocations;q.patch_count=args->NumPatchLocations;
    OKMT_S32 status=krender(&q);
    if (status==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    if (status) report("VS KMT status=",status);
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands;args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations;args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches;args->NewPatchLocationListSize=q.patch_capacity;
#endif
    vs_draws++;return S_OK;
}
static void vs_pixels(HANDLE device,HANDLE target)
{
#ifndef UMD_MODEL
    unsigned equal=0;
    for (unsigned attempt=0;attempt<100 && !equal;attempt++) {
        D3DDDIARG_LOCK access=draw_lock(device,target,1,0,0);equal=1;
        const volatile unsigned *pixels=access.pSurfData;
        unsigned left=vs_mode==1 ? 2 : vs_mode==3 ? 4 : 0;
        unsigned extent=vs_mode==1 || vs_mode==3 ? 8 : 16;
        unsigned color=vs_ps==2 ? 0xff404030 : vs_ps ? 0xff286030 : 0xff4080bf;
        for (unsigned i=0;i<256;i++) {
            unsigned x=i%16,y=i/16,covered=x>=left && y>=left && x<left+extent && y<left+extent;
            if (!covered) {if(pixels[i]!=0xa6a6a6a6) equal=0;continue;}
            for (unsigned c=0;c<4;c++) {
                int delta=(int)((pixels[i]>>(c*8))&255)-(int)((color>>(c*8))&255);
                if (delta<-1 || delta>1) equal=0;
            }
        }
        D3DDDIARG_UNLOCK u={target,0,{0}};CHECK(functions.pfnUnlock(device,&u)==S_OK);
        if(!equal) Sleep(10);
    }
    CHECK(equal);
#else
    (void)device;(void)target;
#endif
}
static void vs_exercise(HANDLE device,HANDLE adapter_handle,const D3DDDI_ADAPTERFUNCS *af,
                        const D3DDDIARG_CREATEDEVICE *primary)
{
    CHECK(functions.pfnCreateVertexShaderFunc && functions.pfnSetVertexShaderFunc &&
        functions.pfnDeleteVertexShaderFunc && functions.pfnSetVertexShaderConst);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    HANDLE vertex=draw_resource(device,0x9000,TRUE),target=draw_resource(device,0x9100,FALSE);
    D3DDDI_SURFACEINFO surface={2,2,1,NULL,0,0};
    D3DDDIARG_CREATERESOURCE image={0};image.hResource=(HANDLE)0x9200;image.Format=D3DDDIFMT_A8R8G8B8;
    image.Pool=D3DDDIPOOL_VIDEOMEMORY;image.pSurfList=&surface;image.SurfCount=image.MipLevels=1;
    image.Rotation=D3DDDI_ROTATION_IDENTITY;image.Flags.Texture=1;
    CHECK(functions.pfnCreateResource(device,&image)==S_OK);
    D3DDDIARG_LOCK tex=draw_lock(device,image.hResource,2,0,0);
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++)((unsigned *)((BYTE *)tex.pSurfData+y*tex.Pitch))[x]=0xff804020;
    D3DDDIARG_UNLOCK tex_release={image.hResource,0,{0}};CHECK(functions.pfnUnlock(device,&tex_release)==S_OK);
    CHECK(functions.pfnSetTexture(device,0,image.hResource)==S_OK);
    CHECK(functions.pfnSetTexture(device,15,image.hResource)==S_OK);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0};CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    HANDLE shaders[3];
    for (unsigned i=0;i<3;i++) {
        D3DDDIARG_CREATEPIXELSHADER p={i==2 ? sizeof(ps_sampled) : i ? sizeof(ps_input) : sizeof(ps_plain),NULL};
        CHECK(functions.pfnCreatePixelShader(device,&p,i==2 ? ps_sampled : i ? ps_input : ps_plain)==S_OK);shaders[i]=p.ShaderHandle;
    }
    D3DDDIARG_SETPIXELSHADERCONST pc={0,1};CHECK(functions.pfnSetPixelShaderConst(device,&pc,(const void *)ps_values)==S_OK);
    D3DDDIARG_SETVERTEXSHADERCONST constants={256,1};
    CHECK(functions.pfnSetVertexShaderConst(device,&constants,matrix)==D3DERR_INVALIDCALL);
    constants=(D3DDDIARG_SETVERTEXSHADERCONST){255,2};CHECK(functions.pfnSetVertexShaderConst(device,&constants,matrix)==D3DERR_INVALIDCALL);
    constants=(D3DDDIARG_SETVERTEXSHADERCONST){0,1};CHECK(functions.pfnSetVertexShaderConst(device,&constants,NULL)==D3DERR_INVALIDCALL);
    constants=(D3DDDIARG_SETVERTEXSHADERCONST){256,0};CHECK(functions.pfnSetVertexShaderConst(device,&constants,NULL)==S_OK);
    for(vs_profile=2;vs_profile<=3;vs_profile++) {
        const unsigned *code=vs_profile==2 ? vs2_code : vs3_code;
        unsigned size=vs_profile==2 ? sizeof(vs2_code) : sizeof(vs3_code);
        D3DDDIARG_CREATEVERTEXSHADERFUNC vs={size-1,(HANDLE)0xdead};
        CHECK(functions.pfnCreateVertexShaderFunc(device,&vs,code)==D3DERR_INVALIDCALL && vs.ShaderHandle==(HANDLE)0xdead);
        vs.Size=size;CHECK(functions.pfnCreateVertexShaderFunc(device,&vs,code)==S_OK);
        CHECK(functions.pfnSetVertexShaderFunc(device,shaders[0])==D3DERR_INVALIDCALL);
        CHECK(functions.pfnSetPixelShader(device,vs.ShaderHandle)==D3DERR_INVALIDCALL);
        D3DDDI_DEVICEFUNCS other_funcs;D3DDDIARG_CREATEDEVICE other=*primary;
        other.hDevice=runtime_device;other.pDeviceFuncs=&other_funcs;
        CHECK(af->pfnCreateDevice(adapter_handle,&other)==S_OK);
        CHECK(other_funcs.pfnSetVertexShaderFunc(other.hDevice,vs.ShaderHandle)==D3DERR_INVALIDCALL);
        CHECK(other_funcs.pfnDeleteVertexShaderFunc(other.hDevice,vs.ShaderHandle)==D3DERR_INVALIDCALL);
        CHECK(other_funcs.pfnDestroyDevice(other.hDevice)==S_OK);
        callback_fault=4;CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==E_ABORT);
        callback_fault=6;CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==E_ABORT);
        callback_fault=7;CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==E_ABORT);
        reenter=1;CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==S_OK);reenter=0;
        for(vs_ps=0;vs_ps<3;vs_ps++) for(vs_layout=0;vs_layout<2;vs_layout++) {
            CHECK(functions.pfnSetPixelShader(device,shaders[vs_ps])==S_OK);
            D3DDDIVERTEXELEMENT e[2]={{0,vs_layout ? 16 : 0,vs_layout ? D3DDECLTYPE_FLOAT3 : D3DDECLTYPE_FLOAT4,
                D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},{0,vs_layout ? 0 : 16,D3DDECLTYPE_FLOAT4,
                D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}};
            D3DDDIARG_CREATEVERTEXSHADERDECL decl={2,NULL};CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,e)==S_OK);
            CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
            D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,16,32};CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
            D3DDDIARG_DRAWPRIMITIVE negative={D3DPT_TRIANGLELIST,0,2};
            D3DDDIARG_CREATEVERTEXSHADERDECL missing={2,NULL};e[1].UsageIndex=1;
            CHECK(functions.pfnCreateVertexShaderDecl(device,&missing,e)==S_OK);e[1].UsageIndex=0;
            CHECK(functions.pfnSetVertexShaderDecl(device,missing.ShaderHandle)==S_OK);
            CHECK(functions.pfnDrawPrimitive(device,&negative,NULL)==D3DERR_INVALIDCALL);
            CHECK(functions.pfnDeleteVertexShaderDecl(device,missing.ShaderHandle)==S_OK);
            CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
            stream.Stride=16;CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
            CHECK(functions.pfnDrawPrimitive(device,&negative,NULL)==D3DERR_INVALIDCALL);
            stream.Stride=32;CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
            negative.VStart=0xffffffff;CHECK(functions.pfnDrawPrimitive(device,&negative,NULL)==D3DERR_INVALIDCALL);
            D3DDDIARG_LOCK access=draw_lock(device,vertex,2,0,0);memset(access.pSurfData,0x5a,256);
            const unsigned positions[6][4]={{0xbf800000,0x3f800000,0x3f000000,0x3f800000},
                {0x3f800000,0x3f800000,0x3f000000,0x3f800000},{0xbf800000,0xbf800000,0x3f000000,0x3f800000},
                {0xbf800000,0xbf800000,0x3f000000,0x3f800000},{0x3f800000,0x3f800000,0x3f000000,0x3f800000},
                {0x3f800000,0xbf800000,0x3f000000,0x3f800000}};
            const unsigned uv[4]={0x3f200000,0x3f400000,0x3e800000,0x3f800000};
            for(unsigned i=0;i<6;i++) {
                BYTE *v=(BYTE *)access.pSurfData+16+i*32;
                memcpy(v+(vs_layout ? 16 : 0),positions[i],vs_layout ? 12 : 16);
                memcpy(v+(vs_layout ? 0 : 16),uv,16);
            }
            D3DDDIARG_UNLOCK u={vertex,0,{0}};CHECK(functions.pfnUnlock(device,&u)==S_OK);
            for(vs_mode=0;vs_mode<4;vs_mode++) {
                memset(matrix,0,sizeof(matrix));matrix[0]=matrix[5]=matrix[10]=matrix[15]=0x3f800000;
                if(vs_mode==1){matrix[0]=matrix[5]=0x3f000000;matrix[3]=0xbe800000;matrix[7]=0x3e800000;}
                if(vs_mode==2){matrix[0]=matrix[5]=0x40000000;}
                if(vs_mode==3)matrix[15]=0x40000000;
                constants=(D3DDDIARG_SETVERTEXSHADERCONST){0,4};CHECK(functions.pfnSetVertexShaderConst(device,&constants,matrix)==S_OK);
                access=draw_lock(device,target,2,0,0);memset(access.pSurfData,0xa6,1024);
                u=(D3DDDIARG_UNLOCK){target,0,{0}};CHECK(functions.pfnUnlock(device,&u)==S_OK);
                D3DDDIARG_DRAWPRIMITIVE draw={D3DPT_TRIANGLELIST,0,2};
                CHECK(functions.pfnSetVertexShaderFunc(device,NULL)==S_OK);
                CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==D3DERR_NOTAVAILABLE);
                CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==S_OK);
                unsigned before=vs_draws;callback_fault=8;
                CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==E_ABORT && vs_draws==before);
                reenter=1;CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==S_OK);reenter=0;
                CHECK(vs_draws==before+1);vs_pixels(device,target);
            }
            CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
        }
        /* POSITIONT bypasses a still-bound VS; a stale transform must not
         * affect the old screen-space path or its pixel constant bank. */
        CHECK(functions.pfnSetPixelShader(device,shaders[0])==S_OK);
        D3DDDIVERTEXELEMENT transformed={0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0};
        D3DDDIARG_CREATEVERTEXSHADERDECL passthrough={1,NULL};
        CHECK(functions.pfnCreateVertexShaderDecl(device,&passthrough,&transformed)==S_OK);
        CHECK(functions.pfnSetVertexShaderDecl(device,passthrough.ShaderHandle)==S_OK);
        D3DDDIARG_LOCK a=draw_lock(device,vertex,2,0,0);
        const unsigned positions[6][4]={{0,0,0,0x3f800000},{0x41800000,0,0,0x3f800000},
            {0,0x41800000,0,0x3f800000},{0,0x41800000,0,0x3f800000},
            {0x41800000,0,0,0x3f800000},{0x41800000,0x41800000,0,0x3f800000}};
        for(unsigned i=0;i<6;i++)memcpy((BYTE *)a.pSurfData+16+i*32,positions[i],16);
        D3DDDIARG_UNLOCK unlock={vertex,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        a=draw_lock(device,target,2,0,0);memset(a.pSurfData,0xa6,1024);
        unlock=(D3DDDIARG_UNLOCK){target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        D3DDDIARG_DRAWPRIMITIVE draw={D3DPT_TRIANGLELIST,0,2};vs_bypass=1;
        CHECK(functions.pfnDrawPrimitive(device,&draw,NULL)==S_OK);
        vs_ps=vs_mode=0;vs_pixels(device,target);vs_bypass=0;
        CHECK(functions.pfnDeleteVertexShaderDecl(device,passthrough.ShaderHandle)==S_OK);
        callback_fault=5;CHECK(functions.pfnDeleteVertexShaderFunc(device,vs.ShaderHandle)==E_ABORT);
        CHECK(functions.pfnDeleteVertexShaderFunc(device,vs.ShaderHandle)==S_OK);
        CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==D3DERR_INVALIDCALL);
        CHECK(functions.pfnDeleteVertexShaderFunc(device,vs.ShaderHandle)==D3DERR_INVALIDCALL);
    }
    for(unsigned i=0;i<3;i++) {
        callback_fault=5;CHECK(functions.pfnDeletePixelShader(device,shaders[i])==E_ABORT);
        CHECK(functions.pfnDeletePixelShader(device,shaders[i])==S_OK);
    }
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    CHECK(functions.pfnDestroyResource(device,image.hResource)==S_OK);
}

void native_umd_vs_entry(void)
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
    if (!adapter) {report("UMDVS UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=lock_allocation;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=vs_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    vs_exercise(d.hDevice,a.hAdapter,&af,&d);
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==S_OK);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks && verified_programs==6);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    FreeLibrary(module);
    report("UMDVS checks=",checks); report("UMDVS draws=",vs_draws); report("UMDVS failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
