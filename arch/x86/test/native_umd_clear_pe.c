/* Clear DDI packet/lifetime tests and independent physical pixel checks. */
static void clear_verify(const void *,unsigned);
#define NATIVE_UMD_VERIFY_PROGRAM clear_verify
#define native_umd_draw_entry unused_draw_entry
#include "native_umd_draw_pe.c"
#undef native_umd_draw_entry

int _fltused;

static unsigned clear_count,expected_pixels[256],checked_pixels;
static unsigned expected_x,expected_y,expected_width,expected_height,expected_color;
static unsigned application_draw;
static intel_umd_rectangle_t expected_rectangles[32];
static unsigned expected_rect_count;
static DWORD (WINAPI *diagnostic_clock)(void);
static unsigned model_busy_once;
static unsigned diagnostic_kmt_calls,diagnostic_kmt_busy,diagnostic_kmt_status;

static void clear_verify(const void *memory,unsigned size)
{
    const BYTE *bytes=memory;
    if (size==4096) {
        const FLOAT *quad=memory;
        for (unsigned i=0;i<4;i++) {
            CHECK(quad[i*4]==(i&1 ? 4095.5f : -0.5f));
            CHECK(quad[i*4+1]==(i&2 ? 4095.5f : -0.5f));
            CHECK(quad[i*4+2]==0 && quad[i*4+3]==1);
        }
        for (unsigned i=64;i<size;i++) CHECK(!bytes[i]);
    } else {
        const struct intel_d3d9_program *p=memory;
        CHECK(size==8192 && p->abi==2 && p->header_bytes==128 && p->code_offset==4096);
        CHECK(p->bytecode_offset==128 && p->simd_mask==3 && p->code_bytes>0);
        if (p->parameter_count) {
            const unsigned code[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
            CHECK(p->bytecode_bytes==sizeof(code) && p->parameter_count==4 && p->curb_read_length==1);
            for (unsigned i=0;i<sizeof(code);i++) CHECK(bytes[128+i]==((const BYTE *)code)[i]);
            const unsigned *map=(const void *)(bytes+p->parameter_map_offset);
            for (unsigned i=0;i<4;i++) CHECK(map[i]==i);
        } else {
            CHECK(p->bytecode_bytes==44 && p->code_bytes==sizeof(reference_native));
            for (unsigned i=0;i<p->code_bytes;i++) CHECK(bytes[4096+i]==reference_native[i]);
        }
        for (unsigned i=p->allocation_bytes;i<size;i++) CHECK(!bytes[i]);
    }
}

static HRESULT APIENTRY clear_render(HANDLE h,D3DDDICB_RENDER *args)
{
    if (application_draw) return draw_render(h,args);
    GUARD(8);
#ifdef UMD_MODEL
    if(clear_count==4 && !model_busy_once++)return D3DERR_WASSTILLDRAWING;
#endif
    CHECK(args->CommandLength==152+expected_rect_count*16 && args->NumAllocations==3 && args->NumPatchLocations==3 &&
          !args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    const intel_umd_draw_constants_t *q=current_commands;
    CHECK(q->draw.magic==INTEL_UMD_COMMAND_MAGIC && q->draw.version==(expected_rect_count ? 3u : 2u) &&
          q->draw.opcode==2 && q->draw.bytes==args->CommandLength);
    CHECK(q->draw.destination_width==16 && q->draw.destination_height==16 && q->draw.destination_pitch==64);
    if (expected_rect_count) {
        CHECK(!q->draw.x && !q->draw.y && q->draw.width==16 && q->draw.height==16);
        const intel_umd_rectangle_t *r=(const void *)((const BYTE *)q+152);
        for (unsigned i=0;i<expected_rect_count;i++)
            CHECK(r[i].x==expected_rectangles[i].x && r[i].y==expected_rectangles[i].y &&
                  r[i].width==expected_rectangles[i].width && r[i].height==expected_rectangles[i].height);
    } else CHECK(q->draw.x==expected_x && q->draw.y==expected_y && q->draw.width==expected_width && q->draw.height==expected_height);
    CHECK(q->draw.vertex_stride==16 && !q->draw.vertex_offset && q->draw.vertex_count==4 && q->draw.topology==D3DPT_TRIANGLESTRIP);
    CHECK(q->parameter_count==4 && q->reserved==expected_rect_count && !q->draw.vertices && !q->draw.destination && !q->draw.program);
    CHECK(!q->draw.reserved0 && !q->draw.reserved1[0] && !q->draw.reserved1[1]);
    for (unsigned i=0;i<6;i++) CHECK(!q->draw.reserved2[i]);
    const FLOAT *rgba=(const void *)q->values;
    unsigned shift[]={16,8,0,24};
    for (unsigned i=0;i<4;i++) CHECK((unsigned)(rgba[i]*255.0f+0.5f)==((expected_color>>shift[i])&255));
    unsigned slots[3];
    for (unsigned i=0;i<3;i++) {
        const D3DDDI_PATCHLOCATIONLIST *p=current_patches+i;
        CHECK(p->AllocationIndex==i && p->SlotId==i && p->PatchOffset==80+i*8 &&
              !p->DriverId && !p->AllocationOffset && !p->SplitOffset);
        CHECK(current_allocations[i].Value==(i==1));
        unsigned slot=0;while(slot<8 && backing[slot].allocation!=current_allocations[i].hAllocation)slot++;
        CHECK(slot<8 && backing[slot].allocation && !backing[slot].locked);slots[i]=slot;
    }
    CHECK(slots[0]!=slots[1] && slots[0]!=slots[2] && slots[1]!=slots[2]);
    CHECK(backing[slots[0]].bytes==4096 && backing[slots[1]].bytes==4096 && backing[slots[2]].bytes==8192);
    unsigned next=clear_count&1;
    current_commands=draw_commands[next];current_allocations=draw_allocations[next];current_patches=draw_patches[next];
    memset(current_commands,0xa5,4096);
    args->pNewCommandBuffer=current_commands;args->NewCommandBufferSize=4096;
    args->pNewAllocationList=current_allocations;args->NewAllocationListSize=64;
    args->pNewPatchLocationList=current_patches;args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0};q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength;q.allocation_count=3;q.patch_count=3;
    OKMT_S32 status=krender(&q);
    diagnostic_kmt_calls++; diagnostic_kmt_status=(unsigned)status; if(status==(OKMT_S32)0x80000011)diagnostic_kmt_busy++;
    if(status==(OKMT_S32)0x80000011)return D3DERR_WASSTILLDRAWING;
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands;args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations;args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches;args->NewPatchLocationListSize=q.patch_capacity;
#endif
    clear_count++;return S_OK;
}

static void clear_pixels(HANDLE device,HANDLE target)
{
#ifndef UMD_MODEL
    unsigned equal=0,bad=0,seen=0;
    for(unsigned attempt=0;attempt<300 && !equal;attempt++) {
        Sleep(10);D3DDDIARG_LOCK access=draw_lock(device,target,1,0,0);CHECK(access.Pitch==64);
        equal=1;const volatile unsigned *pixels=access.pSurfData;
        for(unsigned i=0;i<256;i++)if(pixels[i]!=expected_pixels[i]){equal=0;bad=i;seen=pixels[i];break;}
        D3DDDIARG_UNLOCK unlock={target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    }
    if(!equal){report("CLEAR bad index=",bad);report("CLEAR expected=",expected_pixels[bad]);report("CLEAR actual=",seen);}
    CHECK(equal);checked_pixels+=256;
#else
    (void)device;(void)target;
#endif
}

static void clear_expected(unsigned color,unsigned x,unsigned y,unsigned width,unsigned height)
{
    expected_color=color;expected_x=x;expected_y=y;expected_width=width;expected_height=height;
    for(unsigned i=0;i<256;i++)
        if(i%16>=x && i%16<x+width && i/16>=y && i/16<y+height)expected_pixels[i]=color;
}

static void clear_call(HANDLE device,const D3DDDIARG_CLEAR *args,UINT count,const RECT *rects)
{
    HRESULT result=D3DERR_WASSTILLDRAWING;
    unsigned before=clear_count,busy_before=diagnostic_kmt_busy,attempt=0;
    DWORD started=diagnostic_clock();
    for(;attempt<300 && result==D3DERR_WASSTILLDRAWING;attempt++) {
        result=functions.pfnClear(device,args,count,rects);
        if(result==D3DERR_WASSTILLDRAWING)Sleep(10);
    }
    if(result!=S_OK) { report("CLEAR attempts=",attempt); report("CLEAR before=",before); report("CLEAR busy in call=",diagnostic_kmt_busy-busy_before); report("CLEAR elapsed milliseconds=",diagnostic_clock()-started); report("CLEAR rect count=",count); report("CLEAR submit result=",(unsigned)result); report("CLEAR accepted before failure=",clear_count); report("CLEAR KMT calls=",diagnostic_kmt_calls); report("CLEAR KMT busy=",diagnostic_kmt_busy); report("CLEAR KMT status=",diagnostic_kmt_status); }
    CHECK(result==S_OK);
}

static void clear_exercise(HANDLE device)
{
    CHECK(functions.pfnClear);
    D3DDDIARG_CLEAR args={1,0x12345678,1.0f,0};RECT full={0,0,16,16};
    CHECK(functions.pfnClear((HANDLE)0xdead,&args,1,&full)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnClear(device,NULL,0,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnClear(device,&args,1,NULL)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnClear(device,&args,0,(const RECT *)1)==S_OK && !clear_count);
    CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_INVALIDCALL);
    HANDLE target=draw_resource(device,0xb001,FALSE),inactive=draw_resource(device,0xb002,FALSE);
    D3DDDIARG_SETRENDERTARGET rt={0,target,0};CHECK(functions.pfnSetRenderTarget(device,&rt)==S_OK);
    for(unsigned bank=0;bank<2;bank++) {
        D3DDDIARG_LOCK access=draw_lock(device,bank ? inactive : target,2,0,0);
        for(unsigned i=0;i<256;i++)((unsigned *)access.pSurfData)[i]=0xa6a6a6a6;
        D3DDDIARG_UNLOCK unlock={bank ? inactive : target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
        if(!bank)for(unsigned i=0;i<256;i++)expected_pixels[i]=0xa6a6a6a6;
    }
    args.Flags=0x10;CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_INVALIDCALL);
    args.Flags=3;CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_NOTAVAILABLE);
    args.Flags=5;CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_NOTAVAILABLE);
    args.Flags=9;CHECK(functions.pfnClear(device,&args,0,NULL)==D3DERR_INVALIDCALL);
    D3DDDIARG_VIEWPORTINFO viewport={2,3,11,10};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    args.Flags=1;RECT empty={4,4,4,5},outside={17,0,20,8};
    CHECK(functions.pfnClear(device,&args,1,&empty)==S_OK && !clear_count);
    CHECK(functions.pfnClear(device,&args,1,&outside)==S_OK && !private_allocations);
    RECT invalid[2]={{0,0,4,4},{4,4,3,5}};
    CHECK(functions.pfnClear(device,&args,2,invalid)==D3DERR_INVALIDCALL && !clear_count);
    D3DDDIARG_LOCK locked=draw_lock(device,target,2,0,0);
    CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_INVALIDCALL);
    D3DDDIARG_UNLOCK unlock={target,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);(void)locked;
    /* Recover every partial private allocation/upload before publishing work. */
    clear_expected(args.FillColor,0,0,16,16);
    callback_fault=4;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT);
    callback_fault=6;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT);
    null_mapping=1;CHECK(functions.pfnClear(device,&args,1,&full)==(HRESULT)0x88760827);
    callback_fault=7;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT);
    callback_fault=4;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT);
    callback_fault=7;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT);
    callback_fault=8;CHECK(functions.pfnClear(device,&args,1,&full)==E_ABORT && !clear_count);
    reenter=1;clear_call(device,&args,1,&full);reenter=0;CHECK(clear_count==1);clear_pixels(device,target);
    CHECK(private_allocations==2 && verified_programs==2);
    args.Flags=9;args.FillColor=0x80ef2917;clear_expected(args.FillColor,2,3,11,10);
    clear_call(device,&args,0,(const RECT *)1);clear_pixels(device,target);
    RECT scissor={5,4,10,9};CHECK(functions.pfnSetScissorRect(device,&scissor)==S_OK);
    draw_state(device,D3DDDIRS_SCISSORTESTENABLE,1);
    args.FillColor=0xff07fd80;clear_expected(args.FillColor,5,4,5,5);
    clear_call(device,&args,1,&full);clear_pixels(device,target);
    RECT negative={-2000000000,-2000000000,7,6};args.Flags=1;args.FillColor=0x013fbe02;
    clear_expected(args.FillColor,0,0,7,6);clear_call(device,&args,1,&negative);clear_pixels(device,target);
    RECT multiple[2]={{-100,-100,16,16},{0,0,2000000000,2000000000}};
    args.FillColor=0xa58001fe;clear_expected(args.FillColor,0,0,16,16);
    expected_rect_count=2;expected_rectangles[0]=expected_rectangles[1]=(intel_umd_rectangle_t){0,0,16,16};
    callback_fault=8;CHECK(functions.pfnClear(device,&args,2,multiple)==E_ABORT && clear_count==4);
    clear_call(device,&args,2,multiple);CHECK(clear_count==5);clear_pixels(device,target);
    expected_rect_count=0;
    /* Full batch capacity, separated pixels, plus overflow with no admitted
     * prefix. This also checks each scissor's exclusive right/bottom edge. */
    RECT sparse[33];args.FillColor=0x70ff34ab;
    for (unsigned i=0;i<33;i++) {
        unsigned x=(i%8)*2,y=(i/8)*2;sparse[i]=(RECT){x,y,x+1,y+1};
        if(i<32)expected_rectangles[i]=(intel_umd_rectangle_t){x,y,1,1};
    }
    CHECK(functions.pfnClear(device,&args,33,sparse)==D3DERR_NOTAVAILABLE && clear_count==5);
    for (unsigned i=0;i<32;i++)clear_expected(args.FillColor,(i%8)*2,(i/8)*2,1,1);
    expected_rect_count=32;clear_call(device,&args,32,sparse);CHECK(clear_count==6);
    expected_rect_count=0;clear_pixels(device,target);
    args.Flags=0;unsigned before=clear_count;clear_call(device,&args,1,&full);CHECK(clear_count==before);
    scissor=(RECT){15,15,16,16};CHECK(functions.pfnSetScissorRect(device,&scissor)==S_OK);
    args.Flags=9;clear_call(device,&args,0,NULL);CHECK(clear_count==before);
    /* 256 queued colors exercise every UNORM8 channel value and immutable
     * per-job constants; the test CPU never computes rendered output. */
    args.Flags=1;
    for(unsigned i=0;i<256;i++) {
        unsigned x=i%16,y=i/16;RECT pixel={(LONG)x,(LONG)y,(LONG)x+1,(LONG)y+1};
        args.FillColor=(i<<24)|((255-i)<<16)|(((i*73)&255)<<8)|(i^0xa5);
        clear_expected(args.FillColor,x,y,1,1);clear_call(device,&args,1,&pixel);
    }
    clear_pixels(device,target);CHECK(clear_count==262);
    /* Clear with precomputed rectangles must preserve all application draw
     * bindings, constants, viewport and scissor. Render a different shader. */
    HANDLE vertex=draw_resource(device,0xb003,TRUE);
    D3DDDIARG_LOCK access=draw_lock(device,vertex,2,0,0);
    const FLOAT tri[3][4]={{1.25f,1.25f,0,1},{14.25f,1.25f,0,1},{1.25f,14.25f,0,1}};
    memcpy(access.pSurfData,tri,sizeof(tri));unlock=(D3DDDIARG_UNLOCK){vertex,0,{0}};CHECK(functions.pfnUnlock(device,&unlock)==S_OK);
    D3DDDIVERTEXELEMENT element={0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITIONT,0};
    D3DDDIARG_CREATEVERTEXSHADERDECL decl={1,NULL};CHECK(functions.pfnCreateVertexShaderDecl(device,&decl,&element)==S_OK);
    CHECK(functions.pfnSetVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    D3DDDIARG_SETSTREAMSOURCE stream={0,vertex,0,16};CHECK(functions.pfnSetStreamSource(device,&stream)==S_OK);
    const unsigned code[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,0x02000001,0x800f0800,0xa0e40000,0xffff};
    D3DDDIARG_CREATEPIXELSHADER shader={sizeof(code),NULL};CHECK(functions.pfnCreatePixelShader(device,&shader,code)==S_OK);
    CHECK(functions.pfnSetPixelShader(device,shader.ShaderHandle)==S_OK);
    draw_state(device,D3DDDIRS_ZENABLE,0);draw_state(device,D3DDDIRS_CULLMODE,D3DCULL_NONE);
    viewport=(D3DDDIARG_VIEWPORTINFO){4,3,8,9};CHECK(functions.pfnSetViewport(device,&viewport)==S_OK);
    scissor=(RECT){5,4,11,10};CHECK(functions.pfnSetScissorRect(device,&scissor)==S_OK);
    args.FillColor=0xff0000ff;clear_expected(args.FillColor,0,0,16,16);clear_call(device,&args,1,&full);
    expected_draw=(intel_umd_draw32_t){0};expected_draw.magic=INTEL_UMD_COMMAND_MAGIC;
    expected_draw.version=1;expected_draw.opcode=2;expected_draw.bytes=128;
    expected_draw.destination_width=expected_draw.destination_height=16;expected_draw.destination_pitch=64;
    expected_draw.x=5;expected_draw.y=4;expected_draw.width=expected_draw.height=6;
    expected_draw.vertex_stride=16;expected_draw.vertex_count=3;expected_draw.topology=D3DPT_TRIANGLELIST;
    expected_draw.program_bytes=4096+sizeof(reference_native);expected_draw.dispatch_mask=3;
    D3DDDIARG_DRAWPRIMITIVE primitive={D3DPT_TRIANGLELIST,0,1};application_draw=1;
    CHECK(functions.pfnDrawPrimitive(device,&primitive,NULL)==S_OK);application_draw=0;
    for(unsigned i=0;i<256;i++) {unsigned x=i%16,y=i/16;if(x>=5 && x<11 && y>=4 && y<10 && x+y<=15)expected_pixels[i]=0xffff0000;}
    clear_pixels(device,target);
    CHECK(functions.pfnDestroyDevice(device)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDeletePixelShader(device,shader.ShaderHandle)==S_OK);
    CHECK(functions.pfnDeleteVertexShaderDecl(device,decl.ShaderHandle)==S_OK);
    CHECK(functions.pfnDestroyResource(device,vertex)==S_OK);
    CHECK(functions.pfnDestroyResource(device,target)==S_OK);
    for(unsigned i=0;i<256;i++)expected_pixels[i]=0xa6a6a6a6;
    clear_pixels(device,inactive);CHECK(functions.pfnDestroyResource(device,inactive)==S_OK);
    CHECK(functions.pfnClear(device,&args,1,&full)==D3DERR_INVALIDCALL);
}

void native_umd_clear_entry(void)
{
    SetErrorMode(0x8001);
    HMODULE clock_module=LoadLibraryA("kernel32.dll"); CHECK(clock_module);
    diagnostic_clock=(void *)GetProcAddress(clock_module,"GetTickCount"); CHECK(diagnostic_clock);
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
    if (!adapter) {report("CLEAR UNAVAILABLE adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af;
    CHECK(open_adapter(&a)==S_OK);
    D3DDDIARG_GETCAPS caps={0}; CHECK(af.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=draw_lock_callback;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=clear_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    clear_exercise(d.hDevice);
    callback_fault=5; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==S_OK);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks && verified_programs==3);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    FreeLibrary(module); FreeLibrary(clock_module);
    report("CLEAR checks=",checks); report("CLEAR draws=",clear_count); report("CLEAR application draws=",draw_count); report("CLEAR pixels=",checked_pixels); report("CLEAR failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
