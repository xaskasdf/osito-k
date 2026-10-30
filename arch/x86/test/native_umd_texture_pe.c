/* Resource/mip ownership through the public D3D9 DDI. Model callbacks inspect
 * packets only: they do not sample textures or rasterize on the CPU. Physical
 * builds verify copies performed by the native GPU through public KMT. */
#define native_umd_entry unused_base_entry
#include "native_umd_pe.c"
#undef native_umd_entry

static struct {
    HANDLE runtime;
    OKMT_HANDLE allocation,group;
    UINT bytes,locked;
    BYTE memory[16384];
} texture_memory[32];
static UINT allocation_calls,free_calls,lock_calls,unlock_calls,live_levels;
static UINT next_allocation=1000,malformed,reject_free,replace_handle;
/* Independent expected layout for the 31x17 five-level linear Gen9 image. */
static const UINT level_offsets[5]={0,2560,2624,3136,3648};
static const UINT level_x[5]={0,0,16,16,16},level_y[5]={0,20,20,24,28};
static const UINT level_width[5]={31,15,7,3,1},level_height[5]={17,8,4,2,1};

static UINT texture_slot(OKMT_HANDLE handle)
{
    for (UINT i=0;i<32;i++) if (texture_memory[i].allocation==handle && handle) return i;
    CHECK(0); return 0;
}
static HRESULT APIENTRY texture_allocate(HANDLE h,D3DDDICB_ALLOCATE *args)
{
    GUARD(4); allocation_calls++;
    CHECK(args->hResource && args->NumAllocations==1 && !args->PrivateDriverDataSize);
    OKMT_ALLOCATION_INFO info[13]={0}; UINT slots[13];
    for (UINT i=0;i<args->NumAllocations;i++) {
        UINT slot=0;
        while (slot<32 && texture_memory[slot].allocation) slot++;
        CHECK(slot<32);
        intel_umd_buffer_description_t *description=args->pAllocationInfo[i].pPrivateDriverData;
        CHECK(args->pAllocationInfo[i].PrivateDriverDataSize==sizeof(*description));
        CHECK(description->magic==INTEL_UMD_BUFFER_MAGIC && description->version==INTEL_UMD_BUFFER_VERSION);
        CHECK(description->bytes && description->bytes<=256u*1024u*1024u);
        memset(texture_memory[slot].memory,0,sizeof(texture_memory[slot].memory));
        texture_memory[slot].bytes=(UINT)description->bytes;
        texture_memory[slot].runtime=args->hResource;
        texture_memory[slot].allocation=++next_allocation; slots[i]=slot;
        info[i].private_data=description; info[i].private_bytes=sizeof(*description);
    }
    OKMT_HANDLE group=++next_allocation;
#ifndef UMD_MODEL
    OKMT_CREATE_ALLOCATION q={0}; q.device=kernel_device; q.count=args->NumAllocations; q.allocations=info; q.flags=1;
    OKMT_S32 status=kallocate(&q);
    if (status) {
        report("TEXTURE allocation status=",(UINT)status);
        report("TEXTURE allocation levels=",args->NumAllocations);
        report("TEXTURE allocation call=",allocation_calls);
        report("TEXTURE prior copies=",submitted);
        report("TEXTURE prior frees=",free_calls);
    }
    CHECK(status==0); group=q.resource; CHECK(group);
#endif
    for (UINT i=0;i<args->NumAllocations;i++) {
        UINT slot=slots[i];
#ifndef UMD_MODEL
        texture_memory[slot].allocation=info[i].allocation;
#endif
        texture_memory[slot].group=group;
        args->pAllocationInfo[i].hAllocation=texture_memory[slot].allocation;
        live_levels++;
    }
    args->hKMResource=group;
    if (malformed) {
        args->pAllocationInfo[0].hAllocation=0;
        malformed=0;
    }
    return S_OK;
}
static HRESULT APIENTRY texture_deallocate(HANDLE h,const D3DDDICB_DEALLOCATE *args)
{
    GUARD(5);
    if (reject_free) {reject_free--;return E_ABORT;}
    CHECK(args->hResource && !args->NumAllocations && !args->HandleList);
    OKMT_HANDLE group=0; UINT count=0;
    for (UINT i=0;i<32;i++) if (texture_memory[i].allocation && texture_memory[i].runtime==args->hResource) {
        CHECK(!texture_memory[i].locked);
        CHECK(!group || group==texture_memory[i].group);
        group=texture_memory[i].group; count++;
    }
    CHECK(count && group);
#ifndef UMD_MODEL
    OKMT_DESTROY_ALLOCATION q={kernel_device,group,NULL,0}; CHECK(kdeallocate(&q)==0);
#endif
    for (UINT i=0;i<32;i++) if (texture_memory[i].runtime==args->hResource) {
        texture_memory[i].allocation=texture_memory[i].group=0;
        texture_memory[i].runtime=NULL;
    }
    live_levels-=count; free_calls++; return S_OK;
}
static HRESULT APIENTRY texture_lock(HANDLE h,D3DDDICB_LOCK *args)
{
    GUARD(6);
    UINT slot=texture_slot(args->hAllocation);
    CHECK(!texture_memory[slot].locked && args->Flags.LockEntire);
    CHECK(!args->Flags.ReadOnly && !args->Flags.WriteOnly && !args->Flags.IgnoreSync && !args->Flags.IgnoreReadSync);
#ifdef UMD_MODEL
    CHECK(texture_memory[slot].bytes<=sizeof(texture_memory[slot].memory));
    args->pData=texture_memory[slot].memory;
    if (replace_handle) { args->hAllocation=++next_allocation; replace_handle=0; }
#else
    OKMT_LOCK q={0}; q.device=kernel_device; q.allocation=args->hAllocation; q.flags=args->Flags.Value;
    OKMT_S32 status=klock(&q);
    if (status==(OKMT_S32)0xc01e0102) return D3DERR_WASSTILLDRAWING;
    if (status) {
        report("TEXTURE lock status=",(UINT)status);
        report("TEXTURE lock flags=",args->Flags.Value);
        report("TEXTURE lock slot=",slot);
        report("TEXTURE prior locks=",lock_calls);
    }
    CHECK(status==0); args->pData=q.data; args->hAllocation=q.allocation;
#endif
    CHECK(args->pData); texture_memory[slot].allocation=args->hAllocation;
    texture_memory[slot].locked=1; lock_calls++;
    if (null_mapping) {null_mapping--;args->pData=NULL;}
    return S_OK;
}
static HRESULT APIENTRY texture_unlock(HANDLE h,const D3DDDICB_UNLOCK *args)
{
    GUARD(7); CHECK(args->NumAllocations==1);
    UINT slot=texture_slot(args->phAllocations[0]); CHECK(texture_memory[slot].locked);
#ifndef UMD_MODEL
    OKMT_UNLOCK q={kernel_device,1,(const OKMT_HANDLE *)args->phAllocations}; CHECK(kunlock(&q)==0);
#endif
    texture_memory[slot].locked=0; unlock_calls++; return S_OK;
}
static HRESULT APIENTRY texture_render(HANDLE h,D3DDDICB_RENDER *args)
{
    GUARD(8); CHECK(args->CommandLength==64 && args->NumAllocations==2 && args->NumPatchLocations==2);
#ifdef UMD_MODEL
    const intel_umd_copy32_t *p=(const void *)model_commands;
    CHECK(p->magic==INTEL_UMD_COMMAND_MAGIC && p->version==(submitted?2u:1u) && p->opcode==INTEL_UMD_COPY32 && p->bytes==64);
    CHECK(!p->source && !p->destination);
    if (submitted<5) {
        CHECK(p->reserved[0]==level_x[submitted] && p->reserved[1]==level_y[submitted]);
        CHECK(p->reserved[2]==level_x[submitted] && p->reserved[3]==level_y[submitted]);
        CHECK(p->width==level_width[submitted] && p->height==level_height[submitted]);
    } else CHECK(p->width==3 && p->height==2 && p->reserved[0]==2 && p->reserved[1]==23 &&
                 p->reserved[2]==17 && p->reserved[3]==21);
    CHECK(model_patches[0].AllocationIndex==0 && model_patches[0].SlotId==0 && model_patches[0].PatchOffset==32);
    CHECK(model_patches[1].AllocationIndex==1 && model_patches[1].SlotId==1 && model_patches[1].PatchOffset==40);
    CHECK(!model_patches[0].AllocationOffset && !model_patches[1].AllocationOffset);
    CHECK(model_allocations[0].Value==0 && model_allocations[1].Value==1);
    UINT src=texture_slot(model_allocations[0].hAllocation),dst=texture_slot(model_allocations[1].hAllocation);
    CHECK(src!=dst && !texture_memory[src].locked && !texture_memory[dst].locked);
    CHECK(p->width && p->height && p->source_pitch>=p->width*4 && p->destination_pitch>=p->width*4);
    CHECK(p->source_pitch==128 && p->destination_pitch==128);
    CHECK(texture_memory[src].bytes==4096 && texture_memory[dst].bytes==4096);
    args->pNewCommandBuffer=model_commands; args->NewCommandBufferSize=sizeof(model_commands);
    args->pNewAllocationList=model_allocations; args->NewAllocationListSize=64;
    args->pNewPatchLocationList=model_patches; args->NewPatchLocationListSize=64;
#else
    OKMT_RENDER q={0}; q.context=(OKMT_HANDLE)(UINT_PTR)args->hContext;
    q.command_length=args->CommandLength; q.allocation_count=args->NumAllocations; q.patch_count=args->NumPatchLocations;
    OKMT_S32 status=krender(&q);
    if (status==(OKMT_S32)0x80000011) return D3DERR_WASSTILLDRAWING;
    CHECK(status==0);
    args->pNewCommandBuffer=q.commands; args->NewCommandBufferSize=q.command_bytes;
    args->pNewAllocationList=q.allocations; args->NewAllocationListSize=q.allocation_capacity;
    args->pNewPatchLocationList=q.patches; args->NewPatchLocationListSize=q.patch_capacity;
#endif
    submitted++; return S_OK;
}
static D3DDDIARG_CREATERESOURCE texture_description(D3DDDI_SURFACEINFO *surfaces,UINT width,UINT height,UINT levels,HANDLE runtime)
{
    for (UINT i=0;i<levels;i++) {
        surfaces[i]=(D3DDDI_SURFACEINFO){width,height,1,NULL,0,0};
        if (width>1) width/=2;
        if (height>1) height/=2;
    }
    D3DDDIARG_CREATERESOURCE r={0}; r.hResource=runtime; r.Format=D3DDDIFMT_A8R8G8B8;
    r.Pool=D3DDDIPOOL_VIDEOMEMORY; r.pSurfList=surfaces; r.SurfCount=r.MipLevels=levels;
    r.Rotation=D3DDDI_ROTATION_IDENTITY; r.Flags.Texture=1; return r;
}
static D3DDDIARG_LOCK texture_map(HANDLE resource,UINT level,BOOL write)
{
    D3DDDIARG_LOCK l={0}; l.hResource=resource; l.SubResourceIndex=level;
    l.Flags.DoNotWait=1; l.Flags.WriteOnly=write; l.Flags.ReadOnly=!write;
    HRESULT status=D3DERR_WASSTILLDRAWING;
    for (UINT i=0;i<300 && status==D3DERR_WASSTILLDRAWING;i++) {
        status=functions.pfnLock(active_device,&l); if (status==D3DERR_WASSTILLDRAWING) Sleep(10);
    }
    CHECK(status==S_OK && l.pSurfData); return l;
}
static void texture_unmap(HANDLE resource,UINT level)
{
    D3DDDIARG_UNLOCK u={resource,level,{0}}; CHECK(functions.pfnUnlock(active_device,&u)==S_OK);
}
static void texture_invalid_create(D3DDDIARG_CREATERESOURCE *r)
{
    HANDLE original=r->hResource; UINT calls=allocation_calls;
    CHECK(functions.pfnCreateResource(active_device,r)==D3DERR_INVALIDCALL);
    CHECK(r->hResource==original && allocation_calls==calls);
}
static void texture_check_all(HANDLE resource,UINT copied,BOOL partial,BOOL destination)
{
    D3DDDIARG_LOCK map=texture_map(resource,0,FALSE);
    for (UINT i=0;i<4096;i++) {
        UINT expected=0;
        for (UINT level=0;level<5;level++) {
            if (i<level_offsets[level]) continue;
            UINT offset=i-level_offsets[level],y=offset/128,x=offset%128;
            if (y>=level_height[level] || x>=level_width[level]*4) continue;
            expected=destination && level>=copied ? 0xa6 : 0x10+level;
            if ((!destination || level<copied) && level==1 && y>=3 && y<6 && x>=8 && x<20) expected=0x73;
            if (destination && partial && level==2 && y>=1 && y<3 && x>=4 && x<16) expected=0x73;
        }
        CHECK(((BYTE *)map.pSurfData)[i]==expected);
    }
    texture_unmap(resource,0);
}
static void texture_cases(void)
{
    D3DDDI_SURFACEINFO surfaces[14];
    D3DDDIARG_CREATERESOURCE r=texture_description(surfaces,31,17,5,(HANDLE)0x9100);
    r.MipLevels=4; texture_invalid_create(&r); r.MipLevels=5;
    r.Flags.CubeMap=1; texture_invalid_create(&r); r.Flags.CubeMap=0;
    r.Flags.Volume=1; texture_invalid_create(&r); r.Flags.Volume=0;
    r.Flags.AutogenMipmap=1; texture_invalid_create(&r); r.Flags.AutogenMipmap=0;
    r.Flags.Texture=0; texture_invalid_create(&r); r.Flags.Texture=1;
    r.SurfCount=14; texture_invalid_create(&r); r.SurfCount=5;
    surfaces[3].Width++; texture_invalid_create(&r); surfaces[3].Width--;
    surfaces[4].pSysMem=(void *)1; texture_invalid_create(&r); surfaces[4].pSysMem=NULL;
    r.Pool=D3DDDIPOOL_SYSTEMMEM; texture_invalid_create(&r); r.Pool=D3DDDIPOOL_VIDEOMEMORY;
    callback_fault=4; CHECK(functions.pfnCreateResource(active_device,&r)==E_ABORT && r.hResource==(HANDLE)0x9100);
    HANDLE handles[2];
    for (UINT role=0;role<2;role++) {
        r=texture_description(surfaces,31,17,5,(HANDLE)(UINT_PTR)(0x9100+role));
        reenter=1; CHECK(functions.pfnCreateResource(active_device,&r)==S_OK); reenter=0;
        handles[role]=r.hResource;
        UINT locks=lock_calls,unlocks=unlock_calls;
        D3DDDIARG_LOCK maps[5];
        replace_handle=1;
        for (UINT i=0;i<5;i++) {
            maps[i]=texture_map(handles[role],i,TRUE);
            CHECK(maps[i].Pitch==128 && lock_calls==locks+1);
            CHECK(maps[i].SlicePitch==maps[i].Pitch*surfaces[i].Height);
            CHECK(maps[i].pSurfData==(BYTE *)maps[0].pSurfData+level_offsets[i]);
            for (UINT j=0;j<i;j++) CHECK(maps[i].pSurfData!=maps[j].pSurfData);
            for (UINT y=0;y<surfaces[i].Height;y++)
                memset((BYTE *)maps[i].pSurfData+y*maps[i].Pitch,role ? 0xa6 : 0x10+i,surfaces[i].Width*4);
            CHECK(functions.pfnLock(active_device,&maps[i])==D3DERR_INVALIDCALL);
        }
        CHECK(functions.pfnDestroyResource(active_device,handles[role])==D3DERR_INVALIDCALL);
        const UINT order[5]={4,1,0,3,2};callback_fault=7;
        for (UINT i=0;i<4;i++) texture_unmap(handles[role],order[i]);
        CHECK(unlock_calls==unlocks && callback_fault==7);
        D3DDDIARG_UNLOCK last={handles[role],2,{0}};
        CHECK(functions.pfnUnlock(active_device,&last)==E_ABORT && unlock_calls==unlocks);
        CHECK(functions.pfnDestroyResource(active_device,handles[role])==D3DERR_INVALIDCALL);
        texture_unmap(handles[role],2);CHECK(unlock_calls==unlocks+1);
    }
    CHECK(live_levels==2);
    /* A read-first mapping admits a concurrent write to a different mip.
     * The first callback acquired read/write ownership of the whole image. */
    UINT before_locks=lock_calls,before_unlocks=unlock_calls;
    D3DDDIARG_LOCK read=texture_map(handles[0],0,FALSE),write=texture_map(handles[0],1,TRUE);
    CHECK(write.pSurfData==(BYTE *)read.pSurfData+level_offsets[1] && lock_calls==before_locks+1);
    texture_unmap(handles[0],0);CHECK(unlock_calls==before_unlocks);
    texture_unmap(handles[0],1);CHECK(unlock_calls==before_unlocks+1);
    /* Subrect output is based on the chosen mip; pitch stays the full row. */
    D3DDDIARG_LOCK whole=texture_map(handles[0],1,TRUE); BYTE *base=whole.pSurfData;
    texture_unmap(handles[0],1);
    D3DDDIARG_LOCK area={0}; area.hResource=handles[0]; area.SubResourceIndex=1;
    area.Flags.AreaValid=area.Flags.WriteOnly=1; area.Area=(RECT){2,3,5,6};
    area.pSurfData=(void *)0x1234; area.Pitch=17; area.SlicePitch=19;
    const RECT invalid[]={{-1,0,1,1},{0,-1,1,1},{2,2,2,3},{2,3,3,2},{0,0,16,8},{0,0,15,9},{0,0,0x7fffffff,8}};
    for (UINT i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        area.Area=invalid[i]; UINT before=lock_calls;
        CHECK(functions.pfnLock(active_device,&area)==D3DERR_INVALIDCALL && lock_calls==before);
        CHECK(area.pSurfData==(void *)0x1234 && area.Pitch==17 && area.SlicePitch==19);
    }
    area.Area=(RECT){2,3,5,6};
    area.SubResourceIndex=5; CHECK(functions.pfnLock(active_device,&area)==D3DERR_INVALIDCALL); area.SubResourceIndex=1;
    area.Flags.RangeValid=1; CHECK(functions.pfnLock(active_device,&area)==D3DERR_INVALIDCALL); area.Flags.RangeValid=0;
    area.Flags.ReadOnly=1; CHECK(functions.pfnLock(active_device,&area)==D3DERR_INVALIDCALL); area.Flags.ReadOnly=0;
    callback_fault=6; CHECK(functions.pfnLock(active_device,&area)==E_ABORT && area.pSurfData==(void *)0x1234);
    CHECK(functions.pfnLock(active_device,&area)==S_OK && area.Pitch==128 && area.SlicePitch==1024);
#ifdef UMD_MODEL
    CHECK(area.pSurfData==base+3*128+2*4);
#endif
    for (UINT y=0;y<3;y++) memset((BYTE *)area.pSurfData+y*area.Pitch,0x73,12);
    D3DDDIARG_UNLOCK u={handles[0],1,{0}};
    callback_fault=7; CHECK(functions.pfnUnlock(active_device,&u)==E_ABORT);
    CHECK(functions.pfnDestroyResource(active_device,handles[0])==D3DERR_INVALIDCALL);
    texture_unmap(handles[0],1);
    texture_check_all(handles[0],5,FALSE,FALSE);
    texture_check_all(handles[1],0,FALSE,TRUE);
    for (UINT i=0;i<5;i++) {
        D3DDDIARG_BLT b={0}; b.hSrcResource=handles[0]; b.hDstResource=handles[1];
        b.SrcSubResourceIndex=b.DstSubResourceIndex=i;
        b.SrcRect.right=b.DstRect.right=surfaces[i].Width;
        b.SrcRect.bottom=b.DstRect.bottom=surfaces[i].Height;
        b.SrcSubResourceIndex=5; CHECK(functions.pfnBlt(active_device,&b)==D3DERR_INVALIDCALL); b.SrcSubResourceIndex=i;
        whole=texture_map(handles[0],i,FALSE); CHECK(functions.pfnBlt(active_device,&b)==D3DERR_INVALIDCALL);
        texture_unmap(handles[0],i);
        /* A different locked level still owns the shared GPU allocation. */
        UINT other=(i+1)%5; whole=texture_map(handles[0],other,FALSE);
        UINT calls=submitted;
        CHECK(functions.pfnBlt(active_device,&b)==D3DERR_INVALIDCALL && submitted==calls);
        texture_unmap(handles[0],other);
        callback_fault=8; CHECK(functions.pfnBlt(active_device,&b)==E_ABORT);
        CHECK(functions.pfnBlt(active_device,&b)==S_OK);
#ifndef UMD_MODEL
        texture_check_all(handles[1],i+1,FALSE,TRUE);
        texture_check_all(handles[0],5,FALSE,FALSE);
#endif
    }
    D3DDDIARG_BLT part={0};part.hSrcResource=handles[0];part.hDstResource=handles[1];
    part.SrcSubResourceIndex=1;part.DstSubResourceIndex=2;
    part.SrcRect=(RECT){2,3,5,5};part.DstRect=(RECT){1,1,4,3};
    for (UINT bad=0;bad<7;bad++) {
        D3DDDIARG_BLT invalid=part;
        if (bad==0) invalid.SrcRect.left=-1;
        if (bad==1) invalid.DstRect.top=-1;
        if (bad==2) invalid.SrcRect.right=16;
        if (bad==3) invalid.DstRect.bottom=5;
        if (bad==4) invalid.DstRect.right++;
        if (bad==5) invalid.SrcRect.bottom=3;
        if (bad==6) invalid.hDstResource=handles[0];
        UINT calls=submitted;CHECK(functions.pfnBlt(active_device,&invalid)==D3DERR_INVALIDCALL && submitted==calls);
    }
    CHECK(functions.pfnBlt(active_device,&part)==S_OK);
#ifndef UMD_MODEL
    texture_check_all(handles[1],5,TRUE,TRUE);texture_check_all(handles[0],5,FALSE,FALSE);
#endif
    /* Null mappings retain the single allocation lease across failed recovery. */
    D3DDDIARG_LOCK l={0}; l.hResource=handles[0]; l.SubResourceIndex=2;
    null_mapping=1; callback_fault=7;
    CHECK(functions.pfnLock(active_device,&l)==(HRESULT)0x88760827 && !l.pSurfData);
    l.SubResourceIndex=3;callback_fault=7;
    CHECK(functions.pfnLock(active_device,&l)==E_ABORT && !l.pSurfData);
    CHECK(functions.pfnBlt(active_device,&part)==D3DERR_INVALIDCALL);
    callback_fault=7; CHECK(functions.pfnDestroyResource(active_device,handles[0])==E_ABORT);
    callback_fault=5; CHECK(functions.pfnDestroyResource(active_device,handles[0])==E_ABORT);
    CHECK(functions.pfnDestroyResource(active_device,handles[0])==S_OK);
    CHECK(functions.pfnDestroyResource(active_device,handles[0])==D3DERR_INVALIDCALL);
    CHECK(functions.pfnDestroyResource(active_device,handles[1])==S_OK && !live_levels);
#ifdef UMD_MODEL
    /* Full dimension boundary is a layout test. The physical diagnostic
     * provider currently has a 32-page process quota, not a production budget. */
    r=texture_description(surfaces,4096,1,13,(HANDLE)0x9200);
    CHECK(functions.pfnCreateResource(active_device,&r)==S_OK && live_levels==1);
    CHECK(functions.pfnDestroyResource(active_device,r.hResource)==S_OK && !live_levels);
#endif
    r=texture_description(surfaces,1,1,2,(HANDLE)0x9200); texture_invalid_create(&r);
    r=texture_description(surfaces,4097,1,1,(HANDLE)0x9200); texture_invalid_create(&r);
    /* Rollback errors never publish a half-initialized resource or lose it. */
    r=texture_description(surfaces,4,4,3,(HANDLE)0x9300);
    malformed=1; reject_free=2;
    CHECK(functions.pfnCreateResource(active_device,&r)==(HRESULT)0x88760827 && r.hResource==(HANDLE)0x9300);
    CHECK(live_levels==1);
    CHECK(functions.pfnCreateResource(active_device,&r)==E_ABORT && live_levels==1);
    CHECK(functions.pfnCreateResource(active_device,&r)==S_OK && live_levels==1);
    CHECK(functions.pfnDestroyResource(active_device,r.hResource)==S_OK && !live_levels);
    r=texture_description(surfaces,4,4,3,(HANDLE)0x9300); malformed=1; reject_free=2;
    CHECK(functions.pfnCreateResource(active_device,&r)==(HRESULT)0x88760827 && live_levels==1);
    CHECK(functions.pfnDestroyDevice(active_device)==E_ABORT && live_levels==1);
    CHECK(functions.pfnDestroyDevice(active_device)==S_OK && !live_levels);
    CHECK(lock_calls==unlock_calls && allocation_calls==free_calls);
}

void native_umd_texture_entry(void)
{
    SetErrorMode(0x8001);
    char path[512]; DWORD n=GetModuleFileNameA(NULL,path,sizeof(path)); CHECK(n && n<sizeof(path)-20);
    while(n && path[n-1]!='/' && path[n-1]!='\\') n--;
    memcpy(path+n,"osito-intel9.dll",sizeof("osito-intel9.dll"));
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
    for (UINT i=0;i<list.count;i++) {
        intel_umd_adapter_description_t info={0};
        OKMT_QUERY_ADAPTER_INFO q={found[i].adapter,OKMT_QUERY_PRIVATE_DRIVER_DATA,&info,sizeof(info)};
        if (kquery(&q)==0 && info.magic==INTEL_UMD_ADAPTER_MAGIC) adapter=found[i].adapter;
        else {OKMT_CLOSE_ADAPTER c={found[i].adapter};CHECK(kclose_adapter(&c)==0);}
    }
    if (!adapter) {report("TEXTURE UNAVAILABLE native adapter=",0);ExitProcess(2);}
    OKMT_CREATE_DEVICE kd={0}; kd.object.adapter=adapter; CHECK(kdevice(&kd)==0); kernel_device=kd.device;
#endif
    D3DDDI_ADAPTERCALLBACKS callbacks={0}; callbacks.pfnQueryAdapterInfoCb=query_adapter;
    D3DDDI_ADAPTERFUNCS af={0}; D3DDDIARG_OPENADAPTER a={0}; a.hAdapter=(HANDLE)(UINT_PTR)adapter;
    a.Interface=9; a.Version=0x1234; a.pAdapterCallbacks=&callbacks; a.pAdapterFuncs=&af; CHECK(open_adapter(&a)==S_OK);
    D3DDDI_DEVICECALLBACKS dc={0}; dc.pfnCreateContextCb=create_context; dc.pfnDestroyContextCb=destroy_context;
    dc.pfnAllocateCb=texture_allocate; dc.pfnDeallocateCb=texture_deallocate;
    dc.pfnLockCb=texture_lock; dc.pfnUnlockCb=texture_unlock; dc.pfnRenderCb=texture_render;
    D3DDDIARG_CREATEDEVICE d={0}; d.hDevice=runtime_device; d.Interface=9; d.Version=a.Version;
    d.pCallbacks=&dc; d.pDeviceFuncs=&functions; CHECK(af.pfnCreateDevice(a.hAdapter,&d)==S_OK); active_device=d.hDevice;
    texture_cases(); CHECK(submitted==6); CHECK(af.pfnCloseAdapter(a.hAdapter)==S_OK);
#ifndef UMD_MODEL
    OKMT_DESTROY_DEVICE dd={kernel_device}; CHECK(kdestroy_device(&dd)==0);
    OKMT_CLOSE_ADAPTER ca={adapter}; CHECK(kclose_adapter(&ca)==0);
#endif
    CHECK(FreeLibrary(module));
    report("TEXTURE checks=",checks); report("TEXTURE copies=",submitted);
    report("TEXTURE allocations=",allocation_calls); report("TEXTURE frees=",free_calls);
    report("TEXTURE locks=",lock_calls); report("TEXTURE unlocks=",unlock_calls);
    report("TEXTURE failures=",failures); ExitProcess(failures ? 1 : 0);
}
