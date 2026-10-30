/* D3D9 DDI client. The runtime callbacks are a test harness, not a substitute
 * Direct3D runtime. UMD_MODEL uses host memory; the normal build calls public
 * KMT on Osito and requires the physical native prototype adapter. */
#define WIN32_LEAN_AND_MEAN
#define D3D_UMD_INTERFACE_VERSION 0x000c
#include <windows.h>
#include <d3d9.h>
#include <d3dumddi.h>
#include "../win32/d3dkmt_abi.h"
#include "../drivers/intel_gpu_umd_abi.h"
#include "../lib/intel_d3d9/compiler_api.h"
#include "native_umd_shader_reference.h"

static unsigned checks,failures,callback_fault,query_corrupt,submitted,reenter;
static HANDLE runtime_device=(HANDLE)0x7001;
static D3DDDI_DEVICEFUNCS functions;
static HANDLE active_device;
static struct { HANDLE runtime; OKMT_HANDLE group,allocation; unsigned bytes,locked; void *mapping; unsigned char memory[8192]; } backing[8];
static unsigned private_allocations,private_frees,private_locks,private_unlocks,verified_programs,null_mapping;
static unsigned char model_commands[4096];
static D3DDDI_ALLOCATIONLIST model_allocations[64];
static D3DDDI_PATCHLOCATIONLIST model_patches[64];
static OKMT_HANDLE kernel_device,adapter;
static unsigned adapter_node;
static OKMT_S32 (WINAPI *kquery)(OKMT_QUERY_ADAPTER_INFO *);
static OKMT_S32 (WINAPI *kcontext)(OKMT_CREATE_CONTEXT *);
static OKMT_S32 (WINAPI *kdestroy_context)(OKMT_DESTROY_CONTEXT *);
static OKMT_S32 (WINAPI *kallocate)(OKMT_CREATE_ALLOCATION *);
static OKMT_S32 (WINAPI *kdeallocate)(OKMT_DESTROY_ALLOCATION *);
static OKMT_S32 (WINAPI *klock)(OKMT_LOCK *);
static OKMT_S32 (WINAPI *kunlock)(OKMT_UNLOCK *);
static OKMT_S32 (WINAPI *krender)(OKMT_RENDER *);

static void report(const char *text,unsigned value)
{
    DWORD bytes,n=0; while(text[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&bytes,NULL);
    char digits[9]; for (unsigned i=0;i<8;i++) digits[i]="0123456789abcdef"[(value>>(28-i*4))&15];
    digits[8]='\n'; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),digits,9,&bytes,NULL);
}
static void check(int valid,unsigned line)
{
    checks++;
    if (!valid) { failures++; report("UMD FAIL line=",line); ExitProcess(1); }
}
#define CHECK(x) check(!!(x),__LINE__)
#define GUARD(stage) do { CHECK(h==runtime_device); if (reenter) CHECK(functions.pfnDestroyDevice(active_device)==(HRESULT)0x8876021c); \
    if (callback_fault==stage) {callback_fault=0;return E_ABORT;} } while(0)

static void verify_program(unsigned slot)
{
#ifdef NATIVE_UMD_VERIFY_PROGRAM
    NATIVE_UMD_VERIFY_PROGRAM(backing[slot].mapping,backing[slot].bytes);
#else
    const unsigned char *bytes=backing[slot].mapping;
    const struct intel_d3d9_program *program=(const void *)bytes;
    CHECK(backing[slot].bytes==8192 && bytes);
    CHECK(program->bytecode_offset==128 && program->bytecode_bytes==44 && program->code_offset==4096);
    CHECK(program->code_bytes==sizeof(reference_native) && program->allocation_bytes==4096+sizeof(reference_native));
    const unsigned *words=(const void *)bytes;
    CHECK(words[0]==INTEL_D3D9_COMPILER_ABI);
    for (unsigned i=1;i<32;i++) if (i!=2 && i!=6) CHECK(words[i]==reference_header[i]);
    const unsigned code[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
                           0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    for (unsigned i=0;i<sizeof(code);i++) CHECK(bytes[128+i]==((const unsigned char *)code)[i]);
    for (unsigned i=172;i<4096;i++) CHECK(!bytes[i]);
    for (unsigned i=0;i<sizeof(reference_native);i++) CHECK(bytes[4096+i]==reference_native[i]);
    for (unsigned i=program->allocation_bytes;i<backing[slot].bytes;i++) CHECK(!bytes[i]);
#endif
    verified_programs++;
}

static HRESULT APIENTRY query_adapter(HANDLE h,const D3DDDICB_QUERYADAPTERINFO *args)
{
    CHECK(h==(HANDLE)(UINT_PTR)adapter); CHECK(args->PrivateDriverDataSize==32);
    if (callback_fault==1) {callback_fault=0;return E_ABORT;}
    intel_umd_adapter_description_t *info=args->pPrivateDriverData;
#ifdef UMD_MODEL
#ifdef UMD_RCS_MODEL
    *info=(intel_umd_adapter_description_t){INTEL_UMD_ADAPTER_MAGIC,1,32,INTEL_UMD_CAP_RCS_COPY32,0x8086,0x3e9b,0,0};
#else
    *info=(intel_umd_adapter_description_t){INTEL_UMD_ADAPTER_MAGIC,1,32,INTEL_UMD_CAP_COPY32,0x8086,0x3e9b,1,0};
#endif
#else
    OKMT_QUERY_ADAPTER_INFO q={adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,info,32};
    CHECK(kquery(&q)==0);
#endif
    CHECK((info->capabilities==INTEL_UMD_CAP_COPY32 && info->copy_node==INTEL_UMD_NODE_COPY) ||
          (info->capabilities==INTEL_UMD_CAP_RCS_COPY32 && info->copy_node==INTEL_UMD_NODE_RENDER));
    adapter_node=info->copy_node;
    if (query_corrupt==1) info->version++;
    if (query_corrupt==2) info->capabilities^=INTEL_UMD_CAP_COPY32|INTEL_UMD_CAP_RCS_COPY32;
    if (query_corrupt==3) info->copy_node^=1;
    if (query_corrupt==4) info->capabilities|=0x80000000u;
    return S_OK;
}
static HRESULT APIENTRY create_context(HANDLE h,D3DDDICB_CREATECONTEXT *args)
{
    GUARD(2);
    CHECK(args->NodeOrdinal==adapter_node && args->EngineAffinity==0 && args->Flags.Value==0);
    CHECK(args->PrivateDriverDataSize==24);
#ifdef UMD_MODEL
    CHECK(((intel_umd_context_description_t *)args->pPrivateDriverData)->magic==INTEL_UMD_CONTEXT_MAGIC);
    args->hContext=(HANDLE)0x8001; args->pCommandBuffer=model_commands; args->CommandBufferSize=sizeof(model_commands);
    args->pAllocationList=model_allocations; args->AllocationListSize=64;
    args->pPatchLocationList=model_patches; args->PatchLocationListSize=64;
#else
    OKMT_CREATE_CONTEXT q={0}; q.device=kernel_device; q.node=args->NodeOrdinal;
    q.private_data=args->pPrivateDriverData; q.private_bytes=args->PrivateDriverDataSize;
    CHECK(kcontext(&q)==0);
    args->hContext=(HANDLE)(UINT_PTR)q.context; args->pCommandBuffer=q.command_buffer; args->CommandBufferSize=q.command_bytes;
    args->pAllocationList=q.allocation_list; args->AllocationListSize=q.allocation_count;
    args->pPatchLocationList=q.patch_list; args->PatchLocationListSize=q.patch_count;
#endif
    return S_OK;
}
static HRESULT APIENTRY destroy_context(HANDLE h,const D3DDDICB_DESTROYCONTEXT *args)
{
    GUARD(3);
#ifdef UMD_MODEL
    CHECK(args->hContext==(HANDLE)0x8001);
#else
    OKMT_DESTROY_CONTEXT q={(OKMT_HANDLE)(UINT_PTR)args->hContext}; CHECK(kdestroy_context(&q)==0);
#endif
    return S_OK;
}
static HRESULT APIENTRY allocate(HANDLE h,D3DDDICB_ALLOCATE *args)
{
    GUARD(4); CHECK(args->NumAllocations==1 && !args->PrivateDriverDataSize);
    unsigned slot=0; while(slot<8 && backing[slot].allocation) slot++; CHECK(slot<8);
    intel_umd_buffer_description_t *info=args->pAllocationInfo->pPrivateDriverData;
    CHECK(args->pAllocationInfo->PrivateDriverDataSize==24 && info->bytes<=sizeof(backing[slot].memory));
    backing[slot].bytes=(unsigned)info->bytes; backing[slot].runtime=args->hResource;
#ifdef UMD_MODEL
    backing[slot].allocation=slot+1; backing[slot].group=args->hResource ? slot+101 : 0;
#else
    OKMT_ALLOCATION_INFO data={0}; data.private_data=info; data.private_bytes=24;
    OKMT_CREATE_ALLOCATION q={0}; q.device=kernel_device; q.count=1; q.allocations=&data; q.flags=args->hResource ? 1 : 0;
    CHECK(kallocate(&q)==0); backing[slot].allocation=data.allocation; backing[slot].group=q.resource;
#endif
    args->pAllocationInfo->hAllocation=backing[slot].allocation; args->hKMResource=backing[slot].group;
    if (!args->hResource) { CHECK(!args->hKMResource); private_allocations++; }
    return S_OK;
}
static HRESULT APIENTRY deallocate(HANDLE h,const D3DDDICB_DEALLOCATE *args)
{
    GUARD(5);
    CHECK(args->hResource ? !args->NumAllocations : (args->NumAllocations==1 && args->HandleList));
    unsigned slot=0;
    while(slot<8 && (!backing[slot].allocation || (args->hResource ?
          backing[slot].runtime!=args->hResource : backing[slot].allocation!=args->HandleList[0]))) slot++;
    CHECK(slot<8 && !backing[slot].locked);
#ifndef UMD_MODEL
    OKMT_DESTROY_ALLOCATION q={kernel_device,backing[slot].group,
        args->hResource ? NULL : &backing[slot].allocation,args->hResource ? 0 : 1}; CHECK(kdeallocate(&q)==0);
#endif
    if (!args->hResource) private_frees++;
    backing[slot].runtime=NULL; backing[slot].allocation=backing[slot].group=0;
    return S_OK;
}
static HRESULT APIENTRY lock_allocation(HANDLE h,D3DDDICB_LOCK *args)
{
    GUARD(6);
    unsigned slot=0; while(slot<8 && backing[slot].allocation!=args->hAllocation) slot++;
    CHECK(slot<8 && backing[slot].allocation && !backing[slot].locked && args->Flags.LockEntire);
#ifdef UMD_MODEL
    args->pData=backing[slot].memory;
    if (!backing[slot].runtime && !private_locks) args->hAllocation=slot+17;
#else
    OKMT_LOCK q={0}; q.device=kernel_device; q.allocation=args->hAllocation; q.flags=args->Flags.Value;
    OKMT_S32 result=klock(&q);
    if (result==(OKMT_S32)0xc01e0102) return D3DERR_WASSTILLDRAWING;
    CHECK(result==0); args->pData=q.data; args->hAllocation=q.allocation;
#endif
    backing[slot].allocation=args->hAllocation; backing[slot].mapping=args->pData;
    backing[slot].locked=1;
    if (!backing[slot].runtime) {
        CHECK(args->Flags.WriteOnly && !args->Flags.ReadOnly && args->Flags.DonotWait); private_locks++;
        /* Dirty every byte so zero-padding checks detect incomplete uploads. */
        unsigned char *data=args->pData;
        for (unsigned i=0;i<backing[slot].bytes;i++) data[i]=0xa5;
        if (null_mapping) { null_mapping=0; args->pData=NULL; }
    }
    return S_OK;
}
static HRESULT APIENTRY unlock_allocation(HANDLE h,const D3DDDICB_UNLOCK *args)
{
    GUARD(7); CHECK(args->NumAllocations==1);
    unsigned slot=0; while(slot<8 && backing[slot].allocation!=args->phAllocations[0]) slot++;
    CHECK(slot<8 && backing[slot].locked);
    if (!backing[slot].runtime) {
        /* The null-mapping fault has no uploaded header. */
        if (*(unsigned *)backing[slot].mapping!=0xa5a5a5a5u) verify_program(slot);
        private_unlocks++;
    }
#ifndef UMD_MODEL
    OKMT_UNLOCK q={kernel_device,1,(const OKMT_HANDLE *)args->phAllocations}; CHECK(kunlock(&q)==0);
#endif
    backing[slot].locked=0; backing[slot].mapping=NULL; return S_OK;
}
static HRESULT APIENTRY render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8); CHECK(args->CommandLength==64 && args->NumAllocations==2 && args->NumPatchLocations==2 && !args->CommandOffset && !args->Flags.Value);
#ifdef UMD_MODEL
    intel_umd_copy32_t *packet=(void *)model_commands;
    CHECK(packet->magic==INTEL_UMD_COMMAND_MAGIC && packet->version==1 && packet->opcode==INTEL_UMD_COPY32);
    CHECK(packet->source==0 && packet->destination==0 && packet->width==13 && packet->height==7);
    CHECK(model_patches[0].PatchOffset==32 && model_patches[1].PatchOffset==40 && model_patches[1].AllocationIndex==1);
    CHECK(model_allocations[0].Value==0 && model_allocations[1].WriteOperation==1);
    unsigned src=model_allocations[0].hAllocation-1,dst=model_allocations[1].hAllocation-1;
    CHECK(src<8 && dst<8 && src!=dst && !backing[src].locked && !backing[dst].locked);
    for (unsigned y=0;y<7;y++) for (unsigned x=0;x<52;x++)
        backing[dst].memory[y*packet->destination_pitch+x]=backing[src].memory[y*packet->source_pitch+x];
    args->pNewCommandBuffer=model_commands; args->NewCommandBufferSize=sizeof(model_commands);
    args->pNewAllocationList=model_allocations; args->NewAllocationListSize=64;
    args->pNewPatchLocationList=model_patches; args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0}; q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength; q.allocation_count=args->NumAllocations; q.patch_count=args->NumPatchLocations;
    OKMT_S32 result=krender(&q);
    if (result==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    CHECK(result==0);
    args->pNewCommandBuffer=q.commands; args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations; args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches; args->NewPatchLocationListSize=q.patch_capacity;
#endif
    submitted++; return S_OK;
}
static void exercise_pixel_shaders(HANDLE adapter_handle,const D3DDDIARG_CREATEDEVICE *primary,
                                   const D3DDDI_ADAPTERFUNCS *adapter_functions)
{
    CHECK(functions.pfnCreatePixelShader && functions.pfnSetPixelShader && functions.pfnDeletePixelShader);
    UINT code[]={0xffff0200,0x05000051,0xa00f0000,0x3f800000,0,0,0x3f800000,
                 0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
    D3DDDIARG_CREATEPIXELSHADER shader={sizeof(code),(HANDLE)0xdead};
    CHECK(functions.pfnCreatePixelShader(primary->hDevice,NULL,code)==D3DERR_INVALIDCALL);
    CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,NULL)==D3DERR_INVALIDCALL);
    shader.CodeSize=0; CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,code)==D3DERR_INVALIDCALL);
    shader.CodeSize=sizeof(code)-1; CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,code)==D3DERR_INVALIDCALL);
    shader.CodeSize=sizeof(code)-4; CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,code)==D3DERR_INVALIDCALL);
    shader.CodeSize=sizeof(code); code[0]=0xfffe0200;
    CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,code)==D3DERR_NOTAVAILABLE);
    CHECK(shader.ShaderHandle==(HANDLE)0xdead); code[0]=0xffff0200;
    HANDLE handles[140];
    for (unsigned i=0;i<140;i++) {
        CHECK(functions.pfnCreatePixelShader(primary->hDevice,&shader,code)==S_OK && shader.ShaderHandle);
        handles[i]=shader.ShaderHandle;
        for (unsigned j=0;j<i;j++) CHECK(handles[j]!=handles[i]);
    }
    CHECK(private_allocations==0); /* Creation does not allocate GPU pages. */
    callback_fault=4; CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==E_ABORT);
    CHECK(private_allocations==0);
    callback_fault=6; CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==E_ABORT);
    CHECK(private_allocations==1 && private_locks==0);
    callback_fault=7; CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==E_ABORT);
    CHECK(private_allocations==1 && private_locks==1 && private_unlocks==0);
    reenter=1;
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==S_OK);
    reenter=0;
    CHECK(private_allocations==1 && private_locks==1 && private_unlocks==1);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==S_OK);
    CHECK(private_allocations==1 && private_locks==1 && private_unlocks==1);
    D3DDDIARG_CREATEDEVICE second=*primary; D3DDDI_DEVICEFUNCS second_functions={0};
    second.hDevice=runtime_device; second.pDeviceFuncs=&second_functions;
    CHECK(adapter_functions->pfnCreateDevice(adapter_handle,&second)==S_OK);
    CHECK(second_functions.pfnSetPixelShader(second.hDevice,handles[0])==D3DERR_INVALIDCALL);
    CHECK(second_functions.pfnDeletePixelShader(second.hDevice,handles[0])==D3DERR_INVALIDCALL);
    CHECK(second_functions.pfnCreatePixelShader(second.hDevice,&shader,code)==S_OK);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,shader.ShaderHandle)==D3DERR_INVALIDCALL);
    callback_fault=7; CHECK(second_functions.pfnSetPixelShader(second.hDevice,shader.ShaderHandle)==E_ABORT);
    callback_fault=7; CHECK(second_functions.pfnDestroyDevice(second.hDevice)==E_ABORT);
    CHECK(second_functions.pfnSetPixelShader(second.hDevice,shader.ShaderHandle)==S_OK);
    callback_fault=5; CHECK(second_functions.pfnDestroyDevice(second.hDevice)==E_ABORT);
    CHECK(second_functions.pfnSetPixelShader(second.hDevice,shader.ShaderHandle)==S_OK);
    callback_fault=3; CHECK(second_functions.pfnDestroyDevice(second.hDevice)==E_ABORT);
    CHECK(private_allocations==2 && private_frees==1);
    CHECK(second_functions.pfnSetPixelShader(second.hDevice,shader.ShaderHandle)==S_OK);
    CHECK(private_allocations==3); /* Cache release before failed context teardown. */
    CHECK(second_functions.pfnDestroyDevice(second.hDevice)==S_OK); /* releases a live selected program */
    CHECK(second_functions.pfnSetPixelShader(second.hDevice,shader.ShaderHandle)==D3DERR_INVALIDCALL);
    callback_fault=5; CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[0])==E_ABORT);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==S_OK);
    CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[0])==S_OK);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[0])==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[0])==D3DERR_INVALIDCALL);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,NULL)==S_OK);
    /* Delete/teardown must preserve a pending unlock on failure, too. */
    callback_fault=7; CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[1])==E_ABORT);
    callback_fault=7; CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[1])==E_ABORT);
    CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[1])==S_OK);
    null_mapping=1;
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[2])==(HRESULT)0x88760827);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[2])==S_OK);
    for (unsigned i=2;i<139;i++) CHECK(functions.pfnDeletePixelShader(primary->hDevice,handles[i])==S_OK);
    CHECK(functions.pfnSetPixelShader(primary->hDevice,handles[139])==S_OK);
    /* Keep one program until the primary device's final DestroyDevice. */
}

void native_umd_entry(void)
{
    SetErrorMode(0x8001);
    char path[512]; DWORD n=GetModuleFileNameA(NULL,path,sizeof(path)); CHECK(n && n<sizeof(path)-20);
    while(n && path[n-1]!='/' && path[n-1]!='\\') n--;
    static const char name[]="osito-intel9.dll";
    for (unsigned i=0;i<sizeof(name);i++) path[n+i]=name[i];
    HMODULE module=LoadLibraryA(path); CHECK(module);
    PFND3DDDI_OPENADAPTER open_adapter=(void *)GetProcAddress(module,"OpenAdapter"); CHECK(open_adapter);
    CHECK(!GetProcAddress(module,"Direct3DCreate9"));
#ifdef UMD_MODEL
    adapter=0x6001;
#else
    HMODULE gdi=LoadLibraryA("gdi32.dll"); CHECK(gdi);
    OKMT_S32 (WINAPI *enumerate)(OKMT_ENUM_ADAPTERS2 *)=(void *)GetProcAddress(gdi,"D3DKMTEnumAdapters2");
    OKMT_S32 (WINAPI *kdevice)(OKMT_CREATE_DEVICE *)=(void *)GetProcAddress(gdi,"D3DKMTCreateDevice");
    OKMT_S32 (WINAPI *kdestroy_device)(OKMT_DESTROY_DEVICE *)=(void *)GetProcAddress(gdi,"D3DKMTDestroyDevice");
    OKMT_S32 (WINAPI *kclose_adapter)(OKMT_CLOSE_ADAPTER *)=(void *)GetProcAddress(gdi,"D3DKMTCloseAdapter");
#define LOAD(var,symbol) var=(void *)GetProcAddress(gdi,symbol);CHECK(var)
    LOAD(kquery,"D3DKMTQueryAdapterInfo"); LOAD(kcontext,"D3DKMTCreateContext"); LOAD(kdestroy_context,"D3DKMTDestroyContext");
    LOAD(kallocate,"D3DKMTCreateAllocation"); LOAD(kdeallocate,"D3DKMTDestroyAllocation");
    LOAD(klock,"D3DKMTLock"); LOAD(kunlock,"D3DKMTUnlock"); LOAD(krender,"D3DKMTRender");
    CHECK(enumerate && kdevice && kdestroy_device && kclose_adapter);
    OKMT_ADAPTER_INFO found[16]; OKMT_ENUM_ADAPTERS2 list={16,found}; CHECK(enumerate(&list)==0 && list.count<=16);
    for (unsigned i=0;i<list.count;i++) {
        intel_umd_adapter_description_t info={0};
        OKMT_QUERY_ADAPTER_INFO q={found[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&info,sizeof(info)};
        if (kquery(&q)==0 && info.magic==INTEL_UMD_ADAPTER_MAGIC) adapter=found[i].adapter;
        else { OKMT_CLOSE_ADAPTER c={found[i].adapter}; CHECK(kclose_adapter(&c)==0); }
    }
    if (!adapter) {report("UMD UNAVAILABLE native adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS adapter_functions={0};
    D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter; a.Interface=9;
    a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&adapter_functions;
    CHECK(open_adapter(NULL)==D3DERR_INVALIDCALL);
    a.Interface=10; CHECK(open_adapter(&a)==D3DERR_INVALIDCALL); a.Interface=9;
    callback_fault=1; CHECK(open_adapter(&a)==E_ABORT && a.hAdapter==(HANDLE)(UINT_PTR)adapter);
    for (query_corrupt=1;query_corrupt<=4;query_corrupt++) CHECK(open_adapter(&a)==D3DERR_NOTAVAILABLE);
    query_corrupt=0;
    CHECK(open_adapter(&a)==0 && a.DriverVersion==D3D_UMD_INTERFACE_VERSION && a.hAdapter!=(HANDLE)(UINT_PTR)adapter);
    D3DDDIARG_GETCAPS caps={0}; CHECK(adapter_functions.pfnGetCaps(a.hAdapter,&caps)==D3DERR_NOTAVAILABLE);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=allocate; dc.pfnDeallocateCb=deallocate; dc.pfnLockCb=lock_allocation;
    dc.pfnUnlockCb=unlock_allocation; dc.pfnRenderCb=render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version; d.pCallbacks=&dc; d.pDeviceFuncs=&functions;
    callback_fault=2; CHECK(adapter_functions.pfnCreateDevice(a.hAdapter,&d)==E_ABORT && d.hDevice==runtime_device);
    CHECK(adapter_functions.pfnCreateDevice(a.hAdapter,&d)==0); active_device=d.hDevice;
    exercise_pixel_shaders(a.hAdapter,&d,&adapter_functions);
    CHECK(adapter_functions.pfnCloseAdapter(a.hAdapter)==D3DERR_INVALIDCALL);
    HANDLE handles[2]; D3DDDI_SURFACEINFO surface={13,7,1,NULL,0,0};
    for (unsigned i=0;i<2;i++) {
        D3DDDIARG_CREATERESOURCE r={0}; r.hResource=(HANDLE)(UINT_PTR)(0x9001+i); r.Format=D3DDDIFMT_A8R8G8B8;
        r.Pool=D3DDDIPOOL_VIDEOMEMORY; r.pSurfList=&surface; r.SurfCount=r.MipLevels=1; r.Rotation=D3DDDI_ROTATION_IDENTITY;
        r.Flags.CubeMap=1; CHECK(functions.pfnCreateResource(d.hDevice,&r)==D3DERR_INVALIDCALL); r.Flags.Value=0;
        callback_fault=4; CHECK(functions.pfnCreateResource(d.hDevice,&r)==E_ABORT && r.hResource==(HANDLE)(UINT_PTR)(0x9001+i));
        CHECK(functions.pfnCreateResource(d.hDevice,&r)==0); handles[i]=r.hResource;
    }
    CHECK(functions.pfnDestroyDevice(d.hDevice)==D3DERR_INVALIDCALL);
    for (unsigned round=0;round<3;round++) {
        for (unsigned role=0;role<2;role++) {
            D3DDDIARG_LOCK l={0}; l.hResource=handles[role]; l.Flags.WriteOnly=l.Flags.DoNotWait=1;
            callback_fault=6; CHECK(functions.pfnLock(d.hDevice,&l)==E_ABORT && !l.pSurfData);
            HRESULT status=D3DERR_WASSTILLDRAWING;
            for (unsigned wait=0;wait<300 && status==D3DERR_WASSTILLDRAWING;wait++) {status=functions.pfnLock(d.hDevice,&l);if(status==D3DERR_WASSTILLDRAWING)Sleep(10);}
            CHECK(status==0 && l.Pitch==64 && l.SlicePitch==448 && l.pSurfData);
            CHECK(functions.pfnLock(d.hDevice,&l)==D3DERR_INVALIDCALL);
            CHECK(functions.pfnDestroyResource(d.hDevice,handles[role])==D3DERR_INVALIDCALL);
            unsigned char *data=l.pSurfData; for (unsigned i=0;i<448;i++) data[i]=role ? 0xa6 : (unsigned char)(i*13+round*7);
            D3DDDIARG_UNLOCK u={handles[role],0,{0}};
            callback_fault=7; CHECK(functions.pfnUnlock(d.hDevice,&u)==E_ABORT);
            reenter=1; CHECK(functions.pfnUnlock(d.hDevice,&u)==0); reenter=0;
        }
        D3DDDIARG_BLT b={0}; b.hSrcResource=handles[0]; b.hDstResource=handles[1]; b.SrcRect.right=b.DstRect.right=13; b.SrcRect.bottom=b.DstRect.bottom=7;
        b.DstRect.left=1; CHECK(functions.pfnBlt(d.hDevice,&b)==D3DERR_INVALIDCALL); b.DstRect.left=0;
        callback_fault=8; CHECK(functions.pfnBlt(d.hDevice,&b)==E_ABORT);
        CHECK(functions.pfnBlt(d.hDevice,&b)==0);
        D3DDDIARG_LOCK l={0}; l.hResource=handles[1]; l.Flags.ReadOnly=l.Flags.DoNotWait=1;
        HRESULT status=D3DERR_WASSTILLDRAWING;
        for (unsigned wait=0;wait<300 && status==D3DERR_WASSTILLDRAWING;wait++) {status=functions.pfnLock(d.hDevice,&l);if(status==D3DERR_WASSTILLDRAWING)Sleep(10);}
        CHECK(status==0 && l.pSurfData);
        unsigned char *data=l.pSurfData;
        for (unsigned y=0;y<7;y++) for (unsigned x=0;x<64;x++) {
            unsigned expected=x<52 ? (unsigned char)((y*64+x)*13+round*7) : 0xa6;
            if (data[y*64+x]!=expected) {
                report("UMD mismatch round=",round); report("UMD mismatch offset=",y*64+x);
                report("UMD mismatch actual=",data[y*64+x]); report("UMD mismatch expected=",expected);
            }
            CHECK(data[y*64+x]==expected);
        }
        D3DDDIARG_UNLOCK u={handles[1],0,{0}}; CHECK(functions.pfnUnlock(d.hDevice,&u)==0);
    }
    for (unsigned role=0;role<2;role++) {
        callback_fault=5; CHECK(functions.pfnDestroyResource(d.hDevice,handles[role])==E_ABORT);
        CHECK(functions.pfnDestroyResource(d.hDevice,handles[role])==0);
        CHECK(functions.pfnDestroyResource(d.hDevice,handles[role])==D3DERR_INVALIDCALL);
    }
    callback_fault=3; CHECK(functions.pfnDestroyDevice(d.hDevice)==E_ABORT);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==0);
    CHECK(private_allocations==private_frees && private_locks==private_unlocks);
    CHECK(verified_programs==6);
    for (unsigned i=0;i<8;i++) CHECK(!backing[i].allocation && !backing[i].locked);
    CHECK(functions.pfnDestroyDevice(d.hDevice)==D3DERR_INVALIDCALL);
    CHECK(adapter_functions.pfnCloseAdapter(a.hAdapter)==0);
    CHECK(adapter_functions.pfnCloseAdapter(a.hAdapter)==D3DERR_INVALIDCALL);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    CHECK(FreeLibrary(module)); CHECK(submitted==3);
    report("UMD node=",adapter_node);
    report("UMD shader uploads=",verified_programs);
    report("UMD shader allocations=",private_allocations); report("UMD shader frees=",private_frees);
    report("UMD checks=",checks); report("UMD copies=",submitted); report("UMD failures=",failures);
    ExitProcess(failures ? 1 : 0);
}
