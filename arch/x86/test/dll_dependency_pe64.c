/* Reproduce DLL initialization and the data import behind vstdlib's #PF. */
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
API void CALL ExitProcess(DWORD);
API DWORD CALL GetLastError(void);
API DWORD CALL GetCurrentThreadId(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL LoadLibraryExA(const char *, HANDLE, DWORD);
API void *CALL GetProcAddress(HANDLE, const char *);

static void dependency_print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void dependency_hex(DWORD value)
{
    char text[11] = "0x00000000";
    for (DWORD i = 0; i < 8; i++) {
        DWORD digit = (value >> ((7 - i) * 4)) & 15;
        text[2 + i] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    dependency_print(text);
}

void mainCRTStartup(void)
{
    char path[1024];
    DWORD length = GetModuleFileNameA(NIL, path, sizeof(path));
    if (!length || length >= sizeof(path) - 32) {
        dependency_print("DLL-DEPENDENCY FAIL module path\r\n");
        ExitProcess(1);
    }
    while (length && path[length - 1] != '/' && path[length - 1] != '\\') length--;
    const char *names[2] = {"tier0_s64.dll", "vstdlib_s64.dll"};
    for (DWORD module = 0; module < 2; module++) {
        DWORD i = 0;
        do { path[length + i] = names[module][i]; } while (names[module][i++]);
        dependency_print("DLL-DEPENDENCY LOAD "); dependency_print(names[module]);
        dependency_print("\r\n");
        HANDLE loaded = LoadLibraryExA(path, NIL, 0x1100);
        DWORD error = GetLastError();
        if (!loaded) {
            dependency_print("DLL-DEPENDENCY FAIL load "); dependency_print(names[module]);
            dependency_print(" error="); dependency_hex(error); dependency_print("\r\n");
            ExitProcess(2 + module);
        }
        if (!module) {
            DWORD *entry_tid = (DWORD *)GetProcAddress(loaded, "g_dwDllEntryThreadId");
            if (!entry_tid) {
                dependency_print("DLL-DEPENDENCY FAIL data export\r\n");
                ExitProcess(4);
            }
            DWORD saved = *entry_tid;
            DWORD current = GetCurrentThreadId();
            *entry_tid = current;
            if (*entry_tid != current) {
                dependency_print("DLL-DEPENDENCY FAIL data write\r\n");
                ExitProcess(5);
            }
            *entry_tid = saved;
            dependency_print("DLL-DEPENDENCY DATA PASS\r\n");
        } else if (!GetProcAddress(loaded, "KeyValuesSystemSteam")) {
            dependency_print("DLL-DEPENDENCY FAIL dependent export\r\n");
            ExitProcess(6);
        }
        dependency_print("DLL-DEPENDENCY INIT PASS "); dependency_print(names[module]);
        dependency_print("\r\n");
    }
    dependency_print("DLL-DEPENDENCY PASS\r\n");
    ExitProcess(0);
}
