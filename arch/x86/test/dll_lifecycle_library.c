/* Custom payload only: mimic an installer's named singleton and dynamic TLS. */
#include "dll_lifecycle_common.h"
#ifndef FAIL_ATTACH
#define FAIL_ATTACH 0
#endif
static void *guard;
static DWORD slot=(DWORD)-1;
#ifdef STATIC_TLS
DWORD _tls_index;
static const DWORD tls_template=0x98abcdef;
const struct {
    UP start,end,index,callbacks;
    DWORD zero,characteristics;
} _tls_used={(UP)&tls_template,(UP)(&tls_template+1),(UP)&_tls_index,0,0,0};
#if __SIZEOF_POINTER__ == 4
#pragma comment(linker,"/include:__tls_used")
#else
#pragma comment(linker,"/include:_tls_used")
#endif
#endif
__declspec(dllexport) DWORD lifecycle_value(void)
{
#ifdef STATIC_TLS
    UP *vector;
#if __SIZEOF_POINTER__ == 4
    __asm__ volatile("movl %%fs:0x2c,%0":"=r"(vector));
#else
    __asm__ volatile("movq %%gs:0x58,%0":"=r"(vector));
#endif
    if(!vector||!vector[_tls_index]||*(DWORD *)vector[_tls_index]!=tls_template)return 0;
#endif
    return (DWORD)(UP)TlsGetValue(slot);
}
int CALL lifecycle_dll_main(void *module,DWORD reason,void *reserved)
{
    (void)reserved;
    if(reason==1){
        char name[64];lifecycle_name(name,FAIL_ATTACH);
        guard=CreateMutexA(0,0,name);
        DWORD error=GetLastError();
        if(!guard)return 0;
        if(error==183){CloseHandle(guard);guard=0;return 0;}
        DisableThreadLibraryCalls(module);
        slot=TlsAlloc();
        if(slot==(DWORD)-1||!TlsSetValue(slot,(void *)(UP)0x12345678))return 0;
        if(FAIL_ATTACH){SetLastError(5);return 0;}
        return 1;
    }
    if(reason==0){
        if(guard){CloseHandle(guard);guard=0;}
        if(slot!=(DWORD)-1){TlsFree(slot);slot=(DWORD)-1;}
        /* The loader must ignore this value during DETACH. */
        return 0;
    }
    return 1;
}
