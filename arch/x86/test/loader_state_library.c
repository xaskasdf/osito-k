#include "loader_state_common.h"
#ifndef FAIL_ATTACH
#define FAIL_ATTACH 0
#endif
#ifndef STATE_LABEL
#define STATE_LABEL "dll"
#endif
DWORD _tls_index;
static const DWORD tls_template=1;
static int loader_state_tls_valid(void)
{
    UP *vector;
#if __SIZEOF_POINTER__ == 4
    __asm__ volatile("movl %%fs:0x2c,%0":"=r"(vector));
#else
    __asm__ volatile("movq %%gs:0x58,%0":"=r"(vector));
#endif
    return vector && vector[_tls_index] && *(DWORD *)vector[_tls_index]==tls_template;
}
static void CALL loader_state_tls(void *module,DWORD reason,void *reserved)
{
    (void)module;
    loader_state_row(loader_state_tls_valid()?STATE_LABEL "-tls":"bad-tls",reason,reserved);
}
static const UP callbacks[]={(UP)loader_state_tls,0};
const struct {UP start,end,index,callbacks;DWORD zero,characteristics;} _tls_used=
    {(UP)&tls_template,(UP)(&tls_template+1),(UP)&_tls_index,(UP)callbacks,0,0};
#if __SIZEOF_POINTER__ == 4
#pragma comment(linker,"/include:__tls_used")
#else
#pragma comment(linker,"/include:_tls_used")
#endif
__declspec(dllexport) DWORD loader_state_query(void)
{return RtlIsThreadWithinLoaderCallout() | ((DWORD)RtlDllShutdownInProgress()<<8);}
int CALL loader_state_dll_main(void *module,DWORD reason,void *reserved)
{
    (void)module;
    loader_state_row(!loader_state_tls_valid()?"bad-main-tls":
        (FAIL_ATTACH?"failed-dll-main":STATE_LABEL "-main"),reason,reserved);
#ifdef REENTER_SHUTDOWN
    if(reason==0 && reserved) {
        void (CALL *shutdown)(void)=GetProcAddress(GetModuleHandleA("ntdll.dll"),"LdrShutdownProcess");
        if(shutdown)shutdown();
        loader_state_row(STATE_LABEL "-reentered",reason,reserved);
    }
#endif
    return !(FAIL_ATTACH && reason==1);
}
