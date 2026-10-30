/* Capture the PE ABI before entering the graphics registry. The registry
 * operates on kernel buffers only and never accesses user memory under lock. */
#include "d3dkmt.h"
#include "d3dkmt_registry.h"
#include "d3dkmt_cpu.h"
#include "d3dkmt_wait.h"
#include "compat32.h"
#include "nt_event.h"
#include "../kernel/sched_signal.h"
extern DWORD WINAPI GetCurrentProcessId(void);

typedef struct { DWORD count,adapters; } KMT_ENUM32;
typedef struct { DWORD adapter,type,data,bytes; } KMT_QUERY32;
typedef struct { DWORD adapter,flags,device,commands,command_bytes,allocations,
    allocation_count,patches,patch_count; } KMT_CREATE32;
_Static_assert(sizeof(KMT_CREATE32)==36,"PE32 create device ABI");
#ifndef __KERNEL_X86__
#include <string.h>
#include <stdlib.h>
#define kmt_allocate malloc
#define kmt_free free
#else
extern void *kmalloc(uint64_t);
extern void kfree(void *);
#define kmt_allocate kmalloc
#define kmt_free kfree
#endif
extern BOOL win32_termination_requested(void);
extern void win32_main_termination_checkpoint(void);

/* Win32 user threads currently execute on the BSP. Short capture/publication
 * sections exclude their unmap/protect/termination interleavings. Allocator/VM
 * providers run outside them. The separately documented immediate residency
 * commit and prepared offer/reclaim commit are bounded metadata providers
 * allowed in a copy section. Preparation and retirement remain unlocked. */
static uint64_t kmt_copy_lock(void)
{
    uint64_t flags=0;
#ifdef __KERNEL_X86__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    return flags;
}
static void kmt_copy_unlock(uint64_t flags)
{
#ifdef __KERNEL_X86__
    if (flags&(1ULL<<9)) __asm__ volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}
static int kmt_current_kernel_pid(void)
{
#ifdef __KERNEL_X86__
    extern int32_t proc_current_pid(void);
    return proc_current_pid();
#else
    return (int)GetCurrentProcessId();
#endif
}
static struct { int pid; DWORD depth; } kmt_scopes[64];
static unsigned scope_lock;
static BOOL kmt_scope_enter(void)
{
    int pid=kmt_current_kernel_pid();
    if (pid<=0) return FALSE;
    uint64_t flags=kmt_copy_lock();
    while (__atomic_exchange_n(&scope_lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    unsigned free_slot=64; BOOL entered=FALSE;
    for (unsigned i=0;i<64;i++) {
        if (!kmt_scopes[i].pid) free_slot=i;
        if (kmt_scopes[i].pid!=pid) continue;
        if (kmt_scopes[i].depth!=UINT32_MAX) { kmt_scopes[i].depth++; entered=TRUE; }
        free_slot=64; break;
    }
    if (!entered && free_slot<64) { kmt_scopes[free_slot].pid=pid; kmt_scopes[free_slot].depth=1; entered=TRUE; }
    __atomic_store_n(&scope_lock,0,__ATOMIC_RELEASE); kmt_copy_unlock(flags);
    return entered;
}
static void kmt_scope_leave(void)
{
    int pid=kmt_current_kernel_pid(); uint64_t flags=kmt_copy_lock();
    while (__atomic_exchange_n(&scope_lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    for (unsigned i=0;i<64;i++) if (kmt_scopes[i].pid==pid) {
        if (!--kmt_scopes[i].depth) kmt_scopes[i].pid=0;
        break;
    }
    __atomic_store_n(&scope_lock,0,__ATOMIC_RELEASE); kmt_copy_unlock(flags);
}
BOOL d3dkmt_deferred_for_kernel_pid(int pid)
{
    if (pid<=0) return FALSE;
    uint64_t flags=kmt_copy_lock(); BOOL found=FALSE;
    while (__atomic_exchange_n(&scope_lock,1,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    for (unsigned i=0;i<64;i++) if (kmt_scopes[i].pid==pid) { found=TRUE; break; }
    __atomic_store_n(&scope_lock,0,__ATOMIC_RELEASE); kmt_copy_unlock(flags);
    return found;
}
BOOL d3dkmt_deferred_current(void)
{ return d3dkmt_deferred_for_kernel_pid(kmt_current_kernel_pid()); }

static BOOL kmt_user_capture(void *destination,const void *source,SIZE_T bytes,BOOL compat)
{
    uint64_t flags=kmt_copy_lock();
    BOOL valid=win32_user_range_readable(source,bytes,compat);
    if (valid) memcpy(destination,source,bytes);
    kmt_copy_unlock(flags); return valid;
}

NTSTATUS WINAPI D3DKMTEnumAdapters(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_ENUM_ADAPTERS output;
    _Static_assert(sizeof(output)==324,"fixed adapter enumeration ABI");
    _Static_assert(D3DKMT_MAX_ADAPTERS<=OKMT_MAX_ENUM_ADAPTERS,"enumeration capacity");
    /* This legacy ABI has a fixed inline array, no input capacity or pointer.
     * Registration/enumeration uses only bounded kernel tables (no allocation
     * or driver callbacks). Keep validation, references and copyout atomic
     * against BSP user unmapping and process cancellation. */
    DWORD pid=GetCurrentProcessId();
    uint64_t flags=kmt_copy_lock();
    if (!win32_user_range_writable(argument,sizeof(output),compat)) {
        kmt_copy_unlock(flags);
        return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status=d3dkmt_enumerate_adapters(pid,OKMT_MAX_ENUM_ADAPTERS,
                                             output.adapters,&output.count);
    if (status==STATUS_SUCCESS) {
        /* Leave unused array entries untouched; never expose stack padding. */
        memcpy(argument,&output,sizeof(output.count)+output.count*sizeof(output.adapters[0]));
    }
    kmt_copy_unlock(flags);
    return status;
}

NTSTATUS WINAPI D3DKMTEnumAdapters2(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T bytes=compat ? sizeof(KMT_ENUM32) : sizeof(OKMT_ENUM_ADAPTERS2);
    if (!win32_user_range_readable(argument,bytes,compat) ||
        !win32_user_range_writable(argument,sizeof(DWORD),compat))
        return STATUS_INVALID_PARAMETER;
    DWORD capacity;
    OKMT_ADAPTER_INFO *destination;
    if (compat) {
        KMT_ENUM32 captured; memcpy(&captured,argument,sizeof(captured));
        capacity=captured.count;
        destination=(PVOID)(ULONG_PTR)captured.adapters;
    } else {
        OKMT_ENUM_ADAPTERS2 captured; memcpy(&captured,argument,sizeof(captured));
        capacity=captured.count;
        destination=captured.adapters;
    }
    OKMT_ADAPTER_INFO output[D3DKMT_MAX_ADAPTERS];
    DWORD count=0, pid=GetCurrentProcessId();
    NTSTATUS status=d3dkmt_enumerate_adapters(pid,capacity,
                                               destination ? output : NULL,&count);
    if (status!=STATUS_SUCCESS) return status;
    if (destination && count) {
        SIZE_T length=count*sizeof(output[0]);
        if (!win32_user_range_writable(destination,length,compat)) {
            for (DWORD i=0;i<count;i++) d3dkmt_close_adapter(pid,output[i].adapter);
            return STATUS_INVALID_PARAMETER;
        }
        memcpy(destination,output,length);
    }
    memcpy(argument,&count,sizeof(count));
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    if (!win32_user_range_readable(argument,sizeof(OKMT_OPEN_ADAPTER_FROM_LUID),compat) ||
        !win32_user_range_writable(argument,sizeof(OKMT_OPEN_ADAPTER_FROM_LUID),compat))
        return STATUS_INVALID_PARAMETER;
    OKMT_OPEN_ADAPTER_FROM_LUID captured;
    memcpy(&captured,argument,sizeof(captured));
    NTSTATUS status=d3dkmt_open_adapter(GetCurrentProcessId(),captured.luid,&captured.adapter);
    if (status==STATUS_SUCCESS)
        memcpy((BYTE *)argument+8,&captured.adapter,sizeof(captured.adapter));
    return status;
}

/* Called only with BSP user preemption excluded. Capture a bounded UTF-16
 * name before taking the registry lock, including a terminator on the last
 * valid page. No allocator/provider callback executes in this transaction. */
static BOOL kmt_capture_name(OKMT_WCHAR *out,const void *name,DWORD capacity,BOOL compat)
{
    ULONG_PTR address=(ULONG_PTR)name;
    if (!address || address>(ULONG_PTR)-1-capacity*sizeof(*out)) return FALSE;
    for (DWORD i=0;i<capacity;i++) {
        const void *p=(const void *)(address+i*sizeof(*out));
        if (!win32_user_range_readable(p,sizeof(*out),compat)) return FALSE;
        memcpy(out+i,p,sizeof(*out));
        if (!out[i]) return TRUE;
    }
    return FALSE;
}

NTSTATUS WINAPI D3DKMTOpenAdapterFromDeviceName(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T bytes=compat ? 16 : sizeof(OKMT_OPEN_ADAPTER_FROM_DEVICE_NAME);
    DWORD pid=GetCurrentProcessId();
    uint64_t flags=kmt_copy_lock();
    if (!win32_user_range_readable(argument,bytes,compat) ||
        !win32_user_range_writable(argument,bytes,compat)) {
        kmt_copy_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    ULONG_PTR name=0;
    SIZE_T pointer_bytes=compat ? 4 : sizeof(void *);
    memcpy(&name,argument,pointer_bytes);
    OKMT_WCHAR captured[260];
    if (!kmt_capture_name(captured,(const void *)name,260,compat)) {
        kmt_copy_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    OKMT_ADAPTER_INFO opened={0}; DWORD source=0;
    NTSTATUS status=d3dkmt_open_adapter_name(pid,captured,FALSE,&opened,&source);
    /* Windows preserves output on a missing device, but clears it for an
     * empty path. Copy the 12 output bytes only, preserving input/padding. */
    if (status==STATUS_SUCCESS || status==STATUS_OBJECT_PATH_SYNTAX_BAD)
        memcpy((BYTE *)argument+pointer_bytes,&opened,12);
    kmt_copy_unlock(flags); return status;
}

NTSTATUS WINAPI D3DKMTOpenAdapterFromGdiDisplayName(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    DWORD pid=GetCurrentProcessId();
    uint64_t flags=kmt_copy_lock();
    OKMT_OPEN_ADAPTER_FROM_GDI_NAME captured;
    if (!win32_user_range_readable(argument,sizeof(captured),compat) ||
        !win32_user_range_writable(argument,sizeof(captured),compat)) {
        kmt_copy_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    memcpy(&captured,argument,sizeof(captured));
    OKMT_ADAPTER_INFO opened; DWORD source;
    NTSTATUS status=d3dkmt_open_adapter_name(pid,captured.name,TRUE,&opened,&source);
    if (status==STATUS_SUCCESS) {
        captured.adapter=opened.adapter; captured.luid=opened.luid; captured.source=source;
        memcpy((BYTE *)argument+64,(BYTE *)&captured+64,16);
    }
    kmt_copy_unlock(flags); return status;
}

extern BOOL gdi32_dc_display_name(HANDLE,WCHAR [32]);
NTSTATUS WINAPI D3DKMTOpenAdapterFromHdc(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T pointer_bytes=compat ? 4 : sizeof(void *);
    SIZE_T bytes=pointer_bytes+16;
    DWORD pid=GetCurrentProcessId();
    uint64_t flags=kmt_copy_lock();
    if (!win32_user_range_readable(argument,bytes,compat) ||
        !win32_user_range_writable(argument,bytes,compat)) {
        kmt_copy_unlock(flags); return STATUS_INVALID_PARAMETER;
    }
    ULONG_PTR dc=0; memcpy(&dc,argument,pointer_bytes);
    OKMT_WCHAR name[32]; OKMT_ADAPTER_INFO opened={0}; DWORD source=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (gdi32_dc_display_name((HANDLE)dc,name)) {
        status=d3dkmt_open_adapter_name(pid,name,TRUE,&opened,&source);
        if (status==STATUS_UNSUCCESSFUL) status=STATUS_INVALID_PARAMETER;
    }
    /* HDC calls clear their outputs on lookup failure in both Windows ABIs. */
    memcpy((BYTE *)argument+pointer_bytes,&opened,12);
    memcpy((BYTE *)argument+pointer_bytes+12,&source,4);
    kmt_copy_unlock(flags); return status;
}

NTSTATUS WINAPI D3DKMTCloseAdapter(PCVOID argument)
{
    if (!win32_user_range_readable(argument,sizeof(OKMT_CLOSE_ADAPTER),g_compat32_mode))
        return STATUS_INVALID_PARAMETER;
    OKMT_CLOSE_ADAPTER captured;
    memcpy(&captured,argument,sizeof(captured));
    return d3dkmt_close_adapter(GetCurrentProcessId(),captured.adapter);
}

NTSTATUS WINAPI D3DKMTQueryAdapterInfo(PCVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T bytes=compat ? sizeof(KMT_QUERY32) : sizeof(OKMT_QUERY_ADAPTER_INFO);
    if (!win32_user_range_readable(argument,bytes,compat)) return STATUS_INVALID_PARAMETER;
    OKMT_QUERY_ADAPTER_INFO captured;
    if (compat) {
        KMT_QUERY32 narrow; memcpy(&narrow,argument,sizeof(narrow));
        captured=(OKMT_QUERY_ADAPTER_INFO){narrow.adapter,narrow.type,
                                          (PVOID)(ULONG_PTR)narrow.data,narrow.bytes};
    } else memcpy(&captured,argument,sizeof(captured));
    DWORD required=d3dkmt_query_size(captured.type);
    if (captured.type==OKMT_QUERY_PRIVATE_DRIVER_DATA &&
        captured.bytes<=D3DKMT_MAX_ADAPTER_PRIVATE_BYTES) required=captured.bytes;
    if (!required || captured.bytes!=required ||
        !win32_user_range_readable(captured.data,required,compat) ||
        !win32_user_range_writable(captured.data,required,compat))
        return STATUS_INVALID_PARAMETER;
    union { OKMT_UMD_FILENAME name; OKMT_QUERY_DEVICE_IDS ids; DWORD value;
            BYTE private_data[D3DKMT_MAX_ADAPTER_PRIVATE_BYTES]; } data;
    memcpy(&data,captured.data,required);
    NTSTATUS status=d3dkmt_query_adapter(GetCurrentProcessId(),captured.adapter,
                                          captured.type,&data,required,compat);
    if (status==STATUS_SUCCESS) memcpy(captured.data,&data,required);
    return status;
}

NTSTATUS WINAPI D3DKMTCreateDevice(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T bytes=compat ? sizeof(KMT_CREATE32) : sizeof(OKMT_CREATE_DEVICE);
    if (!win32_user_range_readable(argument,bytes,compat) ||
        !win32_user_range_writable(argument,bytes,compat)) return STATUS_INVALID_PARAMETER;
    OKMT_CREATE_DEVICE captured;
    KMT_CREATE32 narrow;
    DWORD adapter,options;
    if (compat) {
        memcpy(&narrow,argument,sizeof(narrow)); adapter=narrow.adapter; options=narrow.flags;
    } else {
        memcpy(&captured,argument,sizeof(captured)); adapter=captured.object.adapter; options=captured.flags;
    }
    OKMT_HANDLE device=0;
    NTSTATUS status=d3dkmt_create_device(GetCurrentProcessId(),adapter,options,&device);
    if (status!=STATUS_SUCCESS) return status;
    /* Modern D3D contexts allocate their buffers at CreateContext. Legacy
     * device buffers are not implemented, and LegacyMode is rejected above. */
    if (compat) {
        narrow.device=device; narrow.commands=narrow.command_bytes=0;
        narrow.allocations=narrow.allocation_count=narrow.patches=narrow.patch_count=0;
        memcpy(argument,&narrow,sizeof(narrow));
    } else {
        captured.device=device; captured.command_buffer=NULL; captured.command_bytes=0;
        captured.allocation_list=NULL; captured.allocation_count=0;
        captured.patch_list=NULL; captured.patch_count=0;
        memcpy(argument,&captured,sizeof(captured));
    }
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTDestroyDevice(PCVOID argument)
{
    if (!win32_user_range_readable(argument,sizeof(OKMT_DESTROY_DEVICE),g_compat32_mode))
        return STATUS_INVALID_PARAMETER;
    OKMT_DESTROY_DEVICE captured; memcpy(&captured,argument,sizeof(captured));
    return d3dkmt_destroy_device(GetCurrentProcessId(),captured.device);
}

typedef struct {
    DWORD device,node,engine,flags,private_data,private_bytes,hint,context;
    DWORD commands,command_bytes,allocations,allocation_count,patches,patch_count;
    uint64_t gpu_commands;
} KMT_CONTEXT32;
_Static_assert(sizeof(KMT_CONTEXT32)==64,"PE32 context header");
_Static_assert(sizeof(OKMT_CREATE_CONTEXT)==96,"PE64 context header");

NTSTATUS WINAPI D3DKMTCreateContext(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    SIZE_T bytes=compat ? sizeof(KMT_CONTEXT32) : sizeof(OKMT_CREATE_CONTEXT);
    KMT_CONTEXT32 narrow={0}; OKMT_CREATE_CONTEXT captured={0};
    D3DKMT_CONTEXT_DESCRIPTION description={.compat32=compat};
    const void *private_data;
    DWORD device;
    if (!kmt_user_capture(compat ? (void *)&narrow : (void *)&captured,argument,bytes,compat))
        return STATUS_INVALID_PARAMETER;
    if (compat) {
        device=narrow.device;
        description.node=narrow.node; description.engine=narrow.engine;
        description.flags=narrow.flags; description.hint=narrow.hint;
        description.private_bytes=narrow.private_bytes;
        private_data=(const void *)(uintptr_t)narrow.private_data;
    } else {
        device=captured.device;
        description.node=captured.node; description.engine=captured.engine;
        description.flags=captured.flags; description.hint=captured.hint;
        description.private_bytes=captured.private_bytes; private_data=captured.private_data;
    }
    if (description.private_bytes>D3DKMT_MAX_CONTEXT_PRIVATE_BYTES ||
        (description.private_bytes && !kmt_user_capture(description.private_data,private_data,
            description.private_bytes,compat))) return STATUS_INVALID_PARAMETER;
    SIZE_T offset=compat ? __builtin_offsetof(KMT_CONTEXT32,context) :
        __builtin_offsetof(OKMT_CREATE_CONTEXT,context);
    if (!win32_user_range_writable((BYTE *)argument+offset,bytes-offset,compat)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    OKMT_HANDLE context=0; D3DKMT_CONTEXT_BUFFERS buffers={0};
    DWORD pid=GetCurrentProcessId();
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_create_context(pid,device,&description,&context,&buffers);
    if (status==STATUS_SUCCESS) {
        uint64_t flags=kmt_copy_lock();
        uint64_t addresses[3]={buffers.commands,buffers.allocations,buffers.patches};
        /* SDK ALLOCATIONLIST is 8 bytes; PATCHLOCATIONLIST is 24 bytes.
         * Counts are elements. Multiplication stays in 64 bits. */
        uint64_t lengths[3]={buffers.command_bytes,(uint64_t)buffers.allocation_count*8,
            (uint64_t)buffers.patch_count*24};
        BOOL valid=win32_user_range_writable((BYTE *)argument+offset,bytes-offset,compat);
        for (unsigned i=0;i<3;i++) {
            if ((!addresses[i])!=(!lengths[i])) valid=FALSE;
            if (lengths[i] && !win32_user_range_writable((void *)(uintptr_t)addresses[i],lengths[i],compat))
                valid=FALSE;
        }
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!valid) status=STATUS_INVALID_PARAMETER;
        else if (compat) {
            narrow.context=context; narrow.commands=(DWORD)buffers.commands;
            narrow.command_bytes=buffers.command_bytes; narrow.allocations=(DWORD)buffers.allocations;
            narrow.allocation_count=buffers.allocation_count; narrow.patches=(DWORD)buffers.patches;
            narrow.patch_count=buffers.patch_count; narrow.gpu_commands=buffers.gpu_commands;
            memcpy((BYTE *)argument+offset,(BYTE *)&narrow+offset,bytes-offset);
        } else {
            captured.context=context; captured.command_buffer=(void *)(uintptr_t)buffers.commands;
            captured.command_bytes=buffers.command_bytes; captured.allocation_list=(void *)(uintptr_t)buffers.allocations;
            captured.allocation_count=buffers.allocation_count; captured.patch_list=(void *)(uintptr_t)buffers.patches;
            captured.patch_count=buffers.patch_count; captured.gpu_commands=buffers.gpu_commands;
            memcpy((BYTE *)argument+offset,(BYTE *)&captured+offset,bytes-offset);
        }
        kmt_copy_unlock(flags);
        if (status!=STATUS_SUCCESS) (void)d3dkmt_destroy_context(pid,context);
    }
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

NTSTATUS WINAPI D3DKMTDestroyContext(PCVOID argument)
{
    OKMT_DESTROY_CONTEXT captured;
    if (!kmt_user_capture(&captured,argument,sizeof(captured),g_compat32_mode)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=d3dkmt_destroy_context(GetCurrentProcessId(),captured.context);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

static BOOL kmt_sync_outputs_writable(PVOID argument,BOOL compat,BOOL extended,BOOL monitored,SIZE_T offset)
{
    return win32_user_range_writable((BYTE *)argument+offset,4,compat) &&
        (!extended || win32_user_range_writable((BYTE *)argument+__builtin_offsetof(OKMT_CREATE_SYNC2,info.shared),4,compat)) &&
        (!monitored || (win32_user_range_writable((BYTE *)argument+
            __builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.cpu_address),compat ? 4 : 8,compat) &&
            win32_user_range_writable((BYTE *)argument+__builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.gpu_address),8,compat)));
}
static NTSTATUS kmt_create_sync(PVOID argument,BOOL extended)
{
    BOOL compat=g_compat32_mode;
    OKMT_CREATE_SYNC old; OKMT_CREATE_SYNC2 modern;
    DWORD device; SIZE_T offset;
    D3DKMT_SYNC_DESCRIPTION description={0};
    if (extended) {
        if (!kmt_user_capture(&modern,argument,sizeof(modern),compat)) return STATUS_INVALID_PARAMETER;
        if (modern.info.type==OKMT_SYNC_MONITORED) {
            /* Local device object; NT sharing requires a separate handle API.
             * Do not accept global/cross-adapter or top-of-pipeline flags. */
            if (modern.info.flags || modern.info.data.monitored.affinity>1)
                return STATUS_NOT_SUPPORTED;
        } else if (modern.info.flags) return STATUS_NOT_SUPPORTED;
        device=modern.device; description.type=modern.info.type;
        description.initial=(description.type==OKMT_SYNC_FENCE || description.type==OKMT_SYNC_MONITORED) ? modern.info.data.value :
            description.type==OKMT_SYNC_SEMAPHORE ? modern.info.data.semaphore.initial : modern.info.data.initial;
        description.maximum=modern.info.data.semaphore.maximum;
        offset=__builtin_offsetof(OKMT_CREATE_SYNC2,object);
    } else {
        if (!kmt_user_capture(&old,argument,sizeof(old),compat)) return STATUS_INVALID_PARAMETER;
        device=old.device; description.type=old.info.type;
        if (description.type!=OKMT_SYNC_MUTEX && description.type!=OKMT_SYNC_SEMAPHORE) return STATUS_INVALID_PARAMETER;
        description.initial=description.type==OKMT_SYNC_SEMAPHORE ? old.info.data.semaphore.initial : old.info.data.initial;
        description.maximum=old.info.data.semaphore.maximum;
        offset=__builtin_offsetof(OKMT_CREATE_SYNC,object);
    }
    BOOL monitored=extended && description.type==OKMT_SYNC_MONITORED;
    if (description.type>=4 && description.type<=7 && !monitored) return STATUS_NOT_SUPPORTED;
    if (!kmt_sync_outputs_writable(argument,compat,extended,monitored,offset))
        return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    DWORD pid=GetCurrentProcessId(); OKMT_HANDLE object=0;
    uint64_t cpu_address=0,gpu_address=0;
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        monitored ? d3dkmt_create_monitored_sync(pid,device,description.initial,compat,&object,&cpu_address,&gpu_address) :
        d3dkmt_create_sync(pid,device,&description,&object);
    if (status==STATUS_SUCCESS) {
        uint64_t flags=kmt_copy_lock();
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!kmt_sync_outputs_writable(argument,compat,extended,monitored,offset))
            status=STATUS_INVALID_PARAMETER;
        else {
            memcpy((BYTE *)argument+offset,&object,4);
            if (extended) { DWORD zero=0; memcpy((BYTE *)argument+__builtin_offsetof(OKMT_CREATE_SYNC2,info.shared),&zero,4); }
            if (monitored) {
                memcpy((BYTE *)argument+__builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.cpu_address),&cpu_address,compat ? 4 : 8);
                memcpy((BYTE *)argument+__builtin_offsetof(OKMT_CREATE_SYNC2,info.data.monitored.gpu_address),&gpu_address,8);
            }
        }
        kmt_copy_unlock(flags);
        if (status!=STATUS_SUCCESS) (void)d3dkmt_destroy_sync(pid,object);
    }
    kmt_scope_leave(); win32_main_termination_checkpoint(); return status;
}
NTSTATUS WINAPI D3DKMTCreateSynchronizationObject(PVOID argument) { return kmt_create_sync(argument,FALSE); }
NTSTATUS WINAPI D3DKMTCreateSynchronizationObject2(PVOID argument) { return kmt_create_sync(argument,TRUE); }
typedef struct { DWORD device;LONG priority;DWORD queue,sync,cpu_address,physical_index; } KMT_PAGING32;
_Static_assert(sizeof(KMT_PAGING32)==24,"PE32 paging queue ABI");
_Static_assert(sizeof(OKMT_CREATE_PAGING_QUEUE)==32,"PE64 paging queue ABI");
static BOOL kmt_paging_output_writable(void *argument,BOOL compat)
{
    return win32_user_range_writable((BYTE *)argument+8,8,compat) &&
        win32_user_range_writable((BYTE *)argument+16,compat?4:8,compat);
}
static NTSTATUS kmt_create_paging_impl(void *argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_CREATE_PAGING_QUEUE input;
    if (compat) {
        KMT_PAGING32 small;
        if (!kmt_user_capture(&small,argument,sizeof(small),TRUE)) return STATUS_INVALID_PARAMETER;
        input=(OKMT_CREATE_PAGING_QUEUE){small.device,small.priority,small.queue,small.sync,
            (void *)(ULONG_PTR)small.cpu_address,small.physical_index};
    } else if (!kmt_user_capture(&input,argument,sizeof(input),FALSE)) return STATUS_INVALID_PARAMETER;
    if (!input.device || input.priority<-1 || input.priority>1 || input.physical_index ||
        !kmt_paging_output_writable(argument,compat)) return STATUS_INVALID_PARAMETER;
    DWORD pid=GetCurrentProcessId();OKMT_HANDLE queue=0,sync=0;uint64_t cpu=0;
    NTSTATUS status=win32_termination_requested()?STATUS_PROCESS_IS_TERMINATING:
        d3dkmt_create_paging_queue(pid,input.device,input.priority,compat,&queue,&sync,&cpu);
    if (!status) {
        uint64_t flags=kmt_copy_lock();
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!kmt_paging_output_writable(argument,compat)) status=STATUS_INVALID_PARAMETER;
        else {
            memcpy((BYTE *)argument+8,&queue,4);memcpy((BYTE *)argument+12,&sync,4);
            if (compat) { DWORD address=(DWORD)cpu;memcpy((BYTE *)argument+16,&address,4); }
            else { void *address=(void *)(ULONG_PTR)cpu;memcpy((BYTE *)argument+16,&address,8); }
        }
        kmt_copy_unlock(flags);
        if (status) (void)d3dkmt_destroy_paging_queue(pid,queue);
    }
    return status;
}
NTSTATUS WINAPI D3DKMTCreatePagingQueue(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_create_paging_impl(argument);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTDestroyPagingQueue(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    OKMT_DESTROY_PAGING_QUEUE input;
    NTSTATUS status=kmt_user_capture(&input,argument,sizeof(input),g_compat32_mode) && input.queue?
        d3dkmt_destroy_paging_queue(GetCurrentProcessId(),input.queue):STATUS_INVALID_PARAMETER;
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

NTSTATUS WINAPI D3DKMTDestroySynchronizationObject(PCVOID argument)
{
    OKMT_DESTROY_SYNC captured;
    if (!kmt_user_capture(&captured,argument,sizeof(captured),g_compat32_mode)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=d3dkmt_destroy_sync(GetCurrentProcessId(),captured.object);
    kmt_scope_leave(); win32_main_termination_checkpoint(); return status;
}
static NTSTATUS kmt_event_retain(DWORD pid,uint64_t handle,PVOID *object)
{ return ntsync_reference_event_for_process((HANDLE)(ULONG_PTR)handle,pid,object); }
static const D3DKMT_SYNC_EVENT_PROVIDER kmt_events={
    kmt_event_retain,ntsync_signal_event_reference,ntsync_event_release
};
static NTSTATUS kmt_sync_queue(PCVOID argument,BOOL signal,BOOL extended)
{
    union { OKMT_WAIT_SYNC wait; OKMT_WAIT_SYNC2 wait2; OKMT_SIGNAL_SYNC signal; OKMT_SIGNAL_SYNC2 signal2; } captured;
    SIZE_T bytes=signal ? (extended ? sizeof(captured.signal2) : sizeof(captured.signal)) :
        (extended ? sizeof(captured.wait2) : sizeof(captured.wait));
    if (!kmt_user_capture(&captured,argument,bytes,g_compat32_mode)) return STATUS_INVALID_PARAMETER;
    if (captured.wait.count>OKMT_MAX_SYNC_OBJECTS) return STATUS_INVALID_PARAMETER;
    D3DKMT_SYNC_COMMAND command={.count=captured.wait.count,.signal=signal,.extended=extended};
    memcpy(command.objects,captured.wait.objects,command.count*sizeof(OKMT_HANDLE));
    if (signal) {
        DWORD flags=captured.signal.flags;
        if (flags&~(extended ? 7u : 1u)) return STATUS_INVALID_PARAMETER;
        if (extended && captured.signal2.broadcast_count>64) return STATUS_INVALID_PARAMETER;
        /* Submission-time signals and broadcasts have distinct
         * ordering/lifetime contracts. Do not silently approximate them. */
        if ((flags&5u) || (extended && captured.signal2.broadcast_count)) return STATUS_NOT_SUPPORTED;
        if (flags&2u) {
            if (command.count) return STATUS_INVALID_PARAMETER;
            command.event_handle=g_compat32_mode ? (DWORD)captured.signal2.values[0] : captured.signal2.values[0];
            if (!command.event_handle) return STATUS_INVALID_PARAMETER;
            command.events=&kmt_events;
        }
        if (extended) command.value=captured.signal2.values[0];
    } else if (extended) command.value=captured.wait2.values[0];
    if (!command.count && !command.event_handle) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    D3DKMT_CONTEXT_LEASE lease={0};
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_acquire_context(GetCurrentProcessId(),captured.wait.context,&lease);
    if (status==STATUS_SUCCESS) status=lease.render.synchronize ? lease.render.synchronize(&lease,&command) : STATUS_NOT_SUPPORTED;
    if (lease.self) (void)d3dkmt_release_context(&lease);
    kmt_scope_leave(); win32_main_termination_checkpoint(); return status;
}
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObject(PCVOID argument) { return kmt_sync_queue(argument,FALSE,FALSE); }
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObject2(PCVOID argument) { return kmt_sync_queue(argument,FALSE,TRUE); }
NTSTATUS WINAPI D3DKMTSignalSynchronizationObject(PCVOID argument) { return kmt_sync_queue(argument,TRUE,FALSE); }
NTSTATUS WINAPI D3DKMTSignalSynchronizationObject2(PCVOID argument) { return kmt_sync_queue(argument,TRUE,TRUE); }

NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromCpu(PCVOID argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_SIGNAL_SYNC_CPU input={0};
    if (compat) {
        struct { DWORD device,count,objects,values,flags; } small;
        if (!kmt_user_capture(&small,argument,sizeof(small),TRUE)) return STATUS_INVALID_PARAMETER;
        input=(OKMT_SIGNAL_SYNC_CPU){small.device,small.count,(const OKMT_HANDLE *)(uintptr_t)small.objects,
            (const OKMT_U64 *)(uintptr_t)small.values,small.flags};
    } else if (!kmt_user_capture(&input,argument,sizeof(input),FALSE)) return STATUS_INVALID_PARAMETER;
    if (!input.count || input.count>D3DKMT_MAX_SYNC_OBJECTS || (input.flags&~7u)) return STATUS_INVALID_PARAMETER;
    if (input.flags&3u) return STATUS_NOT_SUPPORTED;
    OKMT_HANDLE handles[D3DKMT_MAX_SYNC_OBJECTS];
    uint64_t values[D3DKMT_MAX_SYNC_OBJECTS];
    if (!kmt_user_capture(handles,input.objects,input.count*sizeof(*handles),compat) ||
        !kmt_user_capture(values,input.values,input.count*sizeof(*values),compat)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_signal_sync_cpu(GetCurrentProcessId(),input.device,input.count,handles,values,(input.flags&4u)!=0);
    kmt_scope_leave(); win32_main_termination_checkpoint(); return status;
}
static NTSTATUS kmt_sync_from_gpu(PCVOID argument,BOOL signal)
{
    BOOL compat=g_compat32_mode;
    /* Both ABIs have the aligned union at byte 16. PE32 pointer padding is
     * not part of either address, and must never be interpreted as one. */
    OKMT_SYNC_GPU input;
    if (!kmt_user_capture(&input,argument,sizeof(input),compat) ||
        !input.count || input.count>D3DKMT_MAX_SYNC_OBJECTS) return STATUS_INVALID_PARAMETER;
    const OKMT_HANDLE *handles=compat ? (const OKMT_HANDLE *)(uintptr_t)(DWORD)(uintptr_t)input.objects : input.objects;
    D3DKMT_SYNC_COMMAND command={.count=input.count,.extended=TRUE,.signal=signal};
    if (!kmt_user_capture(command.objects,handles,input.count*sizeof(OKMT_HANDLE),compat)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    D3DKMT_CONTEXT_LEASE lease={0};
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_acquire_context(GetCurrentProcessId(),input.context,&lease);
    BOOL monitored=FALSE;
    if (!status) status=d3dkmt_sync_uses_monitored(&lease,command.count,command.objects,&monitored);
    if (!status && signal && !monitored) status=STATUS_INVALID_PARAMETER;
    if (!status && monitored) {
        const OKMT_U64 *values=compat ? (const OKMT_U64 *)(uintptr_t)(DWORD)input.data.value : input.data.values;
        command.vector_values=TRUE;
        if (!kmt_user_capture(command.values,values,input.count*sizeof(uint64_t),compat)) status=STATUS_INVALID_PARAMETER;
    } else command.value=input.data.value;
    if (!status && win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    if (!status) status=lease.render.synchronize ? lease.render.synchronize(&lease,&command) : STATUS_NOT_SUPPORTED;
    if (lease.self) (void)d3dkmt_release_context(&lease);
    kmt_scope_leave(); win32_main_termination_checkpoint(); return status;
}
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromGpu(PCVOID argument) { return kmt_sync_from_gpu(argument,FALSE); }
NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromGpu(PCVOID argument) { return kmt_sync_from_gpu(argument,TRUE); }
NTSTATUS WINAPI D3DKMTSignalSynchronizationObjectFromGpu2(PCVOID argument)
{
    BOOL compat=g_compat32_mode;OKMT_SIGNAL_SYNC_GPU2 input={0};
    if (compat) {
        struct { DWORD count,objects,flags,context_count,contexts; OKMT_U64 data[8]; } small;
        if (!kmt_user_capture(&small,argument,sizeof(small),TRUE)) return STATUS_INVALID_PARAMETER;
        input.count=small.count;input.objects=(const OKMT_HANDLE *)(uintptr_t)small.objects;
        input.flags=small.flags;input.context_count=small.context_count;
        input.contexts=(const OKMT_HANDLE *)(uintptr_t)small.contexts;
        memcpy(input.data.reserved,small.data,sizeof(small.data));
    } else if (!kmt_user_capture(&input,argument,sizeof(input),FALSE)) return STATUS_INVALID_PARAMETER;
    if (!input.context_count || input.context_count>OKMT_MAX_BROADCAST_CONTEXTS ||
        input.count>D3DKMT_MAX_SYNC_OBJECTS || (input.flags&~7u)) return STATUS_INVALID_PARAMETER;
    for (unsigned i=1;i<8;i++) if (input.data.reserved[i]) return STATUS_INVALID_PARAMETER;
    if (input.flags&1u) return STATUS_NOT_SUPPORTED; /* submission-time signals need a distinct hardware boundary */
    BOOL event=(input.flags&2u)!=0;
    if (event ? (input.count || (input.flags&4u)) : !input.count) return STATUS_INVALID_PARAMETER;
    D3DKMT_SYNC_COMMAND command={.count=input.count,.signal=TRUE,.extended=TRUE,
        .allow_rewind=(input.flags&4u)!=0,.broadcast_count=input.context_count};
    if (!kmt_user_capture(command.broadcast,input.contexts,input.context_count*sizeof(OKMT_HANDLE),compat) ||
        (!event && !kmt_user_capture(command.objects,input.objects,input.count*sizeof(OKMT_HANDLE),compat)))
        return STATUS_INVALID_PARAMETER;
    if (event) {
        command.event_handle=compat ? (DWORD)input.data.value : input.data.value;
        if (!command.event_handle) return STATUS_INVALID_PARAMETER;
        command.events=&kmt_events;
    }
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    D3DKMT_CONTEXT_LEASE lease={0};BOOL monitored=FALSE;
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_acquire_context(GetCurrentProcessId(),command.broadcast[0],&lease);
    if (!status && !event) status=d3dkmt_sync_uses_monitored(&lease,command.count,command.objects,&monitored);
    if (!status && monitored) {
        const OKMT_U64 *values=compat ? (const OKMT_U64 *)(uintptr_t)(DWORD)input.data.value : input.data.values;
        command.vector_values=TRUE;
        if (!kmt_user_capture(command.values,values,input.count*sizeof(OKMT_U64),compat)) status=STATUS_INVALID_PARAMETER;
    } else command.value=input.data.value;
    if (!status && win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    if (!status) status=lease.render.synchronize ? lease.render.synchronize(&lease,&command) : STATUS_NOT_SUPPORTED;
    if (lease.self) (void)d3dkmt_release_context(&lease);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTWaitForSynchronizationObjectFromCpu(PCVOID argument)
{
    BOOL compat=g_compat32_mode;OKMT_WAIT_SYNC_CPU input={0};
    if (compat) {
        struct { DWORD device,count,objects,values,event,flags; } small;
        if (!kmt_user_capture(&small,argument,sizeof(small),TRUE)) return STATUS_INVALID_PARAMETER;
        input=(OKMT_WAIT_SYNC_CPU){small.device,small.count,(const OKMT_HANDLE *)(uintptr_t)small.objects,
            (const OKMT_U64 *)(uintptr_t)small.values,(PVOID)(uintptr_t)small.event,small.flags};
    } else if (!kmt_user_capture(&input,argument,sizeof(input),FALSE)) return STATUS_INVALID_PARAMETER;
    if (!input.count || input.count>D3DKMT_MAX_SYNC_OBJECTS || (input.flags&~1u)) return STATUS_INVALID_PARAMETER;
    OKMT_HANDLE objects[D3DKMT_MAX_SYNC_OBJECTS];uint64_t values[D3DKMT_MAX_SYNC_OBJECTS];
    if (!kmt_user_capture(objects,input.objects,input.count*sizeof(*objects),compat) ||
        !kmt_user_capture(values,input.values,input.count*sizeof(*values),compat)) return STATUS_INVALID_PARAMETER;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_wait_cpu(GetCurrentProcessId(),input.device,input.count,objects,values,(input.flags&1u)!=0,
            (uintptr_t)input.event,&kmt_events);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

typedef struct {
    DWORD context,command_offset,command_length,allocation_count,patch_count;
    DWORD commands,command_bytes,allocations,allocation_capacity,patches,patch_capacity,flags;
    uint64_t present_token;
    DWORD broadcast_count,broadcast[64],queued;
    uint64_t gpu_commands;
    DWORD private_data,private_bytes;
} KMT_RENDER32;
_Static_assert(sizeof(KMT_RENDER32)==336,"PE32 render header");

/* Context buffers are fixed for this provider generation. Resize requests
 * can be declined by returning the existing sizes. Never write input flags,
 * handles, broadcast data or user-provided output pointers back as authority. */
static BOOL kmt_render_publish(void *argument,BOOL compat,const D3DKMT_CONTEXT_BUFFERS *b,
    DWORD queued,BOOL publish)
{
    uint64_t flags=kmt_copy_lock();
    SIZE_T bytes=compat ? sizeof(KMT_RENDER32) : sizeof(OKMT_RENDER);
    BOOL valid=win32_user_range_writable(argument,bytes,compat);
    uint64_t addresses[3]={b->commands,b->allocations,b->patches};
    uint64_t lengths[3]={b->command_bytes,(uint64_t)b->allocation_count*8,(uint64_t)b->patch_count*24};
    for (unsigned i=0;i<3;i++) {
        if ((!addresses[i])!=(!lengths[i])) valid=FALSE;
        if (lengths[i] && !win32_user_range_writable((void *)(uintptr_t)addresses[i],lengths[i],compat)) valid=FALSE;
    }
    if (valid && publish) {
        if (compat) {
            DWORD fields[6]={(DWORD)b->commands,b->command_bytes,(DWORD)b->allocations,b->allocation_count,
                (DWORD)b->patches,b->patch_count};
            memcpy((BYTE *)argument+20,fields,sizeof(fields));
            memcpy((BYTE *)argument+316,&queued,4);
            memcpy((BYTE *)argument+320,&b->gpu_commands,8);
        } else {
            /* Separate fields avoid copying padding and any input member. */
#define RENDER_OUT(member,value) memcpy((BYTE *)argument+__builtin_offsetof(OKMT_RENDER,member),&(value),sizeof(value))
            RENDER_OUT(commands,b->commands); RENDER_OUT(command_bytes,b->command_bytes);
            RENDER_OUT(allocations,b->allocations); RENDER_OUT(allocation_capacity,b->allocation_count);
            RENDER_OUT(patches,b->patches); RENDER_OUT(patch_capacity,b->patch_count);
            RENDER_OUT(queued,queued); RENDER_OUT(gpu_commands,b->gpu_commands);
#undef RENDER_OUT
        }
    }
    kmt_copy_unlock(flags); return valid;
}

NTSTATUS WINAPI D3DKMTRender(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    KMT_RENDER32 narrow={0}; OKMT_RENDER captured={0};
    SIZE_T bytes=compat ? sizeof(narrow) : sizeof(captured);
    if (!kmt_user_capture(compat ? (void *)&narrow : (void *)&captured,argument,bytes,compat) ||
        !win32_user_range_writable(argument,bytes,compat)) return STATUS_INVALID_PARAMETER;
    if (compat) {
        captured.context=narrow.context; captured.command_offset=narrow.command_offset;
        captured.command_length=narrow.command_length; captured.allocation_count=narrow.allocation_count;
        captured.patch_count=narrow.patch_count; captured.flags=narrow.flags;
        captured.present_token=narrow.present_token; captured.broadcast_count=narrow.broadcast_count;
        captured.private_data=(void *)(uintptr_t)narrow.private_data; captured.private_bytes=narrow.private_bytes;
    }
    if ((captured.flags&~127u) || captured.broadcast_count>64 || captured.private_data || captured.private_bytes)
        return STATUS_INVALID_PARAMETER;
    if ((captured.flags&~7u) || captured.present_token || captured.broadcast_count) return STATUS_NOT_SUPPORTED;
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    D3DKMT_CONTEXT_LEASE lease={0}; DWORD queued=0;
    NTSTATUS status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
        d3dkmt_acquire_context(GetCurrentProcessId(),captured.context,&lease);
    if (status==0) {
        D3DKMT_CONTEXT_BUFFERS *b=&lease.buffers;
        D3DKMT_RENDER_DESCRIPTION command={captured.command_offset,captured.command_length,
            captured.allocation_count,captured.patch_count};
        if (!kmt_render_publish(argument,compat,b,0,FALSE)) status=STATUS_INVALID_PARAMETER;
        else if (!command.bytes || command.offset>b->command_bytes || command.bytes>b->command_bytes-command.offset ||
            command.allocations>b->allocation_count || command.patches>b->patch_count) status=STATUS_INVALID_PARAMETER;
        else if (!lease.render.submit) status=STATUS_NOT_SUPPORTED;
        else status=lease.render.submit(&lease,&command,&queued);
        /* The provider owns accepted work even if publication fails or the
         * caller exits now. Never free/undo an accepted DMA job on this stack. */
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!kmt_render_publish(argument,compat,b,queued,TRUE)) status=STATUS_INVALID_PARAMETER;
        (void)d3dkmt_release_context(&lease);
    }
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

typedef struct {
    DWORD device,resource,global_share,runtime_data,runtime_bytes,private_data,
        private_bytes,count,allocations,flags,runtime_resource;
} KMT_ALLOCATION32;
typedef struct { DWORD allocation,system_memory,private_data,private_bytes,source,flags; } KMT_INFO32;
typedef struct { KMT_INFO32 base; uint64_t gpu_address; DWORD priority,reserved[5]; } KMT_INFO2_32;
typedef struct { DWORD device,resource,allocations,count,flags; } KMT_DESTROY_ALLOCATION32;
_Static_assert(sizeof(KMT_ALLOCATION32)==44,"PE32 allocation header");
_Static_assert(sizeof(KMT_INFO32)==24 && sizeof(KMT_INFO2_32)==56,"PE32 allocation list");

static BOOL kmt_allocation_publish(void *argument,void *list,SIZE_T stride,BOOL modern,BOOL compat,
    OKMT_HANDLE resource,DWORD count,const OKMT_HANDLE *handles)
{
    uint64_t flags=kmt_copy_lock();
    BOOL valid=win32_user_range_writable((BYTE *)argument+4,8,compat) &&
        (!count || win32_user_range_writable(list,stride*count,compat));
    if (valid) {
        DWORD zero=0; uint64_t gpu_address=0;
        memcpy((BYTE *)argument+4,&resource,4);
        memcpy((BYTE *)argument+8,&zero,4); /* no global share handle */
        for (DWORD i=0;i<count;i++) {
            BYTE *destination=(BYTE *)list+i*stride;
            memcpy(destination,&handles[i],4);
            if (modern) memcpy(destination+(compat ? 24 : 40),&gpu_address,8);
        }
    }
    kmt_copy_unlock(flags); return valid;
}

static NTSTATUS kmt_create_allocation_impl(void *argument,BOOL modern)
{
    BOOL compat=g_compat32_mode;
    SIZE_T header_bytes=compat ? sizeof(KMT_ALLOCATION32) : sizeof(OKMT_CREATE_ALLOCATION);
    OKMT_CREATE_ALLOCATION header;
    if (compat) {
        KMT_ALLOCATION32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_CREATE_ALLOCATION){narrow.device,narrow.resource,narrow.global_share,
            (void *)(ULONG_PTR)narrow.runtime_data,narrow.runtime_bytes,
            (void *)(ULONG_PTR)narrow.private_data,narrow.private_bytes,narrow.count,
            (void *)(ULONG_PTR)narrow.allocations,narrow.flags,(void *)(ULONG_PTR)narrow.runtime_resource};
    } else if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
    BOOL create_resource=(header.flags&1u)!=0;
    if (!header.device || (!header.count && !create_resource) || (create_resource && header.resource) ||
        header.count>D3DKMT_MAX_ALLOCATION_BATCH ||
        (header.flags&~0x7fffffu)) return STATUS_INVALID_PARAMETER;
    /* Owned allocations and device-local groups. Sharing, imported memory,
     * per-resource private metadata and special policies need their contracts. */
    if ((header.flags&~1u) || header.runtime_bytes || header.private_bytes)
        return STATUS_NOT_SUPPORTED;
    SIZE_T stride=modern ? (compat ? sizeof(KMT_INFO2_32) : sizeof(OKMT_ALLOCATION_INFO2)) :
        (compat ? sizeof(KMT_INFO32) : sizeof(OKMT_ALLOCATION_INFO));
    SIZE_T list_bytes=stride*header.count;
    ULONG_PTR list=(ULONG_PTR)header.allocations,base=(ULONG_PTR)argument;
    if (list>(ULONG_PTR)-1-list_bytes || base>(ULONG_PTR)-1-header_bytes ||
        (header.count && base<list+list_bytes && list<base+header_bytes) ||
        !win32_user_range_writable((BYTE *)argument+4,8,compat) ||
        (header.count && !win32_user_range_writable(header.allocations,list_bytes,compat))) return STATUS_INVALID_PARAMETER;
    D3DKMT_PRIVATE_ALLOCATION *captured=header.count ? kmt_allocate(header.count*sizeof(*captured)) : NULL;
    if (header.count && !captured) return STATUS_NO_MEMORY;
    NTSTATUS status=STATUS_SUCCESS;
    for (DWORD i=0;i<header.count;i++) {
        DWORD flags,source,bytes; const void *system_memory,*data; ULONG_PTR reserved=0;
        const void *item=(const BYTE *)header.allocations+i*stride;
        if (compat) {
            KMT_INFO2_32 info={0};
            if (!kmt_user_capture(&info,item,stride,compat)) { status=STATUS_INVALID_PARAMETER; break; }
            system_memory=(void *)(ULONG_PTR)info.base.system_memory;
            data=(void *)(ULONG_PTR)info.base.private_data; bytes=info.base.private_bytes;
            flags=info.base.flags; source=info.base.source;
            if (modern) { reserved=info.priority; for (unsigned j=0;j<5;j++) reserved|=info.reserved[j]; }
        } else {
            /* INFO and INFO2 share the same input field prefix, not their
             * tail padding. Capture into bytes to avoid aliasing assumptions. */
            OKMT_ALLOCATION_INFO2 info={0};
            if (!kmt_user_capture(&info,item,stride,compat)) { status=STATUS_INVALID_PARAMETER; break; }
            system_memory=info.system_memory; data=info.private_data; bytes=info.private_bytes;
            flags=info.flags; source=info.source;
            if (modern) { reserved=info.priority; for (unsigned j=0;j<5;j++) reserved|=info.reserved[j]; }
        }
        if ((flags&~(modern ? 7u : 3u)) || reserved || !bytes || bytes>D3DKMT_MAX_ALLOCATION_PRIVATE_BYTES) {
            status=STATUS_INVALID_PARAMETER; break;
        }
        if (system_memory || flags || source) { status=STATUS_NOT_SUPPORTED; break; }
        captured[i].bytes=bytes;
        if (!kmt_user_capture(captured[i].data,data,bytes,compat)) { status=STATUS_INVALID_PARAMETER; break; }
    }
    DWORD pid=GetCurrentProcessId(); OKMT_HANDLE handles[D3DKMT_MAX_ALLOCATION_BATCH],resource=0;
    if (status==STATUS_SUCCESS && win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    if (status==STATUS_SUCCESS) {
        status=d3dkmt_create_private_resource_allocations(pid,header.device,header.resource,create_resource,
            header.count,captured,&resource,handles);
        if (status==STATUS_SUCCESS) {
            if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
            else if (!kmt_allocation_publish(argument,header.allocations,stride,modern,compat,resource,header.count,handles))
                status=STATUS_INVALID_PARAMETER;
            if (status!=STATUS_SUCCESS) {
                if (create_resource) (void)d3dkmt_destroy_resource(pid,header.device,resource);
                else (void)d3dkmt_destroy_allocations(pid,header.device,header.count,handles);
            }
        }
    }
    if (captured) kmt_free(captured);
    return status;
}

static NTSTATUS kmt_create_allocation(void *argument,BOOL modern)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_create_allocation_impl(argument,modern);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}
NTSTATUS WINAPI D3DKMTCreateAllocation(PVOID argument)
{ return kmt_create_allocation(argument,FALSE); }
NTSTATUS WINAPI D3DKMTCreateAllocation2(PVOID argument)
{ return kmt_create_allocation(argument,TRUE); }

static NTSTATUS kmt_destroy_allocation_impl(const void *argument,BOOL modern)
{
    BOOL compat=g_compat32_mode;
    OKMT_DESTROY_ALLOCATION2 header={0};
    if (compat) {
        KMT_DESTROY_ALLOCATION32 narrow={0};
        if (!kmt_user_capture(&narrow,argument,modern ? 20 : 16,compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_DESTROY_ALLOCATION2){narrow.device,narrow.resource,
            (void *)(ULONG_PTR)narrow.allocations,narrow.count,narrow.flags};
    } else {
        if (!kmt_user_capture(&header,argument,modern ? sizeof(header) : sizeof(OKMT_DESTROY_ALLOCATION),compat))
            return STATUS_INVALID_PARAMETER;
        if (!modern) header.flags=0; /* legacy tail padding is not a flag */
    }
    if (!header.device || (header.flags&~3u)) return STATUS_INVALID_PARAMETER;
    if (header.flags) return STATUS_NOT_SUPPORTED;
    if (header.resource) {
        if (header.allocations) return STATUS_INVALID_PARAMETER;
        return d3dkmt_destroy_resource(GetCurrentProcessId(),header.device,header.resource);
    }
    if (!header.count || header.count>D3DKMT_MAX_ALLOCATION_BATCH) return STATUS_INVALID_PARAMETER;
    OKMT_HANDLE handles[D3DKMT_MAX_ALLOCATION_BATCH];
    if (!kmt_user_capture(handles,header.allocations,header.count*sizeof(*handles),compat)) return STATUS_INVALID_PARAMETER;
    return d3dkmt_destroy_allocations(GetCurrentProcessId(),header.device,header.count,handles);
}
static NTSTATUS kmt_destroy_allocation(const void *argument,BOOL modern)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_destroy_allocation_impl(argument,modern);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}
NTSTATUS WINAPI D3DKMTDestroyAllocation(PCVOID argument)
{ return kmt_destroy_allocation(argument,FALSE); }
NTSTATUS WINAPI D3DKMTDestroyAllocation2(PCVOID argument)
{ return kmt_destroy_allocation(argument,TRUE); }

typedef struct {
    DWORD device,allocation,private_data,page_count,pages,data,flags,padding;
    uint64_t gpu_address;
} KMT_LOCK32;
typedef struct { DWORD device,count,allocations; } KMT_UNLOCK32;
_Static_assert(sizeof(KMT_LOCK32)==40 && __builtin_offsetof(KMT_LOCK32,gpu_address)==32,
    "PE32 lock ABI");

/* Common publication/rollback for both public lock ABIs. The CPU service
 * owns the mapping independently of this stack and of the caller's process. */
static sched_signal_t kmt_lock_delay;
static NTSTATUS kmt_lock_publish(OKMT_HANDLE device,OKMT_HANDLE allocation,
    BOOL compat,BOOL writable,BOOL do_not_wait,BYTE *output)
{
    SIZE_T width=compat ? sizeof(DWORD) : sizeof(void *);
    if (!win32_user_range_writable(output,width,compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    DWORD pid=GetCurrentProcessId();
    uint64_t ticket=0,address=0;
    NTSTATUS status=d3dkmt_cpu_acquire(pid,device,allocation,compat,writable,&ticket,&address);
    while (status==D3DKMT_STATUS_DEVICE_BUSY && !do_not_wait) {
        /* A failed acquire retains neither a CPU mapping nor an allocation
         * lease. Sleep on stable kernel storage; no subsystem lock, user
         * pointer access or preemption hold spans the scheduler wait. The
         * KMT cancellation scope keeps this caller alive until it can unwind.
         * Recheck the complete handle identity after each bounded timer wait:
         * GPU retirement, CPU unlock and device/process close may race it. */
        if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
        if (!win32_user_range_writable(output,width,compat)) return STATUS_INVALID_PARAMETER;
        if (sched_signal_wait(&kmt_lock_delay,1)==SCHED_SIGNAL_INVALID) return STATUS_NOT_SUPPORTED;
        if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
        status=d3dkmt_cpu_acquire(pid,device,allocation,compat,writable,&ticket,&address);
    }
    if (status==D3DKMT_STATUS_DEVICE_BUSY) return (NTSTATUS)0xc01e0102; /* GRAPHICS_ALLOCATION_BUSY */
    if (status!=STATUS_SUCCESS) return status;
    uint64_t irq=kmt_copy_lock();
    if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    else if ((compat && address>=0x80000000ULL) || !win32_user_range_writable(output,width,compat))
        status=STATUS_INVALID_PARAMETER;
    else if (compat) { DWORD narrow=(DWORD)address; memcpy(output,&narrow,sizeof(narrow)); }
    else { void *data=(void *)(ULONG_PTR)address; memcpy(output,&data,sizeof(data)); }
    kmt_copy_unlock(irq);
    if (status!=STATUS_SUCCESS) (void)d3dkmt_cpu_release(pid,ticket);
    return status;
}

static NTSTATUS kmt_lock_impl(void *argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_LOCK header;
    if (compat) {
        KMT_LOCK32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_LOCK){narrow.device,narrow.allocation,narrow.private_data,narrow.page_count,
            (void *)(ULONG_PTR)narrow.pages,(void *)(ULONG_PTR)narrow.data,narrow.flags,narrow.gpu_address};
    } else if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
    DWORD f=header.flags;
    if (!header.device || !header.allocation || header.gpu_address || (f&~0x7ffu) ||
        (f&3u)==3u || ((f&OKMT_LOCK_IGNORE_SYNC) && (f&OKMT_LOCK_ACQUIRE_APERTURE)) ||
        ((f&OKMT_LOCK_ALTERNATE_VA) && !(f&OKMT_LOCK_ACQUIRE_APERTURE)) ||
        ((f&OKMT_LOCK_NO_REFERENCE) && !(f&OKMT_LOCK_DISCARD))) return STATUS_INVALID_PARAMETER;
    if ((f&OKMT_LOCK_ENTIRE) && (header.page_count || header.pages)) return STATUS_INVALID_PARAMETER;
    /* Complete linear WB allocations, with blocking or immediate acquisition.
     * Subranges, renaming and swizzling require different ownership contracts. */
    DWORD supported=OKMT_LOCK_ENTIRE|OKMT_LOCK_DO_NOT_WAIT|OKMT_LOCK_READ_ONLY|
        OKMT_LOCK_WRITE_ONLY|OKMT_LOCK_DO_NOT_EVICT;
    if ((f&~supported) || !(f&OKMT_LOCK_ENTIRE) || header.private_data) return STATUS_NOT_SUPPORTED;
    SIZE_T offset=compat ? __builtin_offsetof(KMT_LOCK32,data) : __builtin_offsetof(OKMT_LOCK,data);
    return kmt_lock_publish(header.device,header.allocation,compat,!(f&OKMT_LOCK_READ_ONLY),
                            (f&OKMT_LOCK_DO_NOT_WAIT)!=0,
                            (BYTE *)argument+offset);
}

NTSTATUS WINAPI D3DKMTLock(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_lock_impl(argument);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

static NTSTATUS kmt_unlock_impl(const void *argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_UNLOCK header;
    if (compat) {
        KMT_UNLOCK32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_UNLOCK){narrow.device,narrow.count,(void *)(ULONG_PTR)narrow.allocations};
    } else if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
    if (!header.device || !header.count || header.count>D3DKMT_MAX_ALLOCATION_BATCH)
        return STATUS_INVALID_PARAMETER;
    OKMT_HANDLE allocations[D3DKMT_MAX_ALLOCATION_BATCH];
    if (!kmt_user_capture(allocations,header.allocations,header.count*sizeof(*allocations),compat))
        return STATUS_INVALID_PARAMETER;
    return d3dkmt_cpu_unlock(GetCurrentProcessId(),header.device,header.count,allocations);
}

NTSTATUS WINAPI D3DKMTUnlock(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_unlock_impl(argument);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

/* Lock2 has a single full-allocation read/write view and no defined flags.
 * It retains the existing immediate BUSY, cache handoff and exit lifetime
 * contract; no WDDM GPU-VA capabilities are implied by exporting this ABI. */
typedef struct { DWORD device,allocation,flags,data; } KMT_LOCK2_32;
_Static_assert(sizeof(KMT_LOCK2_32)==16 && __builtin_offsetof(KMT_LOCK2_32,data)==12,
    "PE32 Lock2 ABI");
_Static_assert(sizeof(OKMT_LOCK2)==24 && __builtin_offsetof(OKMT_LOCK2,data)==16,
    "PE64 Lock2 ABI");
_Static_assert(sizeof(OKMT_UNLOCK2)==8,"Unlock2 ABI");
static NTSTATUS kmt_lock2_impl(void *argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_LOCK2 header;
    if (compat) {
        KMT_LOCK2_32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_LOCK2){narrow.device,narrow.allocation,narrow.flags,(void *)(ULONG_PTR)narrow.data};
    } else if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
    if (!header.device || !header.allocation || header.flags) return STATUS_INVALID_PARAMETER;
    SIZE_T offset=compat ? __builtin_offsetof(KMT_LOCK2_32,data) : __builtin_offsetof(OKMT_LOCK2,data);
    return kmt_lock_publish(header.device,header.allocation,compat,TRUE,TRUE,(BYTE *)argument+offset);
}
NTSTATUS WINAPI D3DKMTLock2(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_lock2_impl(argument);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}
NTSTATUS WINAPI D3DKMTUnlock2(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    OKMT_UNLOCK2 header;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (kmt_user_capture(&header,argument,sizeof(header),g_compat32_mode) && header.device && header.allocation)
        status=d3dkmt_cpu_unlock(GetCurrentProcessId(),header.device,1,&header.allocation);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

typedef struct { DWORD device,resource,allocations,count,values; } KMT_PROPERTIES32;
_Static_assert(sizeof(KMT_PROPERTIES32)==20,"PE32 allocation property ABI");

static NTSTATUS kmt_allocation_properties_impl(PCVOID argument,DWORD operation)
{
    BOOL compat=g_compat32_mode;
    OKMT_ALLOCATION_PROPERTIES header;
    if (compat) {
        KMT_PROPERTIES32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        header=(OKMT_ALLOCATION_PROPERTIES){narrow.device,narrow.resource,
            (void *)(ULONG_PTR)narrow.allocations,narrow.count,(void *)(ULONG_PTR)narrow.values};
    } else if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
    if (!header.device || !header.values ||
        (header.resource && (header.count || header.allocations)) ||
        (!header.resource && (!header.count || header.count>D3DKMT_MAX_HANDLES || !header.allocations)))
        return STATUS_INVALID_PARAMETER;
    DWORD results=header.resource ? 1 : header.count;
    SIZE_T bytes=results*sizeof(DWORD);
    if (operation==D3DKMT_PROPERTY_RESIDENCY && !win32_user_range_writable(header.values,bytes,compat))
        return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    D3DKMT_ALLOCATION_PROPERTIES_WORK *work=kmt_allocate(sizeof(*work));
    if (!work) return STATUS_NO_MEMORY;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    memset(work,0,sizeof(*work));
    if (!header.resource && !kmt_user_capture(work->handles,header.allocations,
        header.count*sizeof(*header.allocations),compat)) goto done;
    if (operation==D3DKMT_PROPERTY_SET_PRIORITY &&
        !kmt_user_capture(work->values,header.values,bytes,compat)) goto done;
    status=d3dkmt_allocation_properties(GetCurrentProcessId(),header.device,header.resource,
        header.count,operation,work);
    if (status==STATUS_SUCCESS && operation==D3DKMT_PROPERTY_RESIDENCY) {
        uint64_t flags=kmt_copy_lock();
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!win32_user_range_writable(header.values,bytes,compat)) status=STATUS_INVALID_PARAMETER;
        else memcpy(header.values,work->values,bytes);
        kmt_copy_unlock(flags);
    }
done:
    kmt_free(work);return status;
}

NTSTATUS WINAPI D3DKMTQueryAllocationResidency(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_allocation_properties_impl(argument,D3DKMT_PROPERTY_RESIDENCY);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

NTSTATUS WINAPI D3DKMTSetAllocationPriority(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_allocation_properties_impl(argument,D3DKMT_PROPERTY_SET_PRIORITY);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

/* The execution prefix is pointer-free and identical in PE32/PE64. */
static NTSTATUS kmt_get_device_state_impl(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    struct { DWORD device,type; } header;
    if (!kmt_user_capture(&header,argument,sizeof(header),compat) || !header.device ||
        !header.type || header.type>6) return STATUS_INVALID_PARAMETER;
    if (header.type!=OKMT_DEVICE_STATE_EXECUTION) return STATUS_NOT_SUPPORTED;
    BYTE *output=(BYTE *)argument+__builtin_offsetof(OKMT_DEVICE_EXECUTION,state);
    if (!win32_user_range_writable(output,sizeof(DWORD),compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    DWORD state=0;
    NTSTATUS status=d3dkmt_query_device_execution(GetCurrentProcessId(),header.device,&state);
    if (status==STATUS_SUCCESS) {
        uint64_t flags=kmt_copy_lock();
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!win32_user_range_writable(output,sizeof(state),compat)) status=STATUS_INVALID_PARAMETER;
        else memcpy(output,&state,sizeof(state));
        kmt_copy_unlock(flags);
    }
    return status;
}
NTSTATUS WINAPI D3DKMTGetDeviceState(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_get_device_state_impl(argument);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

NTSTATUS WINAPI D3DKMTMarkDeviceAsError(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    OKMT_MARK_DEVICE_ERROR header;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (kmt_user_capture(&header,argument,sizeof(header),g_compat32_mode))
        status=win32_termination_requested() ? STATUS_PROCESS_IS_TERMINATING :
            d3dkmt_mark_device_error(GetCurrentProcessId(),header.device,header.reason);
    kmt_scope_leave(); win32_main_termination_checkpoint();
    return status;
}

static NTSTATUS kmt_context_priority_impl(PVOID argument,BOOL set)
{
    BOOL compat=g_compat32_mode;OKMT_CONTEXT_PRIORITY header={0};
    if (!kmt_user_capture(&header,argument,set?sizeof(header):sizeof(header.context),compat))
        return STATUS_INVALID_PARAMETER;
    BYTE *output=(BYTE *)argument+__builtin_offsetof(OKMT_CONTEXT_PRIORITY,priority);
    if (!set && !win32_user_range_writable(output,sizeof(LONG),compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    NTSTATUS status=d3dkmt_context_priority(GetCurrentProcessId(),header.context,set,&header.priority);
    if (!set && !status) {
        uint64_t flags=kmt_copy_lock();
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!win32_user_range_writable(output,sizeof(header.priority),compat)) status=STATUS_INVALID_PARAMETER;
        else memcpy(output,&header.priority,sizeof(header.priority));
        kmt_copy_unlock(flags);
    }
    return status;
}
NTSTATUS WINAPI D3DKMTGetContextSchedulingPriority(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_context_priority_impl(argument,FALSE);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTSetContextSchedulingPriority(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_context_priority_impl((PVOID)argument,TRUE);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

/* Immediate residency commits are a narrowly bounded metadata provider call.
 * Unlike allocation/VM callbacks, this interface permits a BSP copy section:
 * validate outputs, commit counts, publish outputs without a user unmap/exit
 * interleaving. Prepare and retirement always execute outside that section. */
typedef struct {
    DWORD queue,count,allocations,priorities,flags;
    uint64_t fence __attribute__((aligned(8)));
    uint64_t trim;
} KMT_RESIDENT32;
typedef struct {
    DWORD device,count,allocations,flags;
    uint64_t trim __attribute__((aligned(8)));
} KMT_EVICT32;
_Static_assert(sizeof(KMT_RESIDENT32)==40 && __builtin_offsetof(KMT_RESIDENT32,fence)==24,"PE32 residency ABI");
_Static_assert(sizeof(OKMT_MAKE_RESIDENT)==48 && __builtin_offsetof(OKMT_MAKE_RESIDENT,fence)==32,"PE64 residency ABI");
_Static_assert(sizeof(KMT_EVICT32)==24 && __builtin_offsetof(KMT_EVICT32,trim)==16,"PE32 eviction ABI");
_Static_assert(sizeof(OKMT_EVICT)==32 && __builtin_offsetof(OKMT_EVICT,trim)==24,"PE64 eviction ABI");
static BOOL kmt_residency_outputs_writable(void *argument,BOOL compat,BOOL make)
{
    if (make) return win32_user_range_writable((BYTE *)argument+4,4,compat) &&
        win32_user_range_writable((BYTE *)argument+(compat?24:32),16,compat);
    /* The Windows PE32 thunk preserves NumBytesToTrim on successful Evict;
     * it still captures the full input header. PE64 publishes this output. */
    return compat || win32_user_range_writable((BYTE *)argument+24,8,compat);
}
static NTSTATUS kmt_residency_impl(void *argument,BOOL make)
{
    BOOL compat=g_compat32_mode;
    OKMT_HANDLE target=0;DWORD count=0,options=0;const OKMT_HANDLE *allocations=NULL;
    if (make) {
        if (compat) {
            KMT_RESIDENT32 header;
            if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
            target=header.queue;count=header.count;options=header.flags;allocations=(void *)(ULONG_PTR)header.allocations;
        } else {
            OKMT_MAKE_RESIDENT header;
            if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
            target=header.queue;count=header.count;options=header.flags;allocations=header.allocations;
        }
    } else {
        if (compat) {
            KMT_EVICT32 header;
            if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
            target=header.device;count=header.count;options=header.flags;allocations=(void *)(ULONG_PTR)header.allocations;
        } else {
            OKMT_EVICT header;
            if (!kmt_user_capture(&header,argument,sizeof(header),compat)) return STATUS_INVALID_PARAMETER;
            target=header.device;count=header.count;options=header.flags;allocations=header.allocations;
        }
    }
    if (!target || !count || count>D3DKMT_MAX_HANDLES || !allocations || (options&~3u) ||
        !kmt_residency_outputs_writable(argument,compat,make)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    D3DKMT_RESIDENCY_WORK *work=kmt_allocate(sizeof(*work));
    if (!work) return STATUS_NO_MEMORY;
    memset(work,0,sizeof(*work));NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (!kmt_user_capture(work->handles,allocations,count*sizeof(*allocations),compat)) goto done;
    status=d3dkmt_residency_prepare(GetCurrentProcessId(),target,make,count,work);
    if (status) goto done;
    uint64_t irq=kmt_copy_lock();
    if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    else if (!kmt_residency_outputs_writable(argument,compat,make)) status=STATUS_INVALID_PARAMETER;
    else {
        status=d3dkmt_residency_commit(work);
        if (!status) {
            uint64_t zero=0;
            if (make) {
                memcpy((BYTE *)argument+4,&count,4);
                memcpy((BYTE *)argument+(compat?24:32),&zero,8);
                memcpy((BYTE *)argument+(compat?32:40),&zero,8);
            } else if (!compat) memcpy((BYTE *)argument+24,&zero,8);
        }
    }
    kmt_copy_unlock(irq);
    /* No residency paging is needed for this provider's already charged RAM;
     * flags affect no additional budget/trim or asynchronous operation. */
    (void)d3dkmt_residency_release(work);
done:
    kmt_free(work);return status;
}
NTSTATUS WINAPI D3DKMTMakeResident(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_residency_impl(argument,TRUE);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTEvict(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_residency_impl(argument,FALSE);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

typedef struct { DWORD device,resources,allocations,count,priority,flags; } KMT_OFFER32;
typedef struct { DWORD device,resources,allocations,discarded,count; } KMT_RECLAIM32;
typedef struct {
    DWORD queue,count,resources,allocations,discarded;
    uint64_t fence __attribute__((aligned(8)));
} KMT_RECLAIM2_32;
_Static_assert(sizeof(KMT_OFFER32)==24 && sizeof(OKMT_OFFER_ALLOCATIONS)==40,"offer ABI");
_Static_assert(sizeof(KMT_RECLAIM32)==20 && sizeof(OKMT_RECLAIM_ALLOCATIONS)==40,"reclaim ABI");
_Static_assert(sizeof(KMT_RECLAIM2_32)==32 && __builtin_offsetof(KMT_RECLAIM2_32,fence)==24,"reclaim2 PE32 ABI");
_Static_assert(sizeof(OKMT_RECLAIM_ALLOCATIONS2)==40 && __builtin_offsetof(OKMT_RECLAIM_ALLOCATIONS2,fence)==32,"reclaim2 PE64 ABI");
static BOOL kmt_offer_outputs(DWORD *values,DWORD count,void *fence,BOOL compat)
{
    if (values && !win32_user_range_writable(values,count*sizeof(*values),compat)) return FALSE;
    if (fence && !win32_user_range_writable(fence,sizeof(uint64_t),compat)) return FALSE;
    /* A single overlapping output cannot simultaneously represent both values. */
    if (values && fence && (uintptr_t)values<(uintptr_t)fence+sizeof(uint64_t) &&
        (uintptr_t)fence<(uintptr_t)values+count*sizeof(*values)) return FALSE;
    return TRUE;
}
static NTSTATUS kmt_offer_impl(PCVOID argument,DWORD mode)
{
    BOOL compat=g_compat32_mode;
    OKMT_HANDLE target=0;DWORD count=0,priority=0,options=0,*values=NULL;
    const OKMT_HANDLE *resources=NULL,*allocations=NULL;void *fence=NULL;
    if (mode==D3DKMT_OFFER) {
        if (compat) {
            KMT_OFFER32 h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.device;count=h.count;priority=h.priority;options=h.flags;
            resources=(void *)(ULONG_PTR)h.resources;allocations=(void *)(ULONG_PTR)h.allocations;
        } else {
            OKMT_OFFER_ALLOCATIONS h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.device;count=h.count;priority=h.priority;options=h.flags;
            resources=h.resources;allocations=h.allocations;
        }
    } else if (mode==D3DKMT_RECLAIM) {
        if (compat) {
            KMT_RECLAIM32 h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.device;count=h.count;resources=(void *)(ULONG_PTR)h.resources;
            allocations=(void *)(ULONG_PTR)h.allocations;values=(void *)(ULONG_PTR)h.discarded;
        } else {
            OKMT_RECLAIM_ALLOCATIONS h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.device;count=h.count;resources=h.resources;allocations=h.allocations;values=h.discarded;
        }
    } else {
        if (compat) {
            KMT_RECLAIM2_32 h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.queue;count=h.count;resources=(void *)(ULONG_PTR)h.resources;
            allocations=(void *)(ULONG_PTR)h.allocations;values=(void *)(ULONG_PTR)h.discarded;
        } else {
            OKMT_RECLAIM_ALLOCATIONS2 h;
            if (!kmt_user_capture(&h,argument,sizeof(h),compat)) return STATUS_INVALID_PARAMETER;
            target=h.queue;count=h.count;resources=h.resources;allocations=h.allocations;values=h.discarded;
        }
        fence=(BYTE *)argument+(compat?24:32);
    }
    if (!target || !count || count>D3DKMT_MAX_HANDLES || (!resources==!allocations) ||
        (mode==D3DKMT_OFFER && (priority<1 || priority>4 || (options&~3u))) ||
        !kmt_offer_outputs(values,count,fence,compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    D3DKMT_OFFER_WORK *work=kmt_allocate(sizeof(*work));
    if (!work) return STATUS_NO_MEMORY;
    memset(work,0,sizeof(*work));NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (!kmt_user_capture(work->inputs,resources?resources:allocations,count*sizeof(OKMT_HANDLE),compat)) goto done;
    status=d3dkmt_offer_prepare(GetCurrentProcessId(),target,resources!=NULL,count,mode,priority,options,work);
    if (status) goto done;
    uint64_t irq=kmt_copy_lock();
    if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    else if (!kmt_offer_outputs(values,count,fence,compat)) status=STATUS_INVALID_PARAMETER;
    else {
        status=d3dkmt_offer_commit(work);
        if (!status) {
            if (values) memcpy(values,work->results,count*sizeof(*values));
            /* Native preparation synchronously restores all backing. No paging
             * operation remains; zero is already satisfied on this queue. */
            if (fence) {uint64_t ready=0;memcpy(fence,&ready,sizeof(ready));}
        }
    }
    kmt_copy_unlock(irq);
    (void)d3dkmt_offer_release(work);
done:
    kmt_free(work);return status;
}
NTSTATUS WINAPI D3DKMTOfferAllocations(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_offer_impl(argument,D3DKMT_OFFER);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTReclaimAllocations(PCVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_offer_impl(argument,D3DKMT_RECLAIM);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
NTSTATUS WINAPI D3DKMTReclaimAllocations2(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_offer_impl(argument,D3DKMT_RECLAIM2);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

typedef struct { DWORD adapter,source,modes,count; } KMT_DISPLAY_LIST32;
_Static_assert(sizeof(OKMT_DISPLAY_MODE)==44,"display-mode entry ABI");
_Static_assert(sizeof(KMT_DISPLAY_LIST32)==16,"PE32 mode-list ABI");
_Static_assert(sizeof(OKMT_GET_DISPLAY_MODE_LIST)==24 &&
    __builtin_offsetof(OKMT_GET_DISPLAY_MODE_LIST,count)==16,"PE64 mode-list ABI");

static NTSTATUS kmt_display_modes_impl(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    OKMT_GET_DISPLAY_MODE_LIST captured;
    if (compat) {
        KMT_DISPLAY_LIST32 narrow;
        if (!kmt_user_capture(&narrow,argument,sizeof(narrow),compat)) return STATUS_INVALID_PARAMETER;
        captured=(OKMT_GET_DISPLAY_MODE_LIST){narrow.adapter,narrow.source,
            (void *)(ULONG_PTR)narrow.modes,narrow.count};
    } else if (!kmt_user_capture(&captured,argument,sizeof(captured),compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    D3DKMT_DISPLAY_SNAPSHOT snapshot={0};
    NTSTATUS status=d3dkmt_acquire_display_modes(GetCurrentProcessId(),captured.adapter,captured.source,&snapshot);
    if (status) return status;
    /* Registry lock is gone. The snapshot survives adapter removal, rebinding
     * and producer replacement. Exclude BSP unmap/exit only for copyout; no
     * allocator, provider callback or MMIO runs with interrupts excluded. */
    uint64_t flags=kmt_copy_lock();
    BYTE *count_output=(BYTE *)argument+(compat ? 12 : 16);
    if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
    else if (!win32_user_range_writable(count_output,4,compat)) status=STATUS_INVALID_PARAMETER;
    else {
        SIZE_T bytes=(SIZE_T)snapshot.count*sizeof(*snapshot.modes);
        if (!captured.modes) status=STATUS_SUCCESS;
        else if (captured.count<snapshot.count) status=D3DKMT_STATUS_BUFFER_TOO_SMALL;
        else if (!win32_user_range_writable(captured.modes,bytes,compat)) status=STATUS_INVALID_PARAMETER;
        else memcpy(captured.modes,snapshot.modes,bytes);
        if (!status || status==D3DKMT_STATUS_BUFFER_TOO_SMALL)
            memcpy(count_output,&snapshot.count,4);
    }
    kmt_copy_unlock(flags);
    (void)d3dkmt_release_display_modes(&snapshot);
    return status;
}

NTSTATUS WINAPI D3DKMTGetDisplayModeList(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_display_modes_impl(argument);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}

static NTSTATUS kmt_scanline_impl(PVOID argument)
{
    BOOL compat=g_compat32_mode;
    struct { DWORD adapter,source; } captured;
    _Static_assert(sizeof(OKMT_GET_SCANLINE)==16,"scanline PE32/PE64 ABI");
    if (!kmt_user_capture(&captured,argument,sizeof(captured),compat)) return STATUS_INVALID_PARAMETER;
    if (win32_termination_requested()) return STATUS_PROCESS_IS_TERMINATING;
    D3DKMT_DISPLAY_LEASE lease={0};D3DKMT_SCANLINE_SAMPLE sample;
    NTSTATUS status=d3dkmt_acquire_display(GetCurrentProcessId(),captured.adapter,captured.source,&lease);
    if (status) return status;
    status=d3dkmt_sample_display(&lease,&sample);
    if (!status) {
        uint64_t flags=kmt_copy_lock();
        BYTE *blank_output=(BYTE *)argument+8,*line_output=(BYTE *)argument+12;
        if (win32_termination_requested()) status=STATUS_PROCESS_IS_TERMINATING;
        else if (!win32_user_range_writable(blank_output,1,compat) ||
                 !win32_user_range_writable(line_output,4,compat)) status=STATUS_INVALID_PARAMETER;
        else status=d3dkmt_display_status(&lease);
        /* A bounded snapshot is linearized by the final status check. The
         * retained provider outlives copyout even if removal follows it. */
        if (!status) {
            *blank_output=(BYTE)sample.in_vertical_blank;
            memcpy(line_output,&sample.scanline,4);
        }
        kmt_copy_unlock(flags);
    }
    (void)d3dkmt_release_display(&lease);
    return status;
}

NTSTATUS WINAPI D3DKMTGetScanLine(PVOID argument)
{
    if (!kmt_scope_enter()) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=kmt_scanline_impl(argument);
    kmt_scope_leave();win32_main_termination_checkpoint();return status;
}
