/* Kernel32 delay-load binding for installed PE runtimes. No replacement DLL
 * implementation: LoadLibrary/GetProcAddress supply the actual dependency. */
#include "delay_load.h"
#include "kernel32_shim.h"
#include "ntdll_shim.h"
#include "dllloader.h"
#include "compat32.h"
#ifdef __KERNEL_X86__
#include "../include/types.h"
#else
#include <string.h>
#endif
extern ULONG_PTR win32_current_image_base(void);
typedef struct { DWORD attributes,name,module,iat,names,bound,unload,time; } DELAY_DESCRIPTOR;
typedef struct {
    ULONG size;
    PCVOID descriptor;
    PVOID thunk;
    PCSTR dll;
    struct { ULONG by_name; union { PCSTR name; ULONG ordinal; }; } api;
    PVOID module,unused;
    ULONG error;
} DELAY_INFO64;
_Static_assert(sizeof(DELAY_DESCRIPTOR)==32,"delay descriptor ABI");
_Static_assert(sizeof(DELAY_INFO64)==72,"delay failure callback PE64 ABI");

static BOOL delay_range(ULONG_PTR base,DWORD size,ULONG_PTR address,SIZE_T bytes,BOOL compat)
{
    return address>=base && address-base<=size && bytes<=size-(address-base) &&
        win32_user_range_readable((PCVOID)address,bytes,compat);
}
static BOOL delay_string(ULONG_PTR base,DWORD size,DWORD rva,char *copy,SIZE_T capacity,BOOL compat)
{
    if (!rva || rva>=size) return FALSE;
    for (SIZE_T i=0;i<capacity;i++) {
        if (!delay_range(base,size,base+rva+i,1,compat)) return FALSE;
        copy[i]=*(volatile char *)(base+rva+i);
        if (!copy[i]) return i!=0;
    }
    return FALSE;
}
static ULONG_PTR delay_pointer(PCVOID address,BOOL compat)
{
    if (compat) return __atomic_load_n((const DWORD *)address,__ATOMIC_ACQUIRE);
    return __atomic_load_n((const ULONG_PTR *)address,__ATOMIC_ACQUIRE);
}
static BOOL delay_publish(PVOID slot,ULONG_PTR expected,ULONG_PTR value,BOOL compat,ULONG_PTR *actual,BOOL *installed)
{
    SIZE_T bytes=compat ? 4 : 8; DWORD protection=0;
    BOOL changed=!win32_user_range_writable(slot,bytes,compat);
    if (changed && !VirtualProtect(slot,bytes,PAGE_READWRITE,&protection)) return FALSE;
    if (!win32_user_range_writable(slot,bytes,compat)) {
        if (changed) { DWORD ignored; VirtualProtect(slot,bytes,protection,&ignored); }
        return FALSE;
    }
    if (compat) {
        DWORD prior=(DWORD)expected;
        BOOL replaced=__atomic_compare_exchange_n((DWORD *)slot,&prior,(DWORD)value,FALSE,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        *actual=replaced ? value : prior;
        if (installed) *installed=replaced;
    } else {
        ULONG_PTR prior=expected;
        BOOL replaced=__atomic_compare_exchange_n((ULONG_PTR *)slot,&prior,value,FALSE,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        *actual=replaced ? value : prior;
        if (installed) *installed=replaced;
    }
    if (changed) { DWORD ignored; if (!VirtualProtect(slot,bytes,protection,&ignored)) return FALSE; }
    return TRUE;
}
static PVOID delay_failure(PVOID dll_hook,PVOID system_hook,PCVOID descriptor,PVOID thunk,
    PCSTR dll,PCSTR name,ULONG ordinal,HANDLE module,DWORD error,BOOL compat)
{
    PVOID result=NULL;
    if (dll_hook) {
        if (!win32_user_range_executable(dll_hook,1,compat)) {SetLastError(87);return NULL;}
        /* Both ABIs receive process-owned writable callback data, never a
         * high-half kernel stack address. Process exit owns abandoned data. */
        PVOID memory=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (!memory) return NULL;
        if (compat) {
            DWORD data[9]={36,(DWORD)(ULONG_PTR)descriptor,(DWORD)(ULONG_PTR)thunk,
                (DWORD)(ULONG_PTR)dll,name!=NULL,name ? (DWORD)(ULONG_PTR)name : ordinal,
                (DWORD)(ULONG_PTR)module,0,error};
            memcpy(memory,data,sizeof(data));
            DWORD args[]={4,(DWORD)(ULONG_PTR)memory};
            result=(PVOID)(ULONG_PTR)compat32_callback_args((DWORD)(ULONG_PTR)dll_hook,2,args);
        } else {
            DELAY_INFO64 data={0};data.size=sizeof(data);data.descriptor=descriptor;data.thunk=thunk;
            data.dll=dll;data.api.by_name=name!=NULL;data.api.name=name;
            if (!name) data.api.ordinal=ordinal;
            data.module=module;data.error=error;memcpy(memory,&data,sizeof(data));
            result=((PVOID(WINAPI *)(ULONG,PVOID))dll_hook)(4,memory);
        }
        VirtualFree(memory,0,MEM_RELEASE);
    }
    if (!result && system_hook) {
        if (!win32_user_range_executable(system_hook,1,compat)) {SetLastError(87);return NULL;}
        PCSTR procedure=name ? name : (PCSTR)(ULONG_PTR)ordinal;
        if (compat) {
            DWORD args[]={(DWORD)(ULONG_PTR)dll,(DWORD)(ULONG_PTR)procedure};
            result=(PVOID)(ULONG_PTR)compat32_callback_args((DWORD)(ULONG_PTR)system_hook,2,args);
        } else result=((PVOID(WINAPI *)(PCSTR,PCSTR))system_hook)(dll,procedure);
    }
    return result;
}
PVOID WINAPI ResolveDelayLoadedAPI(PVOID parent,PCVOID descriptor,PVOID dll_hook,
                                   PVOID system_hook,PVOID thunk,ULONG flags)
{
    BOOL compat=g_compat32_mode;ULONG_PTR base=(ULONG_PTR)parent;SIZE_T width=compat ? 4 : 8;
    PVOID retained=NULL,result=NULL;DWORD error=87;
    if (!base || flags || ((ULONG_PTR)thunk&(width-1))) goto done;
    if (base!=win32_current_image_base()) {
        retained=dll_get_module_handle_by_address(parent,TRUE,FALSE);
        if (retained!=parent) goto done;
    }
    /* Header reads and all RVA arithmetic are bounded before dereferencing.
     * Parent DLL references remain held across dependency loads/callbacks. */
    IMAGE_DOS_HEADER dos;
    if (!win32_user_range_readable(parent,sizeof(dos),compat)) goto done;
    memcpy(&dos,parent,sizeof(dos));
    if (dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>4096-88) goto done;
    BYTE header[88];
    if (!win32_user_range_readable((PCVOID)(base+dos.e_lfanew),sizeof(header),compat)) goto done;
    memcpy(header,(PCVOID)(base+dos.e_lfanew),sizeof(header));
    DWORD signature,size;WORD magic;
    memcpy(&signature,header,4);memcpy(&magic,header+24,2);memcpy(&size,header+24+56,4);
    if (signature!=IMAGE_NT_SIGNATURE || magic!=(compat ? IMAGE_NT_OPTIONAL_HDR32_MAGIC : IMAGE_NT_OPTIONAL_HDR64_MAGIC) ||
        !size || base>(ULONG_PTR)-1-size || (compat && base+size>0x100000000ULL) ||
        !delay_range(base,size,(ULONG_PTR)descriptor,sizeof(DELAY_DESCRIPTOR),compat)) goto done;
    DELAY_DESCRIPTOR d;memcpy(&d,descriptor,sizeof(d));
    if (d.attributes!=1 || !d.module || !d.iat || !d.names || (d.module&(width-1)) || (d.iat&(width-1)) ||
        (d.names&(width-1)) || !delay_range(base,size,base+d.module,width,compat) ||
        !delay_range(base,size,base+d.iat,width,compat) || (ULONG_PTR)thunk<base+d.iat ||
        !delay_range(base,size,(ULONG_PTR)thunk,width,compat)) goto done;
    ULONG_PTR offset=(ULONG_PTR)thunk-(base+d.iat);
    if (offset>size || d.names>size-offset || !delay_range(base,size,base+d.names+offset,width,compat)) goto done;
    ULONG_PTR initial=delay_pointer(thunk,compat);
    if (initial && (initial<base || initial-base>=size)) {
        if (win32_user_range_executable((PVOID)initial,1,compat)) {result=(PVOID)initial;error=0;}
        goto done;
    }
    ULONG_PTR lookup=delay_pointer((PCVOID)(base+d.names+offset),compat);
    BOOL ordinal=(lookup&(compat ? 1ULL<<31 : 1ULL<<63))!=0;
    char dll[260],procedure[512];PCSTR guest_name=NULL;
    if (!lookup || !delay_string(base,size,d.name,dll,sizeof(dll),compat)) goto done;
    if (!ordinal) {
        if (lookup>UINT32_MAX-2 || !delay_string(base,size,(DWORD)lookup+2,procedure,sizeof(procedure),compat)) goto done;
        guest_name=(PCSTR)(base+lookup+2);
    }
    PVOID module_slot=(PVOID)(base+d.module);HANDLE module=(HANDLE)delay_pointer(module_slot,compat);
    if (!module) {
        HANDLE loaded=LoadLibraryA(dll);
        if (loaded) {
            ULONG_PTR winner;BOOL installed=FALSE;
            if (!delay_publish(module_slot,0,(ULONG_PTR)loaded,compat,&winner,&installed)) {
                if (!installed) FreeLibrary(loaded);
                error=5;
                goto done;
            }
            module=(HANDLE)winner;
            if (!installed) FreeLibrary(loaded);
        }
    }
    if (module) result=GetProcAddress(module,ordinal ? (PCSTR)(lookup&0xffff) : procedure);
    if (!result) {
        error=module ? (ordinal ? 182 : 127) : GetLastError();
        if (!error) error=126;
        result=delay_failure(dll_hook,system_hook,descriptor,thunk,(PCSTR)(base+d.name),guest_name,
                             (ULONG)(lookup&0xffff),module,error,compat);
        if (result && !win32_user_range_executable(result,1,compat)) {result=NULL;error=87;}
        /* Windows preserves the original name thunk on callback recovery;
         * ordinal recovery updates it, as ordinary export resolution does. */
        if (!result || !ordinal) goto done;
    }
    if (!win32_user_range_executable(result,1,compat)) {result=NULL;error=87;goto done;}
    ULONG_PTR winner;
    if (!delay_publish(thunk,initial,(ULONG_PTR)result,compat,&winner,NULL)) {result=NULL;error=5;goto done;}
    result=(PVOID)winner;error=0;
done:
    if (retained) dll_release_module(retained);
    if (!result) SetLastError(error);
    return result;
}
PVOID WINAPI DelayLoadFailureHook(PCSTR dll,PCSTR procedure)
{
    EXCEPTION_RECORD record={0};record.ExceptionCode=0x505;record.ExceptionFlags=1;
    record.NumberParameters=2;record.ExceptionInformation[0]=(ULONG_PTR)dll;
    record.ExceptionInformation[1]=(ULONG_PTR)procedure;
    RtlRaiseException(&record);
    return NULL;
}
