#include "process_shutdown_common.h"
DWORD _tls_index;
static const DWORD tls_template=0;
static DWORD *current_tls(void)
{
    UP *vector;
#if __SIZEOF_POINTER__ == 4
    __asm__ volatile("movl %%fs:0x2c,%0":"=r"(vector));
#else
    __asm__ volatile("movq %%gs:0x58,%0":"=r"(vector));
#endif
    return vector?(DWORD *)vector[_tls_index]:0;
}
static void CALL process_shutdown_tls(void *module,DWORD reason,void *reserved)
{
    (void)module;(void)reserved;
    DWORD *tls=current_tls();
    if(reason==1||reason==2){if(tls)*tls=GetCurrentThreadId();}
    if(reason==0) {
        loader_state_row("shutdown-tls",reason,reserved);
        shutdown_row("tls-identity",tls && *tls==GetCurrentThreadId());
    }
    if(reason==3)shutdown_row("unexpected-thread-detach",1);
}
static const UP callbacks[]={(UP)process_shutdown_tls,0};
const struct {UP start,end,index,callbacks;DWORD zero,characteristics;} _tls_used=
    {(UP)&tls_template,(UP)(&tls_template+1),(UP)&_tls_index,(UP)callbacks,0,0};
#if __SIZEOF_POINTER__ == 4
#pragma comment(linker,"/include:__tls_used")
#else
#pragma comment(linker,"/include:_tls_used")
#endif
static void *peers[3];
static DWORD expected_tid;
__declspec(dllexport) void shutdown_configure(void *a,void *b,void *c,DWORD tid)
{peers[0]=a;peers[1]=b;peers[2]=c;expected_tid=tid;}
static DWORD CALL unexpected_worker(void *argument)
{(void)argument;shutdown_row("unexpected-new-thread",1);return 0;}
int CALL process_shutdown_dll_main(void *module,DWORD reason,void *reserved)
{
    (void)module;
    if(reason==0) {
        loader_state_row("shutdown-main",reason,reserved);
        DWORD *tls=current_tls();
        shutdown_row("caller-identity",expected_tid==GetCurrentThreadId());
        shutdown_row("main-tls-identity",tls && *tls==expected_tid);
        for(unsigned i=0;i<3;i++)if(peers[i]) {
            DWORD code=0;shutdown_row("peer-signaled",WaitForSingleObject(peers[i],1000));
            shutdown_row("peer-exit-known",GetExitCodeThread(peers[i],&code));
            shutdown_row("peer-exit",code);
        }
        SetLastError(0x12345);
        void *thread=CreateThread(0,0,unexpected_worker,0,0,0);
        shutdown_row("new-thread-created",thread!=0);
        shutdown_row("new-thread-error",GetLastError());
        if(thread){shutdown_row("new-thread-wait",WaitForSingleObject(thread,50));CloseHandle(thread);}
    }
    return 1;
}
