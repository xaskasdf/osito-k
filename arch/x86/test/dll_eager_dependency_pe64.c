/* Exercise eager function/data imports in both the main PE and a child PE. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
API void CALL ExitProcess(DWORD);
API DWORD CALL GetCurrentThreadId(void);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API BOOL CALL CloseHandle(HANDLE);
API DWORD g_dwDllEntryThreadId;
API void *CALL KeyValuesSystemSteam(void);
static void *CALL (*volatile dependent_function)(void) = KeyValuesSystemSteam;

void mainCRTStartup(void)
{
    DWORD written;
    char *args = GetCommandLineA();
    BOOL child = 0;
    for (DWORD i = 0; args[i]; i++)
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'C') child = 1;
    DWORD *entry_tid = &g_dwDllEntryThreadId;
    if (!entry_tid || !dependent_function) {
        const char fail[] = "DLL-EAGER FAIL eager import\r\n";
        WriteFile(GetStdHandle((DWORD)-11), fail, sizeof(fail) - 1, &written, NIL);
        ExitProcess(1);
    }
    DWORD saved = *entry_tid;
    DWORD current = GetCurrentThreadId();
    *entry_tid = current;
    if (*entry_tid != current) ExitProcess(2);
    *entry_tid = saved;
    if (child) {
        const char pass[] = "DLL-EAGER CHILD PASS\r\n";
        WriteFile(GetStdHandle((DWORD)-11), pass, sizeof(pass) - 1, &written, NIL);
        ExitProcess(0);
    }
    const char main_pass[] = "DLL-EAGER MAIN PASS\r\n";
    WriteFile(GetStdHandle((DWORD)-11), main_pass, sizeof(main_pass) - 1, &written, NIL);
    char path[1024], command[1040];
    DWORD length = GetModuleFileNameA(NIL, path, sizeof(path));
    if (!length || length >= sizeof(path)) ExitProcess(3);
    DWORD p = 0, n = 0;
    command[p++] = '"';
    while (path[n]) command[p++] = path[n++];
    command[p++] = '"'; command[p++] = ' ';
    command[p++] = '-'; command[p++] = '-'; command[p++] = 'C'; command[p] = 0;
    unsigned char startup[104] = {0};
    *(DWORD *)startup = sizeof(startup);
    PROCESS_INFORMATION process;
    if (!CreateProcessA(path, command, NIL, NIL, 0, 0, NIL, NIL, startup, &process)) ExitProcess(4);
    if (WaitForSingleObject(process.process, 10000) != 0) ExitProcess(5);
    DWORD status;
    if (!GetExitCodeProcess(process.process, &status) || status != 0) ExitProcess(6);
    CloseHandle(process.thread); CloseHandle(process.process);
    const char pass[] = "DLL-EAGER PASS\r\n";
    WriteFile(GetStdHandle((DWORD)-11), pass, sizeof(pass) - 1, &written, NIL);
    ExitProcess(0);
}
