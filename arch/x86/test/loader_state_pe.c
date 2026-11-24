#include "loader_state_common.h"
static DWORD CALL loader_state_worker(void *argument)
{(void)argument;loader_state_row("worker",9,0);return 17;}
void loader_state_entry(void)
{
    DWORD failures=0,code=0;
    loader_state_row("entry",9,0);
    failures+=RtlIsThreadWithinLoaderCallout()!=0 || RtlDllShutdownInProgress()!=0;
    void *module=LoadLibraryA("loader-state.dll");
    if(!module)ExitProcess(1);
    DWORD (*query)(void)=(void *)GetProcAddress(module,"loader_state_query");
    failures+=!query || query()!=0;
    void *thread=CreateThread(0,0,loader_state_worker,0,0,0);
    failures+=!thread || WaitForSingleObject(thread,5000)!=0 || !GetExitCodeThread(thread,&code) || code!=17;
    if(thread)CloseHandle(thread);
    void *ntdll=GetModuleHandleA("ntdll.dll");
    int (CALL *lock)(DWORD,DWORD *,UP *)=GetProcAddress(ntdll,"LdrLockLoaderLock");
    int (CALL *unlock)(DWORD,UP)=GetProcAddress(ntdll,"LdrUnlockLoaderLock");
    if(lock&&unlock){DWORD disposition=0;UP cookie=0;failures+=lock(0,&disposition,&cookie)!=0;
        loader_state_row("explicit-lock",9,0);failures+=RtlIsThreadWithinLoaderCallout()!=1;
        failures+=unlock(0,cookie)!=0;}
    else failures++;
    failures+=!FreeLibrary(module);
    loader_state_row("after-unload",9,0);
    failures+=RtlIsThreadWithinLoaderCallout()!=0 || RtlDllShutdownInProgress()!=0;
    module=LoadLibraryA("loader-state-fail.dll");
    failures+=module!=0 || GetLastError()!=1114;
    if(module)FreeLibrary(module);
    loader_state_row("after-failure",9,0);
    failures+=RtlIsThreadWithinLoaderCallout()!=0 || RtlDllShutdownInProgress()!=0;
    module=LoadLibraryA("loader-state.dll");failures+=module==0;
    loader_state_row("before-exit",9,0);
#ifdef EXPLICIT_SHUTDOWN
    void (CALL *shutdown)(void)=GetProcAddress(ntdll,"LdrShutdownProcess");
    if(!shutdown)ExitProcess(1);
    shutdown();loader_state_row("after-shutdown",9,0);
    failures+=RtlDllShutdownInProgress()!=1 || RtlIsThreadWithinLoaderCallout()!=0;
    shutdown();loader_state_row("after-second-shutdown",9,0);
#endif
    ExitProcess(failures?1:0);
}
