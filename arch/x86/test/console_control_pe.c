/* Foreground cancellation and guest NULL-fetch ownership regression. */
typedef unsigned int DWORD;
typedef __UINTPTR_TYPE__ UPTR;
#define CALL __attribute__((stdcall))
#define API __declspec(dllimport)
API void CALL ExitProcess(DWORD);
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(void *, const char *);
#define SAY(text) do { DWORD n; WriteFile(GetStdHandle((DWORD)-11),text,sizeof(text)-1,&n,0); } while (0)
typedef void (CALL *sleep_fn)(DWORD);
typedef void (CALL *thread_exit_fn)(DWORD);
static thread_exit_fn thread_exit;
#if CONTROL_MODE >= 2
static DWORD CALL null_worker(void *unused)
{
    DWORD (CALL *volatile target)(void *)=0;
    return target(unused);
}
#endif
#if CONTROL_MODE == 3
static DWORD CALL resume_worker(void *unused)
{
    (void)unused;
    thread_exit(38);
    return 0;
}
static long CALL null_handler(void *opaque)
{
    void **pointers=opaque;
    unsigned char *record=pointers[0],*context=pointers[1];
    if (*(DWORD *)record!=0xc0000005U) return 0;
#ifdef _WIN64
    if (*(UPTR *)(context+0xf8)!=0) return 0;
    *(UPTR *)(context+0xf8)=(UPTR)resume_worker;
#else
    if (*(DWORD *)(context+184)!=0) return 0;
    *(DWORD *)(context+184)=(DWORD)(UPTR)resume_worker;
#endif
    return -1;
}
#endif
void console_control_entry(void)
{
    void *kernel=LoadLibraryA("kernel32.dll");
    sleep_fn sleep=(sleep_fn)GetProcAddress(kernel,"Sleep");
    thread_exit=(thread_exit_fn)GetProcAddress(kernel,"ExitThread");
#if CONTROL_MODE == 0
    SAY("CONTROL READY sleep\n");
    for (;;) sleep(1000);
#elif CONTROL_MODE == 1
    (void)sleep;
    SAY("CONTROL READY spin\n");
    for (;;) __asm__ volatile ("pause" ::: "memory");
#else
    (void)sleep;
    typedef void *(CALL *thread_fn)(void *,UPTR,DWORD(CALL *)(void *),void *,DWORD,DWORD *);
    typedef DWORD (CALL *wait_fn)(void *,DWORD);
    typedef int (CALL *exit_fn)(void *,DWORD *);
    typedef int (CALL *close_fn)(void *);
    thread_fn create=(thread_fn)GetProcAddress(kernel,"CreateThread");
    wait_fn wait=(wait_fn)GetProcAddress(kernel,"WaitForSingleObject");
    exit_fn code=(exit_fn)GetProcAddress(kernel,"GetExitCodeThread");
    close_fn close=(close_fn)GetProcAddress(kernel,"CloseHandle");
    if (!create || !wait || !code || !close || !thread_exit) ExitProcess(80);
#if CONTROL_MODE == 3
    typedef void *(CALL *veh_fn)(DWORD,long(CALL *)(void *));
    veh_fn add=(veh_fn)GetProcAddress(kernel,"AddVectoredExceptionHandler");
    if (!add || !add(1,null_handler)) ExitProcess(81);
    SAY("CONTROL NULL handled\n");
#else
    SAY("CONTROL NULL unhandled\n");
#endif
    void *thread=create(0,0,null_worker,0,0,0);
    if (!thread) ExitProcess(82);
    DWORD status=0;
    if (wait(thread,10000)!=0 || !code(thread,&status) || status!=38 || !close(thread)) {
        SAY("CONTROL FAIL null-recovery\n");ExitProcess(83);
    }
    SAY("CONTROL PASS null-recovery\n");
    ExitProcess(0);
#endif
}
