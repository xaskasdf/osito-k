/* Native Intel D3D9 DDI, compiled against the user's WDK headers. This is a
 * driver DLL, not a replacement d3d9.dll. The current copy subset is exercised
 * explicitly; GetCaps does not advertise a working 3D device yet. */
#define WIN32_LEAN_AND_MEAN
#define D3D_UMD_INTERFACE_VERSION 0x000c
#include <windows.h>
#include <d3d9.h>
#include <d3dumddi.h>
#include <stdlib.h>
#include <string.h>
#include "compiler_api.h"
#include "../../drivers/intel_gpu_umd_abi.h"
#include "../../drivers/intel_gen9_texture.h"
#include "../../drivers/intel_gen9_vertex.h"

/* MSVC-ABI float marker for this freestanding object in the MinGW DLL link. */
int _fltused;

#define INVALID_CALL ((HRESULT)0x8876086c)
#define NOT_AVAILABLE ((HRESULT)0x8876086a)
#define STILL_DRAWING ((HRESULT)0x8876021c)
#define DRIVER_ERROR ((HRESULT)0x88760827)
#define ADAPTERS 16
#define DEVICES 16
#define RESOURCES 128
#define MIP_LEVELS 13 /* 4096 through 1, inclusive. */

typedef struct {
    UINT id,version,devices;
    HANDLE runtime;
    intel_umd_adapter_description_t info;
} adapter_t;
typedef struct pixel_shader {
    struct pixel_shader *next;
    /* Arithmetic PS keeps its qualified legacy program. A programmable VS
     * lazily selects a resource-ABI variant, owned by the same PS handle. */
    struct pixel_shader *vertex_variant;
    UINT id;
    const struct intel_d3d9_program *program;
    D3DKMT_HANDLE allocation;
    UINT allocation_bytes,content_bytes,locked,uploaded;
} pixel_shader_t;
typedef struct vertex_decl {
    struct vertex_decl *next;
    UINT id,offset,texcoord_mask,texcoord_offset[8],texcoord_type[8],typed;
    UINT count,pretransformed,semantics[16],attributes[16];
} vertex_decl_t;
typedef struct {
    UINT texture,srgb,max_mip;
    intel_umd_sampler_t state;
} sampler_t;
typedef struct {
    UINT id,adapter,busy,resources;
    HANDLE runtime,context;
    D3DDDI_DEVICECALLBACKS cb;
    void *commands;
    UINT command_bytes,allocation_count,patch_count;
    D3DDDI_ALLOCATIONLIST *allocations;
    D3DDDI_PATCHLOCATIONLIST *patches;
    UINT pci_id;
    struct intel_d3d9_compiler *compiler,*resource_compiler,*vertex_compiler;
    pixel_shader_t *shaders,*pixel_shader;
    pixel_shader_t *vertex_shaders,*vertex_shader;
    vertex_decl_t *declarations,*vertex_decl;
    UINT node,vertex_buffer,target,stream_offset,stream_stride,index_buffer,index_stride;
    UINT viewport_valid,scissor_enabled,depth_enabled,cull;
    UINT blend_enabled,blend_separate,blend_src,blend_dst,blend_op;
    UINT blend_src_alpha,blend_dst_alpha,blend_op_alpha,blend_factor,color_write;
    D3DDDIARG_VIEWPORTINFO viewport;
    RECT scissor;
    UINT pixel_constants[224][4];
    UINT vertex_constants[256][4];
    sampler_t samplers[INTEL_UMD_MAX_TEXTURES];
    /* Per-device callback serialization also owns this reusable publication
     * workspace. Avoid a >4 KiB stack frame or allocations on every Draw. */
    struct {
        union {
            intel_umd_draw_sampled_t sampled;
            intel_umd_draw_vertex_shader_t vertex;
            intel_umd_draw_indexed_t indexed;
        } packet;
        D3DDDI_ALLOCATIONLIST allocations[INTEL_UMD_MAX_RESOURCES];
        D3DDDI_PATCHLOCATIONLIST patches[INTEL_UMD_MAX_RESOURCES];
    } sampled;
    pixel_shader_t clear_shader;
    D3DKMT_HANDLE clear_vertices;
    UINT clear_vertices_locked,clear_vertices_uploaded;
} device_t;
typedef struct {
    UINT width,height,pitch,locked,bytes,offset;
} surface_t;
typedef struct {
    UINT id,device,levels,vertex_buffer,index_buffer,render_target,texture,orphan;
    D3DDDIFORMAT format;
    HANDLE runtime;
    D3DKMT_HANDLE allocation;
    UINT allocation_bytes,lock_count,cpu_held;
    void *mapping;
    intel_gen9_image_t image;
    surface_t surfaces[MIP_LEVELS];
} resource_t;
typedef struct {
    resource_t *resource;
    UINT offset,count,stride,min_index,num_vertices;
    INT base_vertex;
} index_draw_t;
static adapter_t adapters[ADAPTERS];
static device_t devices[DEVICES];
static resource_t resources[RESOURCES];
static unsigned exclusion,next_id=1;

static void metadata_lock(void)
{
    while (__atomic_exchange_n(&exclusion,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static void metadata_unlock(void) { __atomic_store_n(&exclusion,0,__ATOMIC_RELEASE); }
/* All identities share one nonwrapping process-local namespace. Never expose
 * pointers to recyclable static slots as runtime handles. */
static UINT new_id(void) { return next_id==0xffffffffu ? 0 : next_id++; }
static adapter_t *get_adapter(HANDLE h)
{
    for (UINT i=0;i<ADAPTERS;i++) if (adapters[i].id && (UINT_PTR)h==adapters[i].id) return &adapters[i];
    return NULL;
}
static device_t *get_device(HANDLE h)
{
    for (UINT i=0;i<DEVICES;i++) if (devices[i].id && (UINT_PTR)h==devices[i].id) return &devices[i];
    return NULL;
}
static resource_t *get_resource(device_t *d,HANDLE h)
{
    resource_t *r=NULL;
    metadata_lock();
    for (UINT i=0;i<RESOURCES;i++)
        if (resources[i].id && (UINT_PTR)h==resources[i].id && resources[i].device==d->id &&
            !resources[i].orphan) { r=&resources[i]; break; }
    metadata_unlock(); return r;
}
static HRESULT device_enter(HANDLE h,device_t **out)
{
    metadata_lock(); device_t *d=get_device(h);
    HRESULT result=!d ? INVALID_CALL : d->busy ? STILL_DRAWING : S_OK;
    if (result==S_OK) { d->busy=1; *out=d; }
    metadata_unlock(); return result;
}
static HRESULT device_leave(device_t *d,HRESULT result)
{
    metadata_lock(); d->busy=0; metadata_unlock(); return result;
}
#define ENTER() device_t *d; HRESULT result=device_enter(h,&d); if (FAILED(result)) return result

/* Device-owned storage, allocated lazily on first use. Keep callback ownership
 * through failures: a failed unlock/deallocation must remain retryable. */
static HRESULT shader_release(device_t *d,pixel_shader_t *shader)
{
    if (shader->locked) {
        D3DDDICB_UNLOCK unlock={1,&shader->allocation};
        HRESULT result=d->cb.pfnUnlockCb(d->runtime,&unlock);
        if (FAILED(result)) return result;
        shader->locked=0;
    }
    if (shader->allocation) {
        D3DDDICB_DEALLOCATE request={NULL,1,&shader->allocation};
        HRESULT result=d->cb.pfnDeallocateCb(d->runtime,&request);
        if (FAILED(result)) return result;
        shader->allocation=shader->allocation_bytes=shader->uploaded=0;
    }
    return S_OK;
}

static HRESULT shader_upload(device_t *d,pixel_shader_t *shader)
{
    const struct intel_d3d9_program *program=shader->program;
    if (program->bytecode_bytes>65536u || program->code_bytes>65536u ||
        program->parameter_count>INTEL_UMD_MAX_PARAMETERS)
        return NOT_AVAILABLE;
    /* The immutable compiler result is repacked with a page-aligned code base
     * for Gen9 instruction addressing. Relative kernel offsets stay intact. */
    UINT bytecode_offset=program->header_bytes;
    if (bytecode_offset!=(program->abi==INTEL_D3D9_COMPILER_VERTEX_ABI ?
        sizeof(struct intel_d3d9_vertex_program) : sizeof(*program))) return DRIVER_ERROR;
    UINT map_bytes=program->parameter_count*4;
    UINT map_offset=bytecode_offset+program->bytecode_bytes;
    UINT code_offset=(map_offset+map_bytes+4095u)&~4095u;
    UINT content_bytes=code_offset+program->code_bytes;
    if (!shader->allocation) {
        UINT bytes=(content_bytes+4095u)&~4095u;
        intel_umd_buffer_description_t description={INTEL_UMD_BUFFER_MAGIC,
            INTEL_UMD_BUFFER_VERSION,bytes,0,0};
        D3DDDI_ALLOCATIONINFO allocation={0};
        allocation.pPrivateDriverData=&description; allocation.PrivateDriverDataSize=sizeof(description);
        D3DDDICB_ALLOCATE request={0}; request.NumAllocations=1; request.pAllocationInfo=&allocation;
        HRESULT result=d->cb.pfnAllocateCb(d->runtime,&request);
        if (FAILED(result)) return result;
        if (!allocation.hAllocation) return DRIVER_ERROR;
        shader->allocation=allocation.hAllocation; shader->allocation_bytes=bytes;
    }
    /* A previous upload may have reached Unlock but not completed it. */
    if (shader->locked) {
        D3DDDICB_UNLOCK unlock={1,&shader->allocation};
        HRESULT result=d->cb.pfnUnlockCb(d->runtime,&unlock);
        if (FAILED(result)) return result;
        shader->locked=0;
    }
    if (shader->uploaded) return S_OK;
    D3DDDICB_LOCK lock={0}; lock.hAllocation=shader->allocation;
    lock.Flags.LockEntire=lock.Flags.WriteOnly=lock.Flags.DonotWait=1;
    HRESULT result=d->cb.pfnLockCb(d->runtime,&lock);
    if (FAILED(result)) return result;
    shader->allocation=lock.hAllocation; shader->locked=1;
    if (!lock.pData) return DRIVER_ERROR;
    memset(lock.pData,0,shader->allocation_bytes);
    struct intel_d3d9_program packed=*program;
    packed.bytecode_offset=bytecode_offset; packed.code_offset=code_offset;
    packed.parameter_map_offset=map_bytes ? map_offset : 0;
    packed.allocation_bytes=content_bytes;
    memcpy(lock.pData,program,bytecode_offset);
    memcpy(lock.pData,&packed,sizeof(packed));
    memcpy((unsigned char *)lock.pData+bytecode_offset,
           (const unsigned char *)program+program->bytecode_offset,program->bytecode_bytes);
    if (map_bytes) memcpy((unsigned char *)lock.pData+map_offset,
           (const unsigned char *)program+program->parameter_map_offset,map_bytes);
    memcpy((unsigned char *)lock.pData+code_offset,
           (const unsigned char *)program+program->code_offset,program->code_bytes);
    shader->content_bytes=content_bytes; shader->uploaded=1;
    D3DDDICB_UNLOCK unlock={1,&shader->allocation};
    result=d->cb.pfnUnlockCb(d->runtime,&unlock);
    if (SUCCEEDED(result)) shader->locked=0;
    return result;
}

static HRESULT shader_release_variants(device_t *d,pixel_shader_t *shader)
{
    if (shader->vertex_variant) {
        HRESULT result=shader_release(d,shader->vertex_variant);
        if (FAILED(result)) return result;
    }
    return shader_release(d,shader);
}
static void shader_free(pixel_shader_t *shader)
{
    if (shader->vertex_variant) {
        intel_d3d9_program_free(shader->vertex_variant->program);
        free(shader->vertex_variant);
    }
    intel_d3d9_program_free(shader->program); free(shader);
}
static HRESULT compile_result(UINT status)
{
    return status==INTEL_D3D9_COMPILE_OK ? S_OK :
        status==INTEL_D3D9_COMPILE_NOMEM ? E_OUTOFMEMORY :
        status==INTEL_D3D9_COMPILE_UNSUPPORTED ? NOT_AVAILABLE :
        status==INTEL_D3D9_COMPILE_INVALID ? INVALID_CALL : DRIVER_ERROR;
}
static HRESULT shader_for_vertex(device_t *d,pixel_shader_t **selected)
{
    pixel_shader_t *shader=*selected;
    if (shader->program->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI) return S_OK;
    if (!shader->vertex_variant) {
        UINT status=0;
        if (!d->resource_compiler) status=intel_d3d9_compiler_create(
            INTEL_D3D9_COMPILER_RESOURCE_ABI,d->pci_id,&d->resource_compiler);
        const struct intel_d3d9_program *program=NULL;
        if (!status) status=intel_d3d9_compile_pixel(d->resource_compiler,
            (const BYTE *)shader->program+shader->program->bytecode_offset,
            shader->program->bytecode_bytes,&program,NULL);
        if (status) return compile_result(status);
        pixel_shader_t *variant=calloc(1,sizeof(*variant));
        if (!variant) { intel_d3d9_program_free(program); return E_OUTOFMEMORY; }
        variant->program=program; shader->vertex_variant=variant;
    }
    *selected=shader->vertex_variant; return S_OK;
}

static HRESULT clear_vertices_release(device_t *d)
{
    if (d->clear_vertices_locked) {
        D3DDDICB_UNLOCK unlock={1,&d->clear_vertices};
        HRESULT result=d->cb.pfnUnlockCb(d->runtime,&unlock);
        if (FAILED(result)) return result;
        d->clear_vertices_locked=0;
    }
    if (d->clear_vertices) {
        D3DDDICB_DEALLOCATE request={NULL,1,&d->clear_vertices};
        HRESULT result=d->cb.pfnDeallocateCb(d->runtime,&request);
        if (FAILED(result)) return result;
        d->clear_vertices=0; d->clear_vertices_uploaded=0;
    }
    return S_OK;
}

static HRESULT APIENTRY umd_get_caps(HANDLE h,const D3DDDIARG_GETCAPS *args)
{
    metadata_lock(); BOOL valid=get_adapter(h)!=NULL; metadata_unlock();
    return !valid || !args ? INVALID_CALL : NOT_AVAILABLE;
}
static HRESULT APIENTRY umd_close_adapter(HANDLE h)
{
    metadata_lock(); adapter_t *a=get_adapter(h);
    HRESULT result=!a || a->devices ? INVALID_CALL : S_OK;
    if (result==S_OK) memset(a,0,sizeof(*a));
    metadata_unlock(); return result;
}
/* A successful allocation callback must return a nonzero allocation handle.
 * If it violates that contract, keep ownership until group rollback succeeds;
 * no incomplete resource handle is ever published to the runtime. */
static HRESULT resource_collect_orphans(device_t *d)
{
    for (UINT i=0;i<RESOURCES;i++) {
        metadata_lock();
        resource_t *r=&resources[i];
        BOOL owned=r->id && r->device==d->id && r->orphan;
        metadata_unlock();
        if (!owned) continue;
        D3DDDICB_DEALLOCATE request={r->runtime,0,NULL};
        HRESULT result=d->cb.pfnDeallocateCb(d->runtime,&request);
        if (FAILED(result)) return result;
        metadata_lock(); memset(r,0,sizeof(*r)); d->resources--; metadata_unlock();
    }
    return S_OK;
}
static HRESULT APIENTRY umd_destroy_device(HANDLE h)
{
    ENTER();
    result=resource_collect_orphans(d);
    if (FAILED(result)) return device_leave(d,result);
    if (d->resources) return device_leave(d,INVALID_CALL);
    for (pixel_shader_t *shader=d->shaders;shader;shader=shader->next) {
        result=shader_release_variants(d,shader);
        if (FAILED(result)) return device_leave(d,result);
    }
    for (pixel_shader_t *shader=d->vertex_shaders;shader;shader=shader->next) {
        result=shader_release(d,shader);
        if (FAILED(result)) return device_leave(d,result);
    }
    result=clear_vertices_release(d);
    if (FAILED(result)) return device_leave(d,result);
    result=shader_release(d,&d->clear_shader);
    if (FAILED(result)) return device_leave(d,result);
    D3DDDICB_DESTROYCONTEXT context={d->context};
    result=d->cb.pfnDestroyContextCb(d->runtime,&context);
    if (FAILED(result)) return device_leave(d,result);
    while (d->shaders) {
        pixel_shader_t *shader=d->shaders; d->shaders=shader->next;
        shader_free(shader);
    }
    while (d->vertex_shaders) {
        pixel_shader_t *shader=d->vertex_shaders; d->vertex_shaders=shader->next;
        shader_free(shader);
    }
    while (d->declarations) {
        vertex_decl_t *decl=d->declarations; d->declarations=decl->next; free(decl);
    }
    intel_d3d9_program_free(d->clear_shader.program);
    intel_d3d9_compiler_destroy(d->compiler);
    intel_d3d9_compiler_destroy(d->resource_compiler);
    intel_d3d9_compiler_destroy(d->vertex_compiler);
    metadata_lock();
    adapter_t *a=get_adapter((HANDLE)(UINT_PTR)d->adapter);
    if (a) a->devices--;
    memset(d,0,sizeof(*d)); metadata_unlock(); return S_OK;
}
static HRESULT APIENTRY umd_create_resource(HANDLE h,D3DDDIARG_CREATERESOURCE *args)
{
    ENTER();
    BOOL vertex=args && args->Flags.VertexBuffer;
    BOOL index=args && args->Flags.IndexBuffer,buffer=vertex || index;
    BOOL texture=args && args->Flags.Texture;
    UINT flags=vertex ? 0x80048u : index ? 0x100048u : 0x10009u; /* Vertex/Static/WriteOnly or Texture/Static/RenderTarget. */
    if (!args || !args->hResource || !args->pSurfList || !args->SurfCount || args->SurfCount>MIP_LEVELS ||
        (texture ? args->MipLevels!=args->SurfCount : args->SurfCount!=1 || args->MipLevels>1) ||
        (vertex ? args->Format!=D3DDDIFMT_VERTEXDATA : index ?
         args->Format!=D3DDDIFMT_INDEX16 && args->Format!=D3DDDIFMT_INDEX32 :
         args->Format!=D3DDDIFMT_A8R8G8B8 && args->Format!=D3DDDIFMT_X8R8G8B8) ||
        args->Pool!=D3DDDIPOOL_VIDEOMEMORY || (args->Flags.Value&~flags) || args->MultisampleType ||
        args->MultisampleQuality || args->Fvf || args->VidPnSourceId || args->RefreshRate.Numerator ||
        args->RefreshRate.Denominator || args->Rotation!=D3DDDI_ROTATION_IDENTITY)
        return device_leave(d,INVALID_CALL);
    /* Capture every level before allocation. One canonical Gen9 image owns
     * the complete mip chain; CPU views and GPU submissions share its handle. */
    surface_t surfaces[MIP_LEVELS]={0};
    for (UINT i=0;i<args->SurfCount;i++) {
        D3DDDI_SURFACEINFO surface=args->pSurfList[i];
        if (!surface.Width || surface.Width>(buffer ? 64u*1024u*1024u : 4096u) ||
            (buffer ? surface.Height!=1 : !surface.Height || surface.Height>4096) ||
            surface.Depth>1 || surface.pSysMem || surface.SysMemPitch || surface.SysMemSlicePitch)
            return device_leave(d,INVALID_CALL);
        if (i && ((surfaces[i-1].width==1 && surfaces[i-1].height==1) ||
            surface.Width!=(surfaces[i-1].width>1 ? surfaces[i-1].width/2 : 1) ||
            surface.Height!=(surfaces[i-1].height>1 ? surfaces[i-1].height/2 : 1)))
            return device_leave(d,INVALID_CALL);
        surface_t *s=&surfaces[i];
        s->width=surface.Width; s->height=surface.Height;
        s->pitch=buffer ? s->width : (s->width*4+63)&~63u; s->bytes=s->pitch*s->height;
    }
    intel_gen9_image_t image={0};
    if (!buffer) {
        if (!intel_gen9_image_layout(surfaces[0].width,surfaces[0].height,args->SurfCount,
            args->Format,0,&image)) return device_leave(d,INVALID_CALL);
        for (UINT i=0;i<args->SurfCount;i++) {
            surfaces[i].pitch=image.pitch; surfaces[i].bytes=image.pitch*surfaces[i].height;
            surfaces[i].offset=image.mip[i].offset;
        }
    }
    result=resource_collect_orphans(d);
    if (FAILED(result)) return device_leave(d,result);
    resource_t *r=NULL;
    metadata_lock();
    for (UINT i=0;i<RESOURCES;i++) if (!resources[i].id) {
        UINT id=new_id(); if (id) { r=&resources[i]; r->id=id; r->device=d->id; } break;
    }
    metadata_unlock();
    if (!r) return device_leave(d,E_OUTOFMEMORY);
    r->levels=args->SurfCount; memcpy(r->surfaces,surfaces,sizeof(surfaces));
    r->vertex_buffer=vertex; r->index_buffer=index; r->render_target=args->Flags.RenderTarget;
    r->texture=texture;
    r->format=args->Format; r->runtime=args->hResource;
    r->image=image; r->allocation_bytes=buffer ? surfaces[0].bytes : image.bytes;
    intel_umd_buffer_description_t description={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,
        r->allocation_bytes,0,0};
    D3DDDI_ALLOCATIONINFO allocation={0};
    allocation.pPrivateDriverData=&description; allocation.PrivateDriverDataSize=sizeof(description);
    D3DDDICB_ALLOCATE request={0}; request.hResource=r->runtime;
    request.NumAllocations=1; request.pAllocationInfo=&allocation;
    result=d->cb.pfnAllocateCb(d->runtime,&request);
    if (FAILED(result)) {
        metadata_lock(); memset(r,0,sizeof(*r)); metadata_unlock();
        return device_leave(d,result);
    }
    r->allocation=allocation.hAllocation;
    if (!r->allocation) r->orphan=1;
    d->resources++;
    if (r->orphan) {
        resource_collect_orphans(d);
        return device_leave(d,DRIVER_ERROR);
    }
    args->hResource=(HANDLE)(UINT_PTR)r->id;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_destroy_resource(HANDLE h,HANDLE resource)
{
    ENTER(); resource_t *r=get_resource(d,resource);
    if (!r) return device_leave(d,INVALID_CALL);
    if (r->lock_count) return device_leave(d,INVALID_CALL);
    if (r->cpu_held) { /* A failed Lock's null mapping still needs retirement. */
        D3DDDICB_UNLOCK release={1,&r->allocation};
        result=d->cb.pfnUnlockCb(d->runtime,&release);
        if (FAILED(result)) return device_leave(d,result);
        r->cpu_held=0; r->mapping=NULL;
    }
    D3DDDICB_DEALLOCATE request={r->runtime,0,NULL};
    result=d->cb.pfnDeallocateCb(d->runtime,&request);
    if (SUCCEEDED(result)) {
        if (d->vertex_buffer==r->id) d->vertex_buffer=0;
        if (d->index_buffer==r->id) {d->index_buffer=0;d->index_stride=0;}
        if (d->target==r->id) d->target=0;
        for (UINT i=0;i<INTEL_UMD_MAX_TEXTURES;i++)
            if (d->samplers[i].texture==r->id) d->samplers[i].texture=0;
        metadata_lock(); memset(r,0,sizeof(*r)); d->resources--; metadata_unlock();
    }
    return device_leave(d,result);
}
static HRESULT APIENTRY umd_lock(HANDLE h,D3DDDIARG_LOCK *args)
{
    ENTER(); resource_t *r=args ? get_resource(d,args->hResource) : NULL;
    surface_t *s=r && args->SubResourceIndex<r->levels ? &r->surfaces[args->SubResourceIndex] : NULL;
    /* One allocation lease covers all mapped levels. A 2D lock exposes only
     * its pixel rows: row padding may contain an adjacent mip. */
    if (!s || s->locked ||
        (args->Flags.Value&~(1u|2u|0x200u|((r->vertex_buffer || r->index_buffer) ? 0x10u : 0x20u))) ||
        (args->Flags.ReadOnly && args->Flags.WriteOnly) ||
        (args->Flags.RangeValid && (!args->Range.Size || args->Range.Offset>s->bytes ||
          args->Range.Size>s->bytes-args->Range.Offset)) ||
        (args->Flags.AreaValid && (args->Area.left<0 || args->Area.top<0 ||
          args->Area.right<=args->Area.left || args->Area.bottom<=args->Area.top ||
          (UINT)args->Area.right>s->width || (UINT)args->Area.bottom>s->height)))
        return device_leave(d,INVALID_CALL);
    UINT offset=args->Flags.RangeValid ? args->Range.Offset :
        args->Flags.AreaValid ? (UINT)args->Area.top*s->pitch+(UINT)args->Area.left*4 : 0;
    if (r->cpu_held && !r->lock_count) {
        D3DDDICB_UNLOCK release={1,&r->allocation};
        result=d->cb.pfnUnlockCb(d->runtime,&release);
        if (FAILED(result)) return device_leave(d,result);
        r->cpu_held=0; r->mapping=NULL;
    }
    if (!r->lock_count) {
        D3DDDICB_LOCK request={0}; request.hAllocation=r->allocation;
        request.Flags.LockEntire=1; request.Flags.DonotWait=args->Flags.DoNotWait;
        /* A later mip lock may read OR write while earlier views stay live.
         * Acquire read/write ownership up front, with full GPU synchronization.
         * Single-level resources can retain their narrower access flags. */
        if (r->levels==1) {
            request.Flags.ReadOnly=args->Flags.ReadOnly; request.Flags.WriteOnly=args->Flags.WriteOnly;
        }
        result=d->cb.pfnLockCb(d->runtime,&request);
        if (FAILED(result)) return device_leave(d,result);
        r->allocation=request.hAllocation; r->cpu_held=1;
        if (!request.pData) {
            D3DDDICB_UNLOCK release={1,&r->allocation};
            if (SUCCEEDED(d->cb.pfnUnlockCb(d->runtime,&release))) r->cpu_held=0;
            return device_leave(d,DRIVER_ERROR);
        }
        r->mapping=request.pData;
    }
    s->locked=1; r->lock_count++;
    args->pSurfData=(BYTE *)r->mapping+s->offset+offset;
    args->Pitch=s->pitch; args->SlicePitch=s->bytes;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_unlock(HANDLE h,const D3DDDIARG_UNLOCK *args)
{
    ENTER(); resource_t *r=args ? get_resource(d,args->hResource) : NULL;
    surface_t *s=r && args->SubResourceIndex<r->levels ? &r->surfaces[args->SubResourceIndex] : NULL;
    if (!s || !s->locked || args->Flags.Value) return device_leave(d,INVALID_CALL);
    if (r->lock_count==1) {
        D3DDDICB_UNLOCK request={1,&r->allocation};
        result=d->cb.pfnUnlockCb(d->runtime,&request);
        if (FAILED(result)) return device_leave(d,result);
        r->cpu_held=0; r->mapping=NULL;
    }
    s->locked=0; r->lock_count--;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_blt(HANDLE h,const D3DDDIARG_BLT *args)
{
    ENTER();
    resource_t *sr=args ? get_resource(d,args->hSrcResource) : NULL;
    surface_t *src=sr && args->SrcSubResourceIndex<sr->levels ? &sr->surfaces[args->SrcSubResourceIndex] : NULL;
    resource_t *dr=args ? get_resource(d,args->hDstResource) : NULL;
    surface_t *dst=dr && args->DstSubResourceIndex<dr->levels ? &dr->surfaces[args->DstSubResourceIndex] : NULL;
    if (!src || !dst || sr==dr || sr->cpu_held || dr->cpu_held || sr->vertex_buffer || dr->vertex_buffer || sr->index_buffer || dr->index_buffer || sr->format!=dr->format ||
        args->Flags.Value || args->ColorKey ||
        args->SrcRect.left<0 || args->SrcRect.top<0 || args->DstRect.left<0 || args->DstRect.top<0 ||
        args->SrcRect.right<=args->SrcRect.left || args->SrcRect.bottom<=args->SrcRect.top ||
        args->DstRect.right<=args->DstRect.left || args->DstRect.bottom<=args->DstRect.top ||
        (UINT)args->SrcRect.right>src->width || (UINT)args->SrcRect.bottom>src->height ||
        (UINT)args->DstRect.right>dst->width || (UINT)args->DstRect.bottom>dst->height ||
        args->SrcRect.right-args->SrcRect.left!=args->DstRect.right-args->DstRect.left ||
        args->SrcRect.bottom-args->SrcRect.top!=args->DstRect.bottom-args->DstRect.top)
        return device_leave(d,INVALID_CALL);
    if (!d->commands || d->command_bytes<sizeof(intel_umd_copy32_t) || !d->allocations ||
        d->allocation_count<2 || !d->patches || d->patch_count<2) return device_leave(d,DRIVER_ERROR);
    UINT sx=sr->image.mip[args->SrcSubResourceIndex].x+(UINT)args->SrcRect.left;
    UINT sy=sr->image.mip[args->SrcSubResourceIndex].y+(UINT)args->SrcRect.top;
    UINT dx=dr->image.mip[args->DstSubResourceIndex].x+(UINT)args->DstRect.left;
    UINT dy=dr->image.mip[args->DstSubResourceIndex].y+(UINT)args->DstRect.top;
    intel_umd_copy32_t command={INTEL_UMD_COMMAND_MAGIC,
        sx || sy || dx || dy ? INTEL_UMD_COPY_RECT_VERSION : INTEL_UMD_COMMAND_VERSION,INTEL_UMD_COPY32,
        sizeof(command),(UINT)(args->SrcRect.right-args->SrcRect.left),(UINT)(args->SrcRect.bottom-args->SrcRect.top),
        src->pitch,dst->pitch,0,0,{sx,sy,dx,dy}};
    memcpy(d->commands,&command,sizeof(command));
    memset(d->allocations,0,2*sizeof(*d->allocations));
    memset(d->patches,0,2*sizeof(*d->patches));
    d->allocations[0].hAllocation=sr->allocation;
    d->allocations[1].hAllocation=dr->allocation; d->allocations[1].WriteOperation=1;
    d->patches[0].AllocationIndex=0; d->patches[0].PatchOffset=32;
    d->patches[1].AllocationIndex=1; d->patches[1].SlotId=1; d->patches[1].PatchOffset=40;
    D3DDDICB_RENDER request={0}; request.hContext=d->context;
    request.CommandLength=sizeof(command); request.NumAllocations=request.NumPatchLocations=2;
    result=d->cb.pfnRenderCb(d->runtime,&request);
    if (SUCCEEDED(result)) {
        d->commands=request.pNewCommandBuffer; d->command_bytes=request.NewCommandBufferSize;
        d->allocations=request.pNewAllocationList; d->allocation_count=request.NewAllocationListSize;
        d->patches=request.pNewPatchLocationList; d->patch_count=request.NewPatchLocationListSize;
    }
    return device_leave(d,result);
}
static HRESULT APIENTRY umd_create_pixel_shader(HANDLE h,D3DDDIARG_CREATEPIXELSHADER *args,const UINT *code)
{
    ENTER();
    if (!args || !code || !args->CodeSize || (args->CodeSize&3) || args->CodeSize>65536)
        return device_leave(d,INVALID_CALL);
    uint32_t status=INTEL_D3D9_COMPILE_OK;
    if (!d->compiler) status=intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_ABI,d->pci_id,&d->compiler);
    const struct intel_d3d9_program *program=NULL;
    if (!status) status=intel_d3d9_compile_pixel(d->compiler,code,args->CodeSize,&program,NULL);
    /* Keep the qualified arithmetic ABI for old programs and private Clear.
     * Resource programs opt into the sampled packet/compiler together. */
    if (status==INTEL_D3D9_COMPILE_UNSUPPORTED) {
        status=INTEL_D3D9_COMPILE_OK;
        if (!d->resource_compiler) status=intel_d3d9_compiler_create(
            INTEL_D3D9_COMPILER_RESOURCE_ABI,d->pci_id,&d->resource_compiler);
        if (!status) status=intel_d3d9_compile_pixel(d->resource_compiler,code,args->CodeSize,&program,NULL);
    }
    if (status) return device_leave(d,status==INTEL_D3D9_COMPILE_NOMEM ? E_OUTOFMEMORY :
        status==INTEL_D3D9_COMPILE_UNSUPPORTED ? NOT_AVAILABLE :
        status==INTEL_D3D9_COMPILE_INVALID ? INVALID_CALL : DRIVER_ERROR);
    pixel_shader_t *shader=calloc(1,sizeof(*shader));
    if (!shader) { intel_d3d9_program_free(program); return device_leave(d,E_OUTOFMEMORY); }
    metadata_lock(); shader->id=new_id(); metadata_unlock();
    if (!shader->id) {
        intel_d3d9_program_free(program); free(shader); return device_leave(d,E_OUTOFMEMORY);
    }
    shader->program=program; shader->next=d->shaders; d->shaders=shader;
    args->ShaderHandle=(HANDLE)(UINT_PTR)shader->id;
    return device_leave(d,S_OK);
}

static HRESULT APIENTRY umd_set_pixel_shader(HANDLE h,HANDLE handle)
{
    ENTER();
    pixel_shader_t *shader=d->shaders;
    while (shader && (UINT_PTR)handle!=shader->id) shader=shader->next;
    if (handle && !shader) return device_leave(d,INVALID_CALL);
    if (shader) {
        result=shader_upload(d,shader);
        if (FAILED(result)) return device_leave(d,result);
    }
    d->pixel_shader=shader;
    return device_leave(d,S_OK);
}

static HRESULT APIENTRY umd_delete_pixel_shader(HANDLE h,HANDLE handle)
{
    ENTER();
    pixel_shader_t **link=&d->shaders;
    while (*link && (UINT_PTR)handle!=(*link)->id) link=&(*link)->next;
    if (!*link) return device_leave(d,INVALID_CALL);
    pixel_shader_t *shader=*link;
    result=shader_release_variants(d,shader);
    if (FAILED(result)) return device_leave(d,result);
    *link=shader->next;
    if (d->pixel_shader==shader) d->pixel_shader=NULL;
    shader_free(shader);
    return device_leave(d,S_OK);
}

static HRESULT APIENTRY umd_create_vertex_shader(HANDLE h,D3DDDIARG_CREATEVERTEXSHADERFUNC *args,const UINT *code)
{
    ENTER();
    if (!args || !code || !args->Size || (args->Size&3) || args->Size>65536)
        return device_leave(d,INVALID_CALL);
    UINT status=0;
    if (!d->vertex_compiler) status=intel_d3d9_compiler_create(
        INTEL_D3D9_COMPILER_VERTEX_ABI,d->pci_id,&d->vertex_compiler);
    const struct intel_d3d9_vertex_program *program=NULL;
    if (!status) status=intel_d3d9_compile_vertex(d->vertex_compiler,code,args->Size,&program,NULL);
    if (status) return device_leave(d,compile_result(status));
    pixel_shader_t *shader=calloc(1,sizeof(*shader));
    if (!shader) { intel_d3d9_program_free(&program->base); return device_leave(d,E_OUTOFMEMORY); }
    shader->program=&program->base;
    metadata_lock(); shader->id=new_id(); metadata_unlock();
    if (!shader->id) { shader_free(shader); return device_leave(d,E_OUTOFMEMORY); }
    shader->next=d->vertex_shaders; d->vertex_shaders=shader;
    args->ShaderHandle=(HANDLE)(UINT_PTR)shader->id;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_vertex_shader(HANDLE h,HANDLE handle)
{
    ENTER(); pixel_shader_t *shader=d->vertex_shaders;
    while (shader && (UINT_PTR)handle!=shader->id) shader=shader->next;
    if (handle && !shader) return device_leave(d,INVALID_CALL);
    if (shader) {
        result=shader_upload(d,shader);
        if (FAILED(result)) return device_leave(d,result);
    }
    d->vertex_shader=shader; return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_delete_vertex_shader(HANDLE h,HANDLE handle)
{
    ENTER(); pixel_shader_t **link=&d->vertex_shaders;
    while (*link && (UINT_PTR)handle!=(*link)->id) link=&(*link)->next;
    if (!*link) return device_leave(d,INVALID_CALL);
    pixel_shader_t *shader=*link;
    result=shader_release(d,shader);
    if (FAILED(result)) return device_leave(d,result);
    *link=shader->next;
    if (d->vertex_shader==shader) d->vertex_shader=NULL;
    shader_free(shader); return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_vertex_shader_const(HANDLE h,const D3DDDIARG_SETVERTEXSHADERCONST *args,const VOID *values)
{
    ENTER();
    if (!args || args->Register>256 || args->Count>256-args->Register || (args->Count && !values))
        return device_leave(d,INVALID_CALL);
    if (args->Count) memcpy(d->vertex_constants[args->Register],values,args->Count*16);
    return device_leave(d,S_OK);
}

static HRESULT APIENTRY umd_create_vertex_decl(HANDLE h,D3DDDIARG_CREATEVERTEXSHADERDECL *args,
                                              const D3DDDIVERTEXELEMENT *elements)
{
    ENTER();
    if (!args || !elements || !args->NumVertexElements) return device_leave(d,INVALID_CALL);
    /* POSITIONT remains float4. TEXCOORDs use native Gen9 vertex conversion,
     * including component defaults, normalization and D3DCOLOR swizzle. */
    if (args->NumVertexElements>16) return device_leave(d,NOT_AVAILABLE);
    UINT position=~0u,mask=0,offsets[8]={0},types[8]={0},typed=0;
    UINT semantics[16]={0},attributes[16]={0};
    for (UINT i=0;i<args->NumVertexElements;i++) {
        const D3DDDIVERTEXELEMENT *e=&elements[i];
        intel_gen9_vertex_format_t format;
        if (e->Stream || (e->Offset&3) || !intel_gen9_vertex_format(e->Type,&format) || e->Offset>2048-format.bytes ||
            e->Method!=D3DDECLMETHOD_DEFAULT || e->Usage>D3DDECLUSAGE_SAMPLE || e->UsageIndex>15)
            return device_leave(d,NOT_AVAILABLE);
        semantics[i]=e->Usage|((UINT)e->UsageIndex<<16);
        attributes[i]=e->Offset|((UINT)e->Type<<16);
        for (UINT j=0;j<i;j++) if (semantics[j]==semantics[i]) return device_leave(d,NOT_AVAILABLE);
        if (e->Usage==D3DDECLUSAGE_POSITIONT) {
            if (e->UsageIndex || e->Type!=D3DDECLTYPE_FLOAT4) return device_leave(d,NOT_AVAILABLE);
            position=e->Offset;
        }
        else if (e->Usage==D3DDECLUSAGE_TEXCOORD && e->UsageIndex<8 && !(mask&(1u<<e->UsageIndex))) {
            mask|=1u<<e->UsageIndex; offsets[e->UsageIndex]=e->Offset; types[e->UsageIndex]=e->Type;
            if (e->Type!=D3DDECLTYPE_FLOAT4) typed=1;
        }
    }
    if (position!=~0u) {
        for (UINT i=0;i<args->NumVertexElements;i++) if (!semantics[i]) return device_leave(d,NOT_AVAILABLE);
        for (UINT i=0;i<8;i++) if ((mask&(1u<<i)) && offsets[i]<position) typed=1;
    }
    vertex_decl_t *decl=calloc(1,sizeof(*decl));
    if (!decl) return device_leave(d,E_OUTOFMEMORY);
    metadata_lock(); decl->id=new_id(); metadata_unlock();
    if (!decl->id) { free(decl); return device_leave(d,E_OUTOFMEMORY); }
    decl->offset=position; decl->texcoord_mask=mask;
    decl->count=args->NumVertexElements; decl->pretransformed=position!=~0u;
    memcpy(decl->semantics,semantics,sizeof(semantics)); memcpy(decl->attributes,attributes,sizeof(attributes));
    memcpy(decl->texcoord_offset,offsets,sizeof(offsets));
    memcpy(decl->texcoord_type,types,sizeof(types)); decl->typed=typed;
    decl->next=d->declarations; d->declarations=decl;
    args->ShaderHandle=(HANDLE)(UINT_PTR)decl->id;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_vertex_decl(HANDLE h,HANDLE handle)
{
    ENTER(); vertex_decl_t *decl=d->declarations;
    while (decl && (UINT_PTR)handle!=decl->id) decl=decl->next;
    if (handle && !decl) return device_leave(d,INVALID_CALL);
    d->vertex_decl=decl; return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_delete_vertex_decl(HANDLE h,HANDLE handle)
{
    ENTER(); vertex_decl_t **link=&d->declarations;
    while (*link && (UINT_PTR)handle!=(*link)->id) link=&(*link)->next;
    if (!*link) return device_leave(d,INVALID_CALL);
    vertex_decl_t *decl=*link; *link=decl->next;
    if (d->vertex_decl==decl) d->vertex_decl=NULL;
    free(decl); return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_stream_source(HANDLE h,const D3DDDIARG_SETSTREAMSOURCE *args)
{
    ENTER();
    if (!args || args->Stream) return device_leave(d,INVALID_CALL);
    resource_t *r=args->hVertexBuffer ? get_resource(d,args->hVertexBuffer) : NULL;
    if (args->hVertexBuffer && (!r || !r->vertex_buffer || (args->Offset&3) ||
        args->Offset>r->surfaces[0].bytes || (args->Stride&3))) return device_leave(d,INVALID_CALL);
    if (r && (args->Stride<4 || args->Stride>2048)) return device_leave(d,NOT_AVAILABLE);
    d->vertex_buffer=r ? r->id : 0; d->stream_offset=args->Offset; d->stream_stride=args->Stride;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_indices(HANDLE h,const D3DDDIARG_SETINDICES *args)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    resource_t *r=args->hIndexBuffer ? get_resource(d,args->hIndexBuffer) : NULL;
    if (args->hIndexBuffer && (!r || !r->index_buffer ||
        args->Stride!=(r->format==D3DDDIFMT_INDEX16 ? 2u : 4u))) return device_leave(d,INVALID_CALL);
    d->index_buffer=r ? r->id : 0;d->index_stride=r ? args->Stride : 0;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_render_target(HANDLE h,const D3DDDIARG_SETRENDERTARGET *args)
{
    ENTER();
    if (!args || args->RenderTargetIndex || args->SubResourceIndex) return device_leave(d,INVALID_CALL);
    resource_t *r=args->hRenderTarget ? get_resource(d,args->hRenderTarget) : NULL;
    if (args->hRenderTarget && (!r || !r->render_target || r->vertex_buffer)) return device_leave(d,INVALID_CALL);
    /* Both byte layouts have native surface states. XRGB blending forces
     * destination alpha to one through B8G8R8X8_UNORM, not stored X bits. */
    if (r && r->format!=D3DDDIFMT_A8R8G8B8 && r->format!=D3DDDIFMT_X8R8G8B8)
        return device_leave(d,NOT_AVAILABLE);
    d->target=r ? r->id : 0; return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_viewport(HANDLE h,const D3DDDIARG_VIEWPORTINFO *args)
{
    ENTER();
    if (!args || !args->Width || !args->Height || args->Width>4096 || args->Height>4096 ||
        args->X>4096-args->Width || args->Y>4096-args->Height) return device_leave(d,INVALID_CALL);
    d->viewport=*args; d->viewport_valid=1; return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_scissor(HANDLE h,const RECT *args)
{
    ENTER();
    if (!args || args->left<0 || args->top<0 || args->right<args->left || args->bottom<args->top ||
        args->right>4096 || args->bottom>4096) return device_leave(d,INVALID_CALL);
    d->scissor=*args; return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_render_state(HANDLE h,const D3DDDIARG_RENDERSTATE *args)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    UINT value=args->Value;
    switch (args->State) {
    case D3DDDIRS_SCISSORTESTENABLE:
        if (value>1) return device_leave(d,INVALID_CALL);
        d->scissor_enabled=value; break;
    case D3DDDIRS_ZENABLE:
        if (value) return device_leave(d,NOT_AVAILABLE);
        d->depth_enabled=0; break;
    case D3DDDIRS_CULLMODE:
        if (value!=D3DCULL_NONE) return device_leave(d,NOT_AVAILABLE);
        d->cull=value; break;
    case D3DDDIRS_FILLMODE:
        if (value!=D3DFILL_SOLID) return device_leave(d,NOT_AVAILABLE);
        break;
    case D3DDDIRS_COLORWRITEENABLE:
        if (value&~15u) return device_leave(d,INVALID_CALL);
        d->color_write=value; break;
    case D3DDDIRS_ALPHABLENDENABLE: case D3DDDIRS_SEPARATEALPHABLENDENABLE:
        if (value>1) return device_leave(d,INVALID_CALL);
        if (args->State==D3DDDIRS_ALPHABLENDENABLE) d->blend_enabled=value;
        else d->blend_separate=value;
        break;
    case D3DDDIRS_SRCBLEND: case D3DDDIRS_DESTBLEND:
    case D3DDDIRS_SRCBLENDALPHA: case D3DDDIRS_DESTBLENDALPHA:
        if (!value || value>17) return device_leave(d,INVALID_CALL);
        /* Obsolete coupled factors and dual-source output require their own
         * contracts. Reject them before changing the previous state. */
        if (value==12 || value==13 || value>=16) return device_leave(d,NOT_AVAILABLE);
        if (args->State==D3DDDIRS_SRCBLEND) d->blend_src=value;
        else if (args->State==D3DDDIRS_DESTBLEND) d->blend_dst=value;
        else if (args->State==D3DDDIRS_SRCBLENDALPHA) d->blend_src_alpha=value;
        else d->blend_dst_alpha=value;
        break;
    case D3DDDIRS_BLENDOP: case D3DDDIRS_BLENDOPALPHA:
        if (value<1 || value>5) return device_leave(d,INVALID_CALL);
        if (args->State==D3DDDIRS_BLENDOP) d->blend_op=value;
        else d->blend_op_alpha=value;
        break;
    case D3DDDIRS_BLENDFACTOR: d->blend_factor=value; break;
    case D3DDDIRS_ZWRITEENABLE:
    case D3DDDIRS_SCENECAPTURE:
        if (value>1) return device_leave(d,INVALID_CALL);
        break; /* No depth surface; Begin/EndScene do not submit work. */
    case D3DDDIRS_ALPHATESTENABLE:
    case D3DDDIRS_STENCILENABLE: case D3DDDIRS_FOGENABLE:
    case D3DDDIRS_DITHERENABLE: case D3DDDIRS_SRGBWRITEENABLE:
        if (value) return device_leave(d,NOT_AVAILABLE);
        break;
    default: return device_leave(d,NOT_AVAILABLE);
    }
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_pixel_shader_const(HANDLE h,const D3DDDIARG_SETPIXELSHADERCONST *args,const FLOAT *values)
{
    ENTER();
    if (!args || args->Register>224 || args->Count>224-args->Register || (args->Count && !values))
        return device_leave(d,INVALID_CALL);
    if (args->Count) memcpy(d->pixel_constants[args->Register],values,(size_t)args->Count*16);
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_texture(HANDLE h,UINT stage,HANDLE handle)
{
    ENTER();
    if (stage>=INTEL_UMD_MAX_TEXTURES) return device_leave(d,NOT_AVAILABLE);
    resource_t *r=handle ? get_resource(d,handle) : NULL;
    if (handle && (!r || !r->texture)) return device_leave(d,INVALID_CALL);
    /* Resolve the current allocation after any rename at Draw, not at bind. */
    d->samplers[stage].texture=r ? r->id : 0;
    return device_leave(d,S_OK);
}
static HRESULT APIENTRY umd_set_texture_state(HANDLE h,const D3DDDIARG_TEXTURESTAGESTATE *args)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    if (args->Stage>=INTEL_UMD_MAX_TEXTURES) return device_leave(d,NOT_AVAILABLE);
    sampler_t next=d->samplers[args->Stage];
    UINT v=args->Value;
    switch (args->State) {
    case D3DDDITSS_ADDRESSU: case D3DDDITSS_ADDRESSV: case D3DDDITSS_ADDRESSW:
        if (v<D3DTADDRESS_WRAP || v>D3DTADDRESS_MIRRORONCE) return device_leave(d,INVALID_CALL);
        if (args->State==D3DDDITSS_ADDRESSU) next.state.address_u=v;
        if (args->State==D3DDDITSS_ADDRESSV) next.state.address_v=v;
        if (args->State==D3DDDITSS_ADDRESSW) next.state.address_w=v;
        break;
    case D3DDDITSS_MINFILTER: case D3DDDITSS_MAGFILTER: case D3DDDITSS_MIPFILTER:
        if (v>D3DTEXF_LINEAR) return device_leave(d,NOT_AVAILABLE);
        if (!v && args->State!=D3DDDITSS_MIPFILTER) return device_leave(d,INVALID_CALL);
        if (args->State==D3DDDITSS_MINFILTER) next.state.min_filter=v;
        if (args->State==D3DDDITSS_MAGFILTER) next.state.mag_filter=v;
        if (args->State==D3DDDITSS_MIPFILTER) next.state.mip_filter=v;
        break;
    case D3DDDITSS_BORDERCOLOR: next.state.border_argb=v; break;
    case D3DDDITSS_SRGBTEXTURE:
        if (v>1) return device_leave(d,NOT_AVAILABLE);
        next.srgb=v; break;
    case D3DDDITSS_MAXMIPLEVEL: next.max_mip=v; break;
    case D3DDDITSS_MIPMAPLODBIAS: {
        FLOAT bias; memcpy(&bias,&v,sizeof(bias));
        if ((v&0x7f800000u)==0x7f800000u) return device_leave(d,INVALID_CALL);
        if (bias< -16.0f || bias>4095.0f/256.0f) return device_leave(d,NOT_AVAILABLE);
        next.state.lod_bias=(int32_t)(bias*256.0f+(bias<0 ? -0.5f : 0.5f));
        break;
    }
    case D3DDDITSS_MAXANISOTROPY:
        if (v!=1) return device_leave(d,NOT_AVAILABLE);
        break;
    case D3DDDITSS_ELEMENTINDEX: case D3DDDITSS_DMAPOFFSET:
        if (v) return device_leave(d,NOT_AVAILABLE);
        break;
    default: return device_leave(d,NOT_AVAILABLE);
    }
    d->samplers[args->Stage]=next;
    return device_leave(d,S_OK);
}

/* Allocation entries name unique images; patches name sampler roles, so
 * several roles may share one retained image. Publish only after validation. */
static HRESULT umd_submit_sampled(device_t *d,resource_t *vertices,resource_t *target,
    pixel_shader_t *shader,intel_umd_draw32_t command,const index_draw_t *indices)
{
    const struct intel_d3d9_program *p=shader->program;
    pixel_shader_t *vs=d->vertex_decl->pretransformed ? NULL : d->vertex_shader;
    const struct intel_d3d9_vertex_program *vp=vs ? (const void *)vs->program : NULL;
    UINT texcoords=vp ? (vp->output_mask>>3)&255 : d->vertex_decl->texcoord_mask;
    if (p->texture_mask!=p->sampler_mask || (p->texture_mask&~0xffffu) ||
        (p->texcoord_mask&~texcoords) ||
        p->parameter_count>INTEL_UMD_MAX_PARAMETERS || !p->simd_mask || (p->simd_mask&~7u)) return NOT_AVAILABLE;
    memset(&d->sampled,0,sizeof(d->sampled));
    /* Both versions share the complete prefix up to textures. Their constant
     * arrays begin at different offsets; never use the V5 array for V7. */
    intel_umd_draw_sampled_t *packet=&d->sampled.packet.sampled;
    intel_umd_draw_vertex_shader_t *vertex_packet=&d->sampled.packet.vertex;
    intel_umd_draw_indexed_t *index_packet=&d->sampled.packet.indexed;
    UINT *values=indices ? index_packet->values : vp ? vertex_packet->values : packet->values;
    D3DDDI_ALLOCATIONLIST *allocations=d->sampled.allocations;
    D3DDDI_PATCHLOCATIONLIST *patches=d->sampled.patches;
    BOOL typed=!vp && (indices || d->vertex_decl->typed);
    UINT position=typed ? d->vertex_decl->offset : 0;
    UINT first_image=3+(vp!=NULL)+(indices!=NULL),count=first_image,patch_count=first_image,extent=vp ? 0 : position+16;
    packet->position_offset=position;
    if (vp) {
        if (vp->base.parameter_count>INTEL_UMD_MAX_PARAMETERS) return NOT_AVAILABLE;
        for (UINT i=0;i<16;i++) if (vp->input_mask&(1u<<i)) {
            UINT j=0;
            while (j<d->vertex_decl->count && d->vertex_decl->semantics[j]!=vp->input_semantics[i]) j++;
            if (j==d->vertex_decl->count) return INVALID_CALL;
            UINT attr=d->vertex_decl->attributes[j],offset=attr&65535;
            intel_gen9_vertex_format_t format;
            if (!intel_gen9_vertex_format(attr>>16,&format) || offset+format.bytes>d->stream_stride) return INVALID_CALL;
            vertex_packet->attributes[i]=attr;
            if (offset+format.bytes>extent) extent=offset+format.bytes;
        }
        vertex_packet->viewport_x=d->viewport.X; vertex_packet->viewport_y=d->viewport.Y;
        vertex_packet->viewport_width=d->viewport.Width; vertex_packet->viewport_height=d->viewport.Height;
        vertex_packet->vertex_parameter_count=vp->base.parameter_count;
    }
    for (UINT i=0;!vp && i<8;i++) if (p->texcoord_mask&(1u<<i)) {
        UINT offset=d->vertex_decl->texcoord_offset[i];
        UINT type=d->vertex_decl->texcoord_type[i];
        intel_gen9_vertex_format_t format;
        if (!intel_gen9_vertex_format(type,&format) || offset+format.bytes>d->stream_stride) return INVALID_CALL;
        if (!typed) offset-=d->vertex_decl->offset;
        packet->texcoord_offset[i]=offset|(typed ? type<<16 : 0);
        if (offset+format.bytes>extent) extent=offset+format.bytes;
    }
    if ((unsigned long long)(command.vertex_count-1)*command.vertex_stride+extent>
        vertices->surfaces[0].bytes-command.vertex_offset) return INVALID_CALL;
    for (UINT i=0;i<INTEL_UMD_MAX_TEXTURES;i++) if (p->texture_mask&(1u<<i)) {
        const sampler_t *s=&d->samplers[i];
        resource_t *r=get_resource(d,(HANDLE)(UINT_PTR)s->texture);
        if (!r || !r->texture || r->cpu_held || r==target || r==vertices) return INVALID_CALL;
        intel_umd_texture_t *t=&packet->textures[i];
        t->width=r->image.width; t->height=r->image.height; t->levels=r->levels;
        t->format=r->format; t->pitch=r->image.pitch; t->count=r->levels; t->srgb=s->srgb;
        t->sampler=s->state;
        UINT first=s->max_mip<r->levels ? s->max_mip : r->levels-1;
        /* With MIPFILTER=NONE the D3D9 HAL oracle ignores MAXMIPLEVEL.
         * Enabled mipmapping clamps absolute LOD; it does not rebase it. */
        if (!s->state.mip_filter) t->count=1;
        else { t->sampler.min_lod=first*256; t->sampler.max_lod=(r->levels-1)*256; }
        UINT index=first_image;
        while (index<count && allocations[index].hAllocation!=r->allocation) index++;
        if (index==count) allocations[count++].hAllocation=r->allocation;
        patches[patch_count].AllocationIndex=index;
        patches[patch_count].SlotId=3+i;
        patches[patch_count++].PatchOffset=offsetof(intel_umd_draw_sampled_t,textures)+i*sizeof(*t);
    }
    UINT bytes=(indices ? offsetof(intel_umd_draw_indexed_t,values) : vp ?
        offsetof(intel_umd_draw_vertex_shader_t,values) : offsetof(intel_umd_draw_sampled_t,values))+
        (p->parameter_count+(vp ? vp->base.parameter_count : 0))*4;
    if (!d->commands || d->command_bytes<bytes || !d->allocations || d->allocation_count<count ||
        !d->patches || d->patch_count<patch_count) return DRIVER_ERROR;
    const UINT *map=(const void *)((const BYTE *)p+p->parameter_map_offset);
    for (UINT i=0;i<p->parameter_count;i++) {
        if (map[i]>=224*4) return DRIVER_ERROR;
        values[i]=d->pixel_constants[map[i]/4][map[i]%4];
    }
    if (vp) {
        map=(const void *)((const BYTE *)vp+vp->base.parameter_map_offset);
        for (UINT i=0;i<vp->base.parameter_count;i++) {
            if (map[i]>=256*4) return DRIVER_ERROR;
            values[p->parameter_count+i]=d->vertex_constants[map[i]/4][map[i]%4];
        }
        HRESULT result=shader_upload(d,vs);
        if (FAILED(result)) return result;
        vertex_packet->vertex_program_bytes=vs->content_bytes;
        allocations[3].hAllocation=vs->allocation;
        patches[3].AllocationIndex=3; patches[3].SlotId=INTEL_UMD_VERTEX_PROGRAM_ROLE;
        patches[3].PatchOffset=offsetof(intel_umd_draw_vertex_shader_t,vertex_program);
    }
    HRESULT result=shader_upload(d,shader);
    if (FAILED(result)) return result;
    command.magic=INTEL_UMD_COMMAND_MAGIC; command.opcode=INTEL_UMD_DRAW32;
    command.version=indices ? INTEL_UMD_DRAW_INDEXED_VERSION : vp ? INTEL_UMD_DRAW_VERTEX_SHADER_VERSION :
        typed ? INTEL_UMD_DRAW_VERTEX_VERSION : INTEL_UMD_DRAW_SAMPLED_VERSION;
    command.bytes=bytes;
    command.destination_width=target->surfaces[0].width; command.destination_height=target->surfaces[0].height;
    command.destination_pitch=target->surfaces[0].pitch;
    command.program_bytes=shader->content_bytes; command.dispatch_mask=p->simd_mask;
    packet->draw=command; packet->parameter_count=p->parameter_count; packet->bound_mask=p->texture_mask;
    allocations[0].hAllocation=vertices->allocation;
    allocations[1].hAllocation=target->allocation; allocations[1].WriteOperation=1;
    allocations[2].hAllocation=shader->allocation;
    for (UINT i=0;i<3;i++) {
        patches[i].AllocationIndex=i; patches[i].SlotId=i; patches[i].PatchOffset=80+i*8;
    }
    if (indices) {
        UINT slot=3+(vp!=NULL);
        index_packet->index_offset=indices->offset;index_packet->index_count=indices->count;
        index_packet->index_stride=indices->stride;index_packet->base_vertex=indices->base_vertex;
        index_packet->min_index=indices->min_index;index_packet->num_vertices=indices->num_vertices;
        allocations[slot].hAllocation=indices->resource->allocation;
        patches[slot].AllocationIndex=slot;patches[slot].SlotId=INTEL_UMD_INDEX_ROLE;
        patches[slot].PatchOffset=offsetof(intel_umd_draw_indexed_t,indices);
    }
    memcpy(d->commands,&d->sampled.packet,bytes);
    memcpy(d->allocations,allocations,count*sizeof(*allocations));
    memcpy(d->patches,patches,patch_count*sizeof(*patches));
    D3DDDICB_RENDER request={0}; request.hContext=d->context;
    request.CommandLength=bytes; request.NumAllocations=count; request.NumPatchLocations=patch_count;
    result=d->cb.pfnRenderCb(d->runtime,&request);
    if (SUCCEEDED(result)) {
        d->commands=request.pNewCommandBuffer; d->command_bytes=request.NewCommandBufferSize;
        d->allocations=request.pNewAllocationList; d->allocation_count=request.NewAllocationListSize;
        d->patches=request.pNewPatchLocationList; d->patch_count=request.NewPatchLocationListSize;
    }
    return result;
}
/* Common primitive publication. Clear supplies private geometry/constants;
 * neither path changes the application's bound resources or render state. */
static HRESULT umd_submit_primitive(device_t *d,D3DKMT_HANDLE vertices,resource_t *target,
    pixel_shader_t *shader,intel_umd_draw32_t command,const UINT *constants,UINT constant_count,
    const intel_umd_rectangle_t *rects,UINT rect_count)
{
    UINT parameters=shader->program->parameter_count;
    BOOL blending=command.version==INTEL_UMD_DRAW_BLEND_VERSION;
    if (parameters>INTEL_UMD_MAX_PARAMETERS || rect_count>INTEL_UMD_MAX_RECTANGLES ||
        (rect_count && (!parameters || !rects))) return NOT_AVAILABLE;
    UINT command_bytes=parameters || blending ? 136+parameters*4 : sizeof(intel_umd_draw32_t);
    command_bytes+=rect_count*sizeof(*rects);
    if (!d->commands || d->command_bytes<command_bytes || !d->allocations ||
        d->allocation_count<3 || !d->patches || d->patch_count<3) return DRIVER_ERROR;
    if (!shader->program->simd_mask || (shader->program->simd_mask&~3u)) return NOT_AVAILABLE;
    HRESULT result=shader_upload(d,shader);
    if (FAILED(result)) return result;
    command.magic=INTEL_UMD_COMMAND_MAGIC;
    command.version=parameters ? INTEL_UMD_DRAW_CONSTANTS_VERSION : INTEL_UMD_COMMAND_VERSION;
    if (rect_count) command.version=INTEL_UMD_DRAW_RECTANGLES_VERSION;
    if (blending) command.version=INTEL_UMD_DRAW_BLEND_VERSION;
    command.opcode=INTEL_UMD_DRAW32; command.bytes=command_bytes;
    command.destination_width=target->surfaces[0].width; command.destination_height=target->surfaces[0].height;
    command.destination_pitch=target->surfaces[0].pitch;
    command.program_bytes=shader->content_bytes; command.dispatch_mask=shader->program->simd_mask;
    if (parameters || blending) {
        intel_umd_draw_rectangles_t packet={0};
        intel_umd_draw_constants_t *snapshot=&packet.constants;
        snapshot->draw=command; snapshot->parameter_count=parameters; snapshot->reserved=rect_count;
        const UINT *map=(const void *)((const unsigned char *)shader->program+shader->program->parameter_map_offset);
        for (UINT i=0;i<parameters;i++) {
            if (map[i]>=constant_count) return DRIVER_ERROR;
            snapshot->values[i]=constants[map[i]];
        }
        if (rect_count) memcpy((unsigned char *)&packet+136+parameters*4,rects,rect_count*sizeof(*rects));
        memcpy(d->commands,&packet,command_bytes);
    } else memcpy(d->commands,&command,sizeof(command));
    memset(d->allocations,0,3*sizeof(*d->allocations)); memset(d->patches,0,3*sizeof(*d->patches));
    d->allocations[0].hAllocation=vertices;
    d->allocations[1].hAllocation=target->allocation; d->allocations[1].WriteOperation=1;
    d->allocations[2].hAllocation=shader->allocation;
    for (UINT i=0;i<3;i++) {
        d->patches[i].AllocationIndex=i; d->patches[i].SlotId=i; d->patches[i].PatchOffset=80+i*8;
    }
    D3DDDICB_RENDER request={0}; request.hContext=d->context;
    request.CommandLength=command_bytes; request.NumAllocations=request.NumPatchLocations=3;
    result=d->cb.pfnRenderCb(d->runtime,&request);
    if (SUCCEEDED(result)) {
        d->commands=request.pNewCommandBuffer; d->command_bytes=request.NewCommandBufferSize;
        d->allocations=request.pNewAllocationList; d->allocation_count=request.NewAllocationListSize;
        d->patches=request.pNewPatchLocationList; d->patch_count=request.NewPatchLocationListSize;
    }
    return result;
}
static HRESULT umd_draw(device_t *d,D3DPRIMITIVETYPE topology,UINT count,UINT start,BOOL byte_offset,
    const D3DDDIARG_DRAWINDEXEDPRIMITIVE *indexed)
{
    if (d->node!=INTEL_UMD_NODE_RENDER || d->depth_enabled || d->cull!=D3DCULL_NONE)
        return NOT_AVAILABLE;
    resource_t *vertices=get_resource(d,(HANDLE)(UINT_PTR)d->vertex_buffer);
    resource_t *target=get_resource(d,(HANDLE)(UINT_PTR)d->target);
    pixel_shader_t *shader=d->pixel_shader;
    if (!vertices || !target || !d->vertex_decl || !shader || !d->viewport_valid ||
        vertices->cpu_held || target->cpu_held) return INVALID_CALL;
    BOOL programmable=!d->vertex_decl->pretransformed;
    if (programmable && !d->vertex_shader) return NOT_AVAILABLE;
    if (!programmable && d->vertex_decl->offset+16>d->stream_stride) return INVALID_CALL;
    if (topology<D3DPT_TRIANGLELIST || topology>D3DPT_TRIANGLEFAN) return NOT_AVAILABLE;
    if ((byte_offset && (start&3)) || !count) return INVALID_CALL;
    unsigned long long vertex_count=topology==D3DPT_TRIANGLELIST ? (unsigned long long)count*3 : (unsigned long long)count+2;
    UINT position=indexed || programmable || (shader->program->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI &&
        d->vertex_decl->typed) ? 0 : d->vertex_decl->offset;
    unsigned long long offset=(unsigned long long)d->stream_offset+position+
        (indexed ? 0 : byte_offset ? start : (unsigned long long)start*d->stream_stride);
    if (vertex_count>65535) return INVALID_CALL;
    index_draw_t indices={0};
    if (indexed) {
        resource_t *buffer=get_resource(d,(HANDLE)(UINT_PTR)d->index_buffer);
        if (!buffer || !buffer->index_buffer || buffer->cpu_held || !indexed->NumVertices ||
            (d->index_stride!=2 && d->index_stride!=4) ||
            (unsigned long long)indexed->MinIndex+indexed->NumVertices>(1ULL<<32)) return INVALID_CALL;
        unsigned long long index_offset=(unsigned long long)indexed->StartIndex*d->index_stride;
        if (index_offset>buffer->surfaces[0].bytes ||
            vertex_count*d->index_stride>buffer->surfaces[0].bytes-index_offset) return INVALID_CALL;
        long long first=(long long)indexed->BaseVertexIndex+indexed->MinIndex;
        if (first<0 || (unsigned long long)first+indexed->NumVertices>0xffffffffu) return INVALID_CALL;
        indices=(index_draw_t){buffer,(UINT)index_offset,(UINT)vertex_count,d->index_stride,
            indexed->MinIndex,indexed->NumVertices,indexed->BaseVertexIndex};
        vertex_count=(unsigned long long)first+indexed->NumVertices;
    }
    if (offset>vertices->surfaces[0].bytes ||
        (vertex_count-1)*d->stream_stride+(programmable ? 0 : 16)>vertices->surfaces[0].bytes-offset) return INVALID_CALL;
    const D3DDDIARG_VIEWPORTINFO *v=&d->viewport;
    if (v->Width>target->surfaces[0].width || v->Height>target->surfaces[0].height || v->X>target->surfaces[0].width-v->Width ||
        v->Y>target->surfaces[0].height-v->Height) return INVALID_CALL;
    UINT x=v->X,y=v->Y,right=x+v->Width,bottom=y+v->Height;
    if (d->scissor_enabled) {
        if ((UINT)d->scissor.left>x) x=d->scissor.left;
        if ((UINT)d->scissor.top>y) y=d->scissor.top;
        if ((UINT)d->scissor.right<right) right=d->scissor.right;
        if ((UINT)d->scissor.bottom<bottom) bottom=d->scissor.bottom;
    }
    if (right<=x || bottom<=y) return S_OK; /* Empty intersection draws no pixels. */
    if (programmable || indexed) {
        HRESULT result=shader_for_vertex(d,&shader);
        if (FAILED(result)) return result;
    }
    intel_umd_draw32_t command={0};
    command.x=x; command.y=y; command.width=right-x; command.height=bottom-y;
    command.vertex_offset=(UINT)offset; command.vertex_stride=d->stream_stride;
    command.vertex_count=(UINT)vertex_count; command.topology=(UINT)topology;
    if (d->blend_enabled || d->color_write!=15 || shader->program->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI) {
        command.version=INTEL_UMD_DRAW_BLEND_VERSION;
        command.reserved2[0]=INTEL_UMD_BLEND_VALID|(d->color_write<<4)|
            (d->blend_enabled ? INTEL_UMD_BLEND_ENABLE : 0)|
            (target->format==D3DDDIFMT_X8R8G8B8 ? INTEL_UMD_BLEND_XRGB : 0);
        command.reserved2[1]=d->blend_src|(d->blend_dst<<5)|(d->blend_op<<10);
        command.reserved2[2]=d->blend_separate ? d->blend_src_alpha|(d->blend_dst_alpha<<5)|(d->blend_op_alpha<<10) :
            command.reserved2[1];
        command.reserved2[3]=d->blend_factor;
    }
    if (shader->program->abi==INTEL_D3D9_COMPILER_RESOURCE_ABI)
        return umd_submit_sampled(d,vertices,target,shader,command,indexed ? &indices : NULL);
    return umd_submit_primitive(d,vertices->allocation,target,shader,command,
                                &d->pixel_constants[0][0],224*4,NULL,0);
}

static HRESULT APIENTRY umd_draw_primitive(HANDLE h,const D3DDDIARG_DRAWPRIMITIVE *args,const UINT *flags)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    if (flags) return device_leave(d,NOT_AVAILABLE); /* Wireframe edge flags. */
    return device_leave(d,umd_draw(d,args->PrimitiveType,args->PrimitiveCount,args->VStart,FALSE,NULL));
}
static HRESULT APIENTRY umd_draw_primitive2(HANDLE h,const D3DDDIARG_DRAWPRIMITIVE2 *args)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    return device_leave(d,umd_draw(d,args->PrimitiveType,args->PrimitiveCount,args->FirstVertexOffset,TRUE,NULL));
}

static HRESULT APIENTRY umd_draw_indexed_primitive(HANDLE h,const D3DDDIARG_DRAWINDEXEDPRIMITIVE *args)
{
    ENTER();
    if (!args) return device_leave(d,INVALID_CALL);
    return device_leave(d,umd_draw(d,args->PrimitiveType,args->PrimitiveCount,0,FALSE,args));
}

static HRESULT clear_prepare(device_t *d)
{
    /* Compile once, with color supplied as a captured per-command constant.
     * The maximum-size quad is immutable and shared only within this device. */
    if (!d->clear_shader.program) {
        static const UINT code[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0x0000ffff};
        UINT status=INTEL_D3D9_COMPILE_OK;
        if (!d->compiler) status=intel_d3d9_compiler_create(INTEL_D3D9_COMPILER_ABI,d->pci_id,&d->compiler);
        if (!status) status=intel_d3d9_compile_pixel(d->compiler,code,sizeof(code),&d->clear_shader.program,NULL);
        if (status) return status==INTEL_D3D9_COMPILE_NOMEM ? E_OUTOFMEMORY : DRIVER_ERROR;
    }
    if (!d->clear_vertices) {
        intel_umd_buffer_description_t layout={INTEL_UMD_BUFFER_MAGIC,INTEL_UMD_BUFFER_VERSION,4096,0,0};
        D3DDDI_ALLOCATIONINFO info={0}; info.pPrivateDriverData=&layout; info.PrivateDriverDataSize=sizeof(layout);
        D3DDDICB_ALLOCATE request={0}; request.NumAllocations=1; request.pAllocationInfo=&info;
        HRESULT result=d->cb.pfnAllocateCb(d->runtime,&request);
        if (FAILED(result)) return result;
        if (!info.hAllocation) return DRIVER_ERROR;
        d->clear_vertices=info.hAllocation;
    }
    if (d->clear_vertices_locked) {
        D3DDDICB_UNLOCK unlock={1,&d->clear_vertices};
        HRESULT result=d->cb.pfnUnlockCb(d->runtime,&unlock);
        if (FAILED(result)) return result;
        d->clear_vertices_locked=0;
    }
    if (!d->clear_vertices_uploaded) {
        D3DDDICB_LOCK lock={0}; lock.hAllocation=d->clear_vertices;
        lock.Flags.LockEntire=lock.Flags.WriteOnly=lock.Flags.DonotWait=1;
        HRESULT result=d->cb.pfnLockCb(d->runtime,&lock);
        if (FAILED(result)) return result;
        d->clear_vertices=lock.hAllocation; d->clear_vertices_locked=1;
        if (!lock.pData) return DRIVER_ERROR;
        static const FLOAT quad[4][4]={{-0.5f,-0.5f,0,1},{4095.5f,-0.5f,0,1},
                                       {-0.5f,4095.5f,0,1},{4095.5f,4095.5f,0,1}};
        memset(lock.pData,0,4096); memcpy(lock.pData,quad,sizeof(quad));
        d->clear_vertices_uploaded=1;
        D3DDDICB_UNLOCK unlock={1,&d->clear_vertices};
        result=d->cb.pfnUnlockCb(d->runtime,&unlock);
        if (FAILED(result)) return result;
        d->clear_vertices_locked=0;
    }
    return S_OK;
}

static BOOL clear_intersection(RECT *r,const RECT *clip)
{
    if (r->left<clip->left) r->left=clip->left;
    if (r->top<clip->top) r->top=clip->top;
    if (r->right>clip->right) r->right=clip->right;
    if (r->bottom>clip->bottom) r->bottom=clip->bottom;
    return r->left<r->right && r->top<r->bottom;
}

static HRESULT APIENTRY umd_clear(HANDLE h,const D3DDDIARG_CLEAR *args,UINT count,const RECT *rects)
{
    ENTER();
    const UINT compute_rects=8; /* D3DCLEAR_COMPUTERECTS, d3dhal.h. */
    if (!args || (args->Flags&~15u) || (count && !rects)) return device_leave(d,INVALID_CALL);
    if (args->Flags&(D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL)) return device_leave(d,NOT_AVAILABLE);
    if (!(args->Flags&D3DCLEAR_TARGET) || (!count && !(args->Flags&compute_rects)))
        return device_leave(d,S_OK);
    if (d->node!=INTEL_UMD_NODE_RENDER) return device_leave(d,NOT_AVAILABLE);
    resource_t *target=get_resource(d,(HANDLE)(UINT_PTR)d->target);
    if (!target || target->cpu_held) return device_leave(d,INVALID_CALL);
    RECT clip={0,0,(LONG)target->surfaces[0].width,(LONG)target->surfaces[0].height};
    if (args->Flags&compute_rects) {
        const D3DDDIARG_VIEWPORTINFO *v=&d->viewport;
        if (!d->viewport_valid || v->Width>target->surfaces[0].width || v->Height>target->surfaces[0].height ||
            v->X>target->surfaces[0].width-v->Width || v->Y>target->surfaces[0].height-v->Height)
            return device_leave(d,INVALID_CALL);
        clip=(RECT){(LONG)v->X,(LONG)v->Y,(LONG)(v->X+v->Width),(LONG)(v->Y+v->Height)};
        if (d->scissor_enabled && !clear_intersection(&clip,&d->scissor))
            return device_leave(d,S_OK);
    }
    /* Validate and capture the entire clipped list before any GPU admission.
     * One Clear is one job: retry after BUSY must never replay an admitted
     * prefix. Larger lists remain explicitly unsupported by this prototype. */
    for (UINT i=0;i<count;i++)
        if (rects[i].left>rects[i].right || rects[i].top>rects[i].bottom)
            return device_leave(d,INVALID_CALL);
    FLOAT color[4]={(FLOAT)((args->FillColor>>16)&255)/255.0f,
                    (FLOAT)((args->FillColor>>8)&255)/255.0f,
                    (FLOAT)(args->FillColor&255)/255.0f,(FLOAT)(args->FillColor>>24)/255.0f};
    UINT values[4]; memcpy(values,color,sizeof(values));
    UINT rectangles=count ? count : 1;
    intel_umd_rectangle_t areas[INTEL_UMD_MAX_RECTANGLES]; UINT used=0;
    for (UINT i=0;i<rectangles;i++) {
        RECT area=count ? rects[i] : clip;
        if (!clear_intersection(&area,&clip)) continue;
        if (used==INTEL_UMD_MAX_RECTANGLES) return device_leave(d,NOT_AVAILABLE);
        areas[used++]=(intel_umd_rectangle_t){(UINT)area.left,(UINT)area.top,
            (UINT)(area.right-area.left),(UINT)(area.bottom-area.top)};
    }
    if (!used) return device_leave(d,S_OK);
    result=clear_prepare(d);
    if (FAILED(result)) return device_leave(d,result);
    intel_umd_draw32_t command={0};
    command.x=areas[0].x; command.y=areas[0].y;
    command.width=areas[0].width; command.height=areas[0].height;
    command.vertex_stride=16; command.vertex_count=4; command.topology=D3DPT_TRIANGLESTRIP;
    if (used>1) { command.x=command.y=0; command.width=target->surfaces[0].width; command.height=target->surfaces[0].height; }
    return device_leave(d,umd_submit_primitive(d,d->clear_vertices,target,&d->clear_shader,command,values,4,
                                              used>1 ? areas : NULL,used>1 ? used : 0));
}

static HRESULT APIENTRY umd_create_device(HANDLE h,D3DDDIARG_CREATEDEVICE *args)
{
    if (!args || !args->hDevice || !args->pCallbacks || !args->pDeviceFuncs || args->Interface!=9 ||
        args->Flags.Value) return INVALID_CALL;
    const D3DDDI_DEVICECALLBACKS *cb=args->pCallbacks;
    if (!cb->pfnCreateContextCb || !cb->pfnDestroyContextCb || !cb->pfnAllocateCb ||
        !cb->pfnDeallocateCb || !cb->pfnLockCb || !cb->pfnUnlockCb || !cb->pfnRenderCb) return INVALID_CALL;
    metadata_lock(); adapter_t *a=get_adapter(h); device_t *d=NULL;
    if (!a || a->version!=args->Version) { metadata_unlock(); return INVALID_CALL; }
    for (UINT i=0;i<DEVICES;i++) if (!devices[i].id) {
        UINT id=new_id(); if (id) { d=&devices[i]; d->id=id; d->busy=1; a->devices++; } break;
    }
    if (!d) { metadata_unlock(); return E_OUTOFMEMORY; }
    d->adapter=a->id; d->runtime=args->hDevice; d->cb=*cb; d->pci_id=a->info.device;
    d->node=a->info.copy_node; d->depth_enabled=1; d->cull=D3DCULL_CCW;
    d->color_write=15;d->blend_src=d->blend_src_alpha=D3DBLEND_ONE;
    d->blend_dst=d->blend_dst_alpha=D3DBLEND_ZERO;
    d->blend_op=d->blend_op_alpha=D3DBLENDOP_ADD;d->blend_factor=0xffffffffu;
    for (UINT i=0;i<INTEL_UMD_MAX_TEXTURES;i++) {
        d->samplers[i].state.min_filter=d->samplers[i].state.mag_filter=D3DTEXF_POINT;
        d->samplers[i].state.address_u=d->samplers[i].state.address_v=
            d->samplers[i].state.address_w=D3DTADDRESS_WRAP;
    }
    metadata_unlock();
    intel_umd_context_description_t layout={INTEL_UMD_CONTEXT_MAGIC,INTEL_UMD_CONTEXT_VERSION,8192,64,64,0};
    D3DDDICB_CREATECONTEXT context={0}; context.NodeOrdinal=a->info.copy_node;
    context.pPrivateDriverData=&layout; context.PrivateDriverDataSize=sizeof(layout);
    HRESULT result=cb->pfnCreateContextCb(d->runtime,&context);
    if (FAILED(result) || !context.hContext) {
        metadata_lock(); a->devices--; memset(d,0,sizeof(*d)); metadata_unlock();
        return FAILED(result) ? result : DRIVER_ERROR;
    }
    d->context=context.hContext; d->commands=context.pCommandBuffer; d->command_bytes=context.CommandBufferSize;
    d->allocations=context.pAllocationList; d->allocation_count=context.AllocationListSize;
    d->patches=context.pPatchLocationList; d->patch_count=context.PatchLocationListSize;
    D3DDDI_DEVICEFUNCS funcs={0}; funcs.pfnDestroyDevice=umd_destroy_device;
    funcs.pfnCreateResource=umd_create_resource; funcs.pfnDestroyResource=umd_destroy_resource;
    funcs.pfnLock=umd_lock; funcs.pfnUnlock=umd_unlock; funcs.pfnBlt=umd_blt;
    funcs.pfnCreatePixelShader=umd_create_pixel_shader;
    funcs.pfnSetPixelShader=umd_set_pixel_shader; funcs.pfnDeletePixelShader=umd_delete_pixel_shader;
    funcs.pfnSetPixelShaderConst=umd_set_pixel_shader_const;
    funcs.pfnSetTexture=umd_set_texture; funcs.pfnSetTextureStageState=umd_set_texture_state;
    funcs.pfnCreateVertexShaderDecl=umd_create_vertex_decl; funcs.pfnSetVertexShaderDecl=umd_set_vertex_decl;
    funcs.pfnDeleteVertexShaderDecl=umd_delete_vertex_decl; funcs.pfnSetStreamSource=umd_set_stream_source;
    funcs.pfnCreateVertexShaderFunc=umd_create_vertex_shader;
    funcs.pfnSetVertexShaderFunc=umd_set_vertex_shader; funcs.pfnDeleteVertexShaderFunc=umd_delete_vertex_shader;
    funcs.pfnSetVertexShaderConst=umd_set_vertex_shader_const;
    funcs.pfnSetRenderTarget=umd_set_render_target; funcs.pfnSetViewport=umd_set_viewport;
    funcs.pfnSetScissorRect=umd_set_scissor; funcs.pfnSetRenderState=umd_set_render_state;
    funcs.pfnSetIndices=umd_set_indices;funcs.pfnDrawIndexedPrimitive=umd_draw_indexed_primitive;
    funcs.pfnDrawPrimitive=umd_draw_primitive; funcs.pfnDrawPrimitive2=umd_draw_primitive2;
    funcs.pfnClear=umd_clear;
    *args->pDeviceFuncs=funcs; args->hDevice=(HANDLE)(UINT_PTR)d->id;
    return device_leave(d,S_OK);
}
HRESULT APIENTRY OpenAdapter(D3DDDIARG_OPENADAPTER *args)
{
    if (!args || !args->hAdapter || args->Interface!=9 || !args->pAdapterFuncs ||
        !args->pAdapterCallbacks || !args->pAdapterCallbacks->pfnQueryAdapterInfoCb) return INVALID_CALL;
    intel_umd_adapter_description_t info={0};
    D3DDDICB_QUERYADAPTERINFO query={&info,sizeof(info)};
    HRESULT result=args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hAdapter,&query);
    if (FAILED(result)) return result;
    if (info.magic!=INTEL_UMD_ADAPTER_MAGIC || info.version!=INTEL_UMD_ADAPTER_VERSION ||
        info.bytes!=sizeof(info) || info.vendor!=0x8086 || info.device!=0x3e9b || info.reserved ||
        !((info.capabilities==INTEL_UMD_CAP_COPY32 && info.copy_node==INTEL_UMD_NODE_COPY) ||
          (info.capabilities==INTEL_UMD_CAP_RCS_COPY32 && info.copy_node==INTEL_UMD_NODE_RENDER))) return NOT_AVAILABLE;
    metadata_lock(); adapter_t *a=NULL;
    for (UINT i=0;i<ADAPTERS;i++) if (!adapters[i].id) {
        UINT id=new_id(); if (id) { a=&adapters[i]; a->id=id; } break;
    }
    if (!a) { metadata_unlock(); return E_OUTOFMEMORY; }
    a->runtime=args->hAdapter; a->version=args->Version; a->info=info;
    D3DDDI_ADAPTERFUNCS funcs={umd_get_caps,umd_create_device,umd_close_adapter};
    *args->pAdapterFuncs=funcs; args->hAdapter=(HANDLE)(UINT_PTR)a->id;
    args->DriverVersion=D3D_UMD_INTERFACE_VERSION;
    metadata_unlock(); return S_OK;
}
