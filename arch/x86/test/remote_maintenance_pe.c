/* Keep a foreground PE and owned workers alive until console stop requests exit. */
typedef unsigned int DWORD;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API void *CALL GetStdHandle(DWORD);
API int CALL WriteFile(void *, const void *, DWORD, DWORD *, void *);
API void *CALL CreateThread(void *, DWORD, DWORD (CALL *)(void *), void *, DWORD, DWORD *);
API void CALL Sleep(DWORD);
API void CALL ExitProcess(DWORD);
static volatile unsigned counters[4];
static DWORD CALL worker(void *context)
{
    volatile unsigned *counter = context;
    for (;;) {
        for (unsigned i = 0; i < 10000; i++) (*counter)++;
        Sleep(0);
    }
}
void mainCRTStartup(void)
{
    for (unsigned i = 0; i < 4; i++) {
        if (!CreateThread(0, 0, worker, (void *)&counters[i], 0, 0)) ExitProcess(2);
    }
    static const char message[] = "REMOTE-MAINTENANCE READY\n";
    DWORD written;
    WriteFile(GetStdHandle((DWORD)-11), message, sizeof(message) - 1, &written, 0);
    for (;;) Sleep(60000);
}
