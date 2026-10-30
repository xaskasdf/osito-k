/* Fault diagnostics must not dereference an unmapped saved instruction IP. */
typedef unsigned long DWORD;
#ifdef _WIN64
typedef unsigned long long SIZE_T;
#else
typedef unsigned long SIZE_T;
#endif
#define IMPORT __declspec(dllimport)
#define WINAPI __attribute__((stdcall))
IMPORT void WINAPI ExitProcess(DWORD);
IMPORT void *WINAPI VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
IMPORT int WINAPI VirtualFree(void *, SIZE_T, DWORD);

#ifdef FAULT_READ_LAUNCHER
typedef struct {
    void *process, *thread;
    DWORD process_id, thread_id;
} PROCESS_INFORMATION;
IMPORT char *WINAPI GetCommandLineA(void);
IMPORT int WINAPI CreateProcessA(const char *, char *, void *, void *, int,
                                DWORD, void *, const char *, void *,
                                PROCESS_INFORMATION *);
IMPORT DWORD WINAPI WaitForSingleObject(void *, DWORD);

void mainCRTStartup(void)
{
    char *child = GetCommandLineA();
    while (*child && *child != ' ') child++;
    while (*child == ' ') child++;
    unsigned char startup[104] = {0};
    *(DWORD *)startup = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!*child || !CreateProcessA(child, child, (void *)0, (void *)0, 0, 0,
                                  (void *)0, (void *)0, startup, &process))
        ExitProcess(84);
    if (WaitForSingleObject(process.process, 15000) != 0) ExitProcess(85);
    ExitProcess(0);
}
#else
void mainCRTStartup(void)
{
    unsigned char *code = VirtualAlloc((void *)0, 8192, 0x2000, 0x40);
    if (!code) ExitProcess(81);
#if FAULT_READ_BOUNDARY
    if (VirtualAlloc(code, 4096, 0x1000, 0x40) != code) ExitProcess(82);
    code[4095] = 0xcc;
    ((void (*)(void))(code + 4095))();
#else
    ((void (*)(void))code)();
#endif
    ExitProcess(83);
}
#endif
