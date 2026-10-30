/* Compare the entire TLS/DllMain sequence with Windows, not just counts. */
#include "loader_state_common.h"
API void CALL ExitThread(DWORD);
DWORD _tls_index;
static const DWORD tls_template=7;
static void CALL loader_multi_tls(void *module,DWORD reason,void *reserved)
{(void)module;loader_state_row("exe-tls",reason,reserved);}
static const UP callbacks[]={(UP)loader_multi_tls,0};
const struct {UP start,end,index,callbacks;DWORD zero,characteristics;} _tls_used=
    {(UP)&tls_template,(UP)(&tls_template+1),(UP)&_tls_index,(UP)callbacks,0,0};
#if __SIZEOF_POINTER__ == 4
#pragma comment(linker,"/include:__tls_used")
#else
#pragma comment(linker,"/include:_tls_used")
#endif
static DWORD CALL loader_multi_worker(void *explicit_exit)
{
    loader_state_row(explicit_exit?"exit-worker":"return-worker",9,0);
    if(explicit_exit)ExitThread(17);
    return 17;
}
void loader_multi_entry(void)
{
    DWORD failures=0;
    loader_state_row("entry",9,0);
    void *first=LoadLibraryA("loader-state-a.dll");
    void *second=LoadLibraryA("loader-state-b.dll");
    void *tls_only=LoadLibraryA("loader-state-tls.dll");
    if(!first||!second||!tls_only)ExitProcess(1);
    for(UP explicit_exit=0;explicit_exit<2;explicit_exit++) {
        void *thread=CreateThread(0,0,loader_multi_worker,(void *)explicit_exit,0,0);
        DWORD code=0;
        failures+=!thread || WaitForSingleObject(thread,5000)!=0 ||
            !GetExitCodeThread(thread,&code) || code!=17;
        if(thread)CloseHandle(thread);
    }
    failures+=!FreeLibrary(tls_only);
    tls_only=LoadLibraryA("loader-state-tls.dll");
    failures+=!tls_only;
    void *ntdll=GetModuleHandleA("ntdll.dll");
    void (CALL *shutdown)(void)=GetProcAddress(ntdll,"LdrShutdownProcess");
    if(!shutdown)ExitProcess(1);
#ifdef SHUTDOWN_LOCKED
    int (CALL *lock)(DWORD,DWORD *,UP *)=GetProcAddress(ntdll,"LdrLockLoaderLock");
    int (CALL *unlock)(DWORD,UP)=GetProcAddress(ntdll,"LdrUnlockLoaderLock");
    DWORD disposition=0;UP cookie=0;
    if(!lock||!unlock||lock(0,&disposition,&cookie))ExitProcess(1);
#endif
    shutdown();loader_state_row("after-shutdown",9,0);
#ifdef SHUTDOWN_LOCKED
    failures+=unlock(0,cookie)!=0;
#endif
    failures+=RtlIsThreadWithinLoaderCallout()!=0 || RtlDllShutdownInProgress()!=1;
    shutdown();loader_state_row("after-second-shutdown",9,0);
    ExitProcess(failures?1:0);
}
