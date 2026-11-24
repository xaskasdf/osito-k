#include "process_shutdown_common.h"
static void *gate,*ready,*peers[3];
static void (*configure)(void *,void *,void *,DWORD);
static DWORD CALL passive_worker(void *argument)
{
    (void)argument;SetEvent(ready);
    for(;;) {
#ifdef SHUTDOWN_BUSY
        __asm__ volatile("pause" ::: "memory");
#else
        Sleep(100);
#endif
    }
}
static DWORD CALL exiting_worker(void *argument)
{
    (void)argument;WaitForSingleObject(gate,5000);
    configure(peers[0],peers[1],peers[2],GetCurrentThreadId());
    shutdown_row("exit-worker",1);ExitProcess(37);return 99;
}
void process_shutdown_entry(void)
{
    void *module=LoadLibraryA("process-shutdown.dll");
    if(!module)ExitProcess(101);
    configure=GetProcAddress(module,"shutdown_configure");
    if(!configure)ExitProcess(102);
    ready=CreateEventA(0,1,0,0);gate=CreateEventA(0,1,0,0);
    peers[0]=CreateThread(0,0,passive_worker,0,0,0);
    if(!peers[0]||WaitForSingleObject(ready,5000))ExitProcess(103);
    peers[1]=CreateThread(0,0,passive_worker,0,4,0);
    if(!peers[1])ExitProcess(104);
#if defined(SHUTDOWN_WORKER)
    peers[2]=OpenThread(0x100040,0,GetCurrentThreadId());
    if(!peers[2])ExitProcess(105);
    void *thread=CreateThread(0,0,exiting_worker,0,0,0);
    if(!thread)ExitProcess(106);
    SetEvent(gate);
#ifdef SHUTDOWN_BUSY
    for(;;)__asm__ volatile("pause" ::: "memory");
#else
    WaitForSingleObject(thread,10000);ExitProcess(107);
#endif
#elif defined(SHUTDOWN_FORCED)
    (void)exiting_worker;
    shutdown_row("forced-exit",1);TerminateProcess(GetCurrentProcess(),37);ExitProcess(108);
#else
    (void)exiting_worker;
    configure(peers[0],peers[1],0,GetCurrentThreadId());
    shutdown_row("exit-main",1);ExitProcess(37);
#endif
}
