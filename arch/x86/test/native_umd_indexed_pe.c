/* D3D9 bound index buffers through the real driver callbacks. The model
 * checks ownership and publication; physical runs compare native GPU pixels. */
#define native_umd_vs_entry indexed_unused_vs_entry
#include "native_umd_vs_pe.c"
#undef native_umd_vs_entry

int _fltused;
static unsigned indexed_draws,indexed_programmable,indexed_stride,indexed_layout;
static D3DDDIARG_DRAWINDEXEDPRIMITIVE indexed_expected;
static BYTE indexed_bytes[128];

static HRESULT APIENTRY indexed_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8);
    unsigned textured=vs_ps==2,geometry=4+indexed_programmable,resources=geometry+textured;
    CHECK(args->CommandLength==1584+4*(ps_count+(indexed_programmable ? vs_count : 0)));
    CHECK(args->NumAllocations==resources && args->NumPatchLocations==resources+textured);
    CHECK(!args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    const intel_umd_draw_indexed_t *p=current_commands;
    CHECK(p->draw.magic==INTEL_UMD_COMMAND_MAGIC && p->draw.version==8 && p->draw.bytes==args->CommandLength);
    CHECK(p->draw.vertex_offset==16 && p->draw.vertex_stride==32 && p->draw.topology==(UINT)indexed_expected.PrimitiveType);
    CHECK(p->draw.vertex_count==(UINT)((long long)indexed_expected.BaseVertexIndex+indexed_expected.MinIndex+6));
    CHECK(p->index_count==(indexed_expected.PrimitiveType==D3DPT_TRIANGLELIST ? 6u : 4u));
    CHECK(p->index_offset==3*indexed_stride && p->index_stride==indexed_stride && !p->indices);
    CHECK(p->base_vertex==indexed_expected.BaseVertexIndex && p->min_index==indexed_expected.MinIndex && p->num_vertices==6);
    CHECK(p->parameter_count==ps_count && p->bound_mask==(textured ? 0x8001u : 0));
    CHECK(!p->draw.vertices && !p->draw.destination && !p->draw.program && !p->rectangle_count);
    if(indexed_programmable) {
        CHECK(p->vertex_parameter_count==vs_count && p->vertex_program_bytes>4096 && !p->vertex_program);
        CHECK(p->viewport_width==16 && p->viewport_height==16 && !p->viewport_x && !p->viewport_y);
        CHECK(p->attributes[0]==((3u<<16)|(indexed_layout ? 16u : 0)));
        CHECK(p->attributes[15]==((3u<<16)|(indexed_layout ? 0 : 16u)));
        for(unsigned i=1;i<15;i++) CHECK(!p->attributes[i]);
        for(unsigned i=0;i<8;i++) CHECK(!p->texcoord_offset[i]);
    } else {
        for(unsigned i=1456;i<1552;i++) CHECK(!((const BYTE *)p)[i]);
        CHECK(p->position_offset==(indexed_layout ? 16u : 0));
        CHECK(p->texcoord_offset[0]==(vs_ps ? (3u<<16)|(indexed_layout ? 0 : 16u) : 0));
    }
    for(unsigned i=0;i<ps_count;i++) CHECK(p->values[i]==ps_values[ps_map[i]]);
    for(unsigned i=0;indexed_programmable && i<vs_count;i++) CHECK(p->values[ps_count+i]==matrix[vs_map[i]]);
    for(unsigned i=0;i<resources;i++) {
        unsigned role=i<3 ? i : indexed_programmable && i==3 ? 19 : i==geometry-1 ? 20 : 3;
        unsigned offset=i<3 ? 80+8*i : role==19 ? 1456 : role==20 ? 1552 : 176;
        const D3DDDI_PATCHLOCATIONLIST *patch=&current_patches[i];
        CHECK(patch->AllocationIndex==i && patch->SlotId==role && patch->PatchOffset==offset);
        CHECK(!patch->AllocationOffset && !patch->DriverId && !patch->SplitOffset);
        CHECK(current_allocations[i].Value==(i==1));
        unsigned slot=0;while(slot<8 && backing[slot].allocation!=current_allocations[i].hAllocation) slot++;
        CHECK(slot<8 && !backing[slot].locked);
        for(unsigned j=0;j<i;j++) CHECK(current_allocations[i].hAllocation!=current_allocations[j].hAllocation);
        if(role==20) for(unsigned j=0;j<128;j++) CHECK(backing[slot].memory[j]==indexed_bytes[j]);
    }
    if(textured) {
        const D3DDDI_PATCHLOCATIONLIST *alias=&current_patches[resources];
        CHECK(alias->AllocationIndex==geometry && alias->SlotId==18 && alias->PatchOffset==176+15*80);
        CHECK(!alias->AllocationOffset && !alias->DriverId && !alias->SplitOffset);
    }
    unsigned next=indexed_draws&1;
    current_commands=draw_commands[next];current_allocations=draw_allocations[next];current_patches=draw_patches[next];
    memset(current_commands,0xa5,4096);
    args->pNewCommandBuffer=current_commands;args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations;args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches;args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0};q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength;q.allocation_count=args->NumAllocations;q.patch_count=args->NumPatchLocations;
    OKMT_S32 status=krender(&q);
    if(status==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    if(status) report("INDEXED KMT status=",status);
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands;args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations;args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches;args->NewPatchLocationListSize=q.patch_capacity;
#endif
    indexed_draws++;return S_OK;
}

static HANDLE indexed_resource(HANDLE device,unsigned stride)
{
    D3DDDI_SURFACEINFO surface={128,1,1,NULL,0,0};
    D3DDDIARG_CREATERESOURCE r={0};r.hResource=(HANDLE)0x9300;
    r.Format=stride==2 ? D3DDDIFMT_INDEX16 : D3DDDIFMT_INDEX32;
    r.Pool=D3DDDIPOOL_VIDEOMEMORY;r.pSurfList=&surface;r.SurfCount=r.MipLevels=1;
    r.Rotation=D3DDDI_ROTATION_IDENTITY;r.Flags.IndexBuffer=1;
    r.Flags.VertexBuffer=1;CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);r.Flags.VertexBuffer=0;
    r.Flags.Texture=1;CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);r.Flags.Texture=0;
    D3DDDIFORMAT format=r.Format;r.Format=D3DDDIFMT_VERTEXDATA;
    CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);r.Format=format;
    surface.Height=2;CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);surface.Height=1;
    r.MipLevels=2;CHECK(functions.pfnCreateResource(device,&r)==D3DERR_INVALIDCALL);r.MipLevels=1;
    callback_fault=4;CHECK(functions.pfnCreateResource(device,&r)==E_ABORT && r.hResource==(HANDLE)0x9300);
    CHECK(functions.pfnCreateResource(device,&r)==S_OK);return r.hResource;
}

static void indexed_negative(HANDLE device,HANDLE buffer,HANDLE vertex,HANDLE image)
{
    D3DDDIARG_SETINDICES bind={NULL,0};
    CHECK(functions.pfnSetIndices(device,&bind)==S_OK);
    CHECK(functions.pfnDrawIndexedPrimitive(device,&indexed_expected)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetIndices(device,NULL)==D3DERR_INVALIDCALL);
    bind=(D3DDDIARG_SETINDICES){vertex,indexed_stride};CHECK(functions.pfnSetIndices(device,&bind)==D3DERR_INVALIDCALL);
    bind.hIndexBuffer=image;CHECK(functions.pfnSetIndices(device,&bind)==D3DERR_INVALIDCALL);
    bind.hIndexBuffer=buffer;CHECK(functions.pfnSetIndices(device,&bind)==S_OK);
    bind.Stride=indexed_stride==2 ? 4 : 2;CHECK(functions.pfnSetIndices(device,&bind)==D3DERR_INVALIDCALL);
    bind.hIndexBuffer=(HANDLE)0xdead;CHECK(functions.pfnSetIndices(device,&bind)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDrawIndexedPrimitive(device,NULL)==D3DERR_INVALIDCALL);
    for(unsigned i=0;i<7;i++) {
        D3DDDIARG_DRAWINDEXEDPRIMITIVE bad=indexed_expected;
        if(i==0) bad.StartIndex=0xffffffffu;
        if(i==1) bad.PrimitiveCount=0xffffffffu;
        if(i==2) bad.NumVertices=0;
        if(i==3) {bad.MinIndex=0xffffffffu;bad.NumVertices=2;}
        if(i==4) {bad.BaseVertexIndex=-1;bad.MinIndex=0;}
        if(i==5) {bad.BaseVertexIndex=0x7fffffff;bad.MinIndex=0;}
        if(i==6) bad.StartIndex=128/indexed_stride-1;
        CHECK(functions.pfnDrawIndexedPrimitive(device,&bad)==D3DERR_INVALIDCALL);
    }
    D3DDDIARG_LOCK lock={0};lock.hResource=buffer;lock.Flags.RangeValid=1;lock.Range.Offset=120;lock.Range.Size=16;
    CHECK(functions.pfnLock(device,&lock)==D3DERR_INVALIDCALL);
    lock.Flags.Value=0;lock.Flags.AreaValid=1;
    CHECK(functions.pfnLock(device,&lock)==D3DERR_INVALIDCALL);
    lock=draw_lock(device,buffer,1,3*indexed_stride,6*indexed_stride);
    CHECK(functions.pfnDrawIndexedPrimitive(device,&indexed_expected)==D3DERR_INVALIDCALL);
    D3DDDIARG_UNLOCK unlock={buffer,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    callback_fault=5;CHECK(functions.pfnDestroyResource(device,buffer)==E_ABORT);
}

static void indexed_exercise(HANDLE device,HANDLE adapter_handle,const D3DDDI_ADAPTERFUNCS *af,
                             const D3DDDIARG_CREATEDEVICE *primary)
{
    CHECK(functions.pfnSetIndices && functions.pfnDrawIndexedPrimitive);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    HANDLE vertex=draw_resource(device,0x9000,TRUE),target=draw_resource(device,0x9100,FALSE);
    D3DDDI_SURFACEINFO surface={2,2,1,NULL,0,0};
    D3DDDIARG_CREATERESOURCE image={0};image.hResource=(HANDLE)0x9200;image.Format=D3DDDIFMT_A8R8G8B8;
    image.Pool=D3DDDIPOOL_VIDEOMEMORY;image.pSurfList=&surface;image.SurfCount=image.MipLevels=1;
    image.Rotation=D3DDDI_ROTATION_IDENTITY;image.Flags.Texture=1;
    CHECK(functions.pfnCreateResource(device,&image)==S_OK);
    D3DDDIARG_LOCK access=draw_lock(device,image.hResource,2,0,0);
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++)((unsigned *)((BYTE *)access.pSurfData+y*access.Pitch))[x]=0xff804020;
    D3DDDIARG_UNLOCK unlock={image.hResource,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    CHECK(functions.pfnSetTexture(device,0,image.hResource)==S_OK);
    CHECK(functions.pfnSetTexture(device,15,image.hResource)==S_OK);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0};CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    D3DDDIARG_VIEWPORTINFO viewport={0,0,16,16};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    D3DDDIARG_SETPIXELSHADERCONST pc={0,1};CHECK(functions.pfnSetPixelShaderConst(device,&pc,(const void *)ps_values)==S_OK);
    for(unsigned pipeline=0;pipeline<3;pipeline++) {
        indexed_programmable=pipeline!=0;vs_profile=pipeline==2 ? 3 : 2;
        D3DDDIARG_CREATEVERTEXSHADERFUNC vs={vs_profile==2 ? sizeof(vs2_code) : sizeof(vs3_code),NULL};
        CHECK(functions.pfnCreateVertexShaderFunc(device,&vs,vs_profile==2 ? vs2_code : vs3_code)==S_OK);
        CHECK(functions.pfnSetVertexShaderFunc(device,vs.ShaderHandle)==S_OK);
        for(indexed_stride=2;indexed_stride<=4;indexed_stride+=2) {
            HANDLE buffer=indexed_resource(device,indexed_stride);
            D3DDDI_DEVICEFUNCS other_funcs;D3DDDIARG_CREATEDEVICE other=*primary;
            other.hDevice=runtime_device;other.pDeviceFuncs=&other_funcs;
            CHECK(af->pfnCreateDevice(adapter_handle,&other)==S_OK);
            D3DDDIARG_SETINDICES bind={buffer,indexed_stride};
            CHECK(other_funcs.pfnSetIndices(other.hDevice,&bind)==D3DERR_INVALIDCALL);
            CHECK(other_funcs.pfnDestroyDevice(other.hDevice)==S_OK);
            for(unsigned topology=4;topology<=6;topology++) for(vs_mode=0;vs_mode<4;vs_mode++) {
                vs_ps=vs_mode%3;indexed_layout=vs_mode&1;
                D3DDDIARG_CREATEPIXELSHADER ps={vs_ps==2 ? sizeof(ps_sampled) : vs_ps ? sizeof(ps_input) : sizeof(ps_plain),NULL};
                CHECK(functions.pfnCreatePixelShader(device,&ps,vs_ps==2 ? ps_sampled : vs_ps ? ps_input : ps_plain)==S_OK);
                CHECK(functions.pfnSetPixelShader(device,ps.ShaderHandle)==S_OK);
                D3DDDIVERTEXELEMENT e[2]={{0,indexed_layout ? 16 : 0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,
                    indexed_programmable ? D3DDECLUSAGE_POSITION : D3DDECLUSAGE_POSITIONT,0},
                    {0,indexed_layout ? 0 : 16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}};
                D3DDDIARG_CREATEVERTEXSHADERDECL decl={2,NULL};CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,e)==S_OK);
                CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
                D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,16,32};CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
                indexed_expected=(D3DDDIARG_DRAWINDEXEDPRIMITIVE){(D3DPRIMITIVETYPE)topology,0,0,6,3,2};
                if(vs_mode==0) indexed_expected.BaseVertexIndex=1;
                if(vs_mode==1) {indexed_expected.BaseVertexIndex=-2;indexed_expected.MinIndex=2;}
                if(vs_mode==2) {
                    indexed_expected.BaseVertexIndex=indexed_stride==2 ? -65530 : (-2147483647-1);
                    indexed_expected.MinIndex=indexed_stride==2 ? 65530u : 0x80000000u;
                }
                unsigned first=(UINT)((long long)indexed_expected.BaseVertexIndex+indexed_expected.MinIndex);
                access=draw_lock(device,vertex,2,0,0);memset(access.pSurfData,0x5a,256);
                const unsigned order[6]={0,1,2,2,1,3};
                const unsigned uv[4]={0x3f200000,0x3f400000,0x3e800000,0x3f800000};
                for(unsigned i=0;i<6;i++) {
                    float left=vs_mode==1 ? 2.0f : vs_mode==3 ? 4.0f : 0.0f;
                    float right=vs_mode==1 ? 10.0f : vs_mode==3 ? 12.0f : 16.0f;
                    float pos[4]={indexed_programmable ? (order[i]&1 ? 1.0f : -1.0f) : (order[i]&1 ? right : left),
                        indexed_programmable ? (order[i]&2 ? -1.0f : 1.0f) : (order[i]&2 ? right : left),0.5f,1.0f};
                    BYTE *v=(BYTE *)access.pSurfData+16+(first+i)*32;
                    memcpy(v+(indexed_layout ? 16 : 0),pos,16);memcpy(v+(indexed_layout ? 0 : 16),uv,16);
                }
                unlock=(D3DDDIARG_UNLOCK){vertex,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
                memset(indexed_bytes,0x5c,sizeof(indexed_bytes));
                const unsigned list[6]={0,1,2,3,4,5},strip[4]={0,1,2,5},fan[4]={0,1,5,2};
                const unsigned *indices=topology==4 ? list : topology==5 ? strip : fan;
                for(unsigned i=0;i<(topology==4 ? 6u : 4u);i++) {
                    unsigned value=indices[i]+indexed_expected.MinIndex;
                    for(unsigned b=0;b<indexed_stride;b++) indexed_bytes[(3+i)*indexed_stride+b]=(BYTE)(value>>(8*b));
                }
                access=draw_lock(device,buffer,2,0,0);memcpy(access.pSurfData,indexed_bytes,sizeof(indexed_bytes));
                unlock=(D3DDDIARG_UNLOCK){buffer,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
                memset(matrix,0,sizeof(matrix));matrix[0]=matrix[5]=matrix[10]=matrix[15]=0x3f800000;
                if(vs_mode==1){matrix[0]=matrix[5]=0x3f000000;matrix[3]=0xbe800000;matrix[7]=0x3e800000;}
                if(vs_mode==2)matrix[0]=matrix[5]=0x40000000;
                if(vs_mode==3)matrix[15]=0x40000000;
                D3DDDIARG_SETVERTEXSHADERCONST constants={0,4};CHECK(functions.pfnSetVertexShaderConst(device,&constants,matrix)==S_OK);
                access=draw_lock(device,target,2,0,0);memset(access.pSurfData,0xa6,1024);
                unlock=(D3DDDIARG_UNLOCK){target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
                indexed_negative(device,buffer,vertex,image.hResource);
                unsigned before=indexed_draws;callback_fault=8;
                CHECK(functions.pfnDrawIndexedPrimitive(device,&indexed_expected)==E_ABORT && indexed_draws==before);
                reenter=1;CHECK(functions.pfnDrawIndexedPrimitive(device,&indexed_expected)==S_OK);reenter=0;
                CHECK(indexed_draws==before+1);vs_pixels(device,target);
                access=draw_lock(device,buffer,1,0,0);
                for(unsigned i=0;i<128;i++) CHECK(((const BYTE *)access.pSurfData)[i]==indexed_bytes[i]);
                unlock=(D3DDDIARG_UNLOCK){buffer,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
                CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
                CHECK(functions.pfnDeletePixelShader(device,ps.ShaderHandle)==S_OK);
            }
            CHECK(functions.pfnDestroyResource(device,buffer)==S_OK);
            CHECK(functions.pfnSetIndices(device,&bind)==D3DERR_INVALIDCALL);
            bind=(D3DDDIARG_SETINDICES){NULL,99};CHECK(functions.pfnSetIndices(device,&bind)==S_OK);
        }
        CHECK(functions.pfnDeleteVertexShaderFunc(device,vs.ShaderHandle)==S_OK);
    }
    CHECK(indexed_draws==72);
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    CHECK(functions.pfnDestroyResource(device,image.hResource)==S_OK);
}

void native_umd_indexed_entry(void)
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
    if (!adapter) {report("UMDINDEXED UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=lock_allocation;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=indexed_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    indexed_exercise(d.hDevice,a.hAdapter,&af,&d);
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==S_OK);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks && verified_programs==111);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    FreeLibrary(module);
    report("UMDINDEXED checks=",checks); report("UMDINDEXED draws=",indexed_draws); report("UMDINDEXED failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
