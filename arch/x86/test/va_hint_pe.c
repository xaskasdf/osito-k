/* A fixed high reservation/view must not exhaust lower free address space. */
typedef unsigned long DWORD;
typedef __SIZE_TYPE__ SIZE_T;
typedef __UINTPTR_TYPE__ UPTR;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
API void CALL ExitProcess(DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void *CALL VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
API BOOL CALL VirtualFree(void *, SIZE_T, DWORD);
API HANDLE CALL CreateFileMappingA(HANDLE, void *, DWORD, DWORD, DWORD, const char *);
API void *CALL MapViewOfFileEx(HANDLE, DWORD, DWORD, DWORD, SIZE_T, void *);
API BOOL CALL UnmapViewOfFile(const void *);
API BOOL CALL CloseHandle(HANDLE);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL LoadLibraryExA(const char *, HANDLE, DWORD);
API void *CALL GetProcAddress(HANDLE, const char *);

static void va_hint_report(const char *text)
{
    DWORD count = 0, written;
    while (text[count]) count++;
    WriteFile(GetStdHandle((DWORD)-11), text, count, &written, NIL);
}

void mainCRTStartup(void)
{
#ifdef _WIN64
    void *high = (void *)(UPTR)0xFFFFFF0000ULL;
    const SIZE_T automatic_size = 0x10000;
#else
    void *high = (void *)(UPTR)0x70000000UL;
    const SIZE_T automatic_size = 0x20000000;
#endif
    void *fixed = VirtualAlloc(high, 0x10000, 0x2000, 1);
#ifndef _WIN64
    /* Native WOW64 DLL ASLR can occupy the first candidate. */
    for (DWORD i = 1; !fixed && i < 0xFFD; i++) {
        high = (void *)(UPTR)(0x70000000UL + i * 0x10000UL);
        fixed = VirtualAlloc(high, 0x10000, 0x2000, 1);
    }
#endif
    if (fixed != high) {
        va_hint_report("VA-HINT FAIL fixed reservation\r\n");
        ExitProcess(1);
    }
    BOOL dll_mode = 0;
    const char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++)
        if (args[i] == '-' && args[i + 1] == '-' && args[i + 2] == 'D') dll_mode = 1;
#ifdef _WIN64
    if (dll_mode) {
        char path[1024];
        DWORD length = GetModuleFileNameA(NIL, path, sizeof(path));
        if (!length || length >= sizeof(path)) ExitProcess(2);
        DWORD prefix = length;
        while (prefix && path[prefix - 1] != '\\' && path[prefix - 1] != '/') prefix--;
        const char tier[] = "tier0_s64.dll";
        for (DWORD i = 0; i < sizeof(tier); i++) path[prefix + i] = tier[i];
        HANDLE module = LoadLibraryExA(path, NIL, 0x1100);
        if (!module) {
            va_hint_report("VA-HINT DLL FAIL tier0 initialization\r\n");
            ExitProcess(3);
        }
        if (!GetProcAddress(module, "g_dwDllEntryThreadId")) ExitProcess(4);
        const char vstdlib[] = "vstdlib_s64.dll";
        for (DWORD i = 0; i < sizeof(vstdlib); i++) path[prefix + i] = vstdlib[i];
        if (!LoadLibraryExA(path, NIL, 0x1100)) {
            va_hint_report("VA-HINT DLL FAIL vstdlib initialization\r\n");
            ExitProcess(5);
        }
        va_hint_report("VA-HINT DLL PASS\r\n");
        VirtualFree(fixed, 0, 0x8000);
        ExitProcess(0);
    }
#else
    (void)dll_mode;
#endif
    void *automatic = VirtualAlloc(NIL, automatic_size, 0x2000, 4);
    if (!automatic) {
        va_hint_report("VA-HINT FAIL automatic reserve after fixed reserve\r\n");
        ExitProcess(6);
    }
    if (VirtualAlloc(automatic, 4096, 0x1000, 4) != automatic) ExitProcess(7);
    *(volatile DWORD *)automatic = 0x12345678;
    if (*(volatile DWORD *)automatic != 0x12345678) ExitProcess(8);
    va_hint_report("VA-HINT RESERVE PASS\r\n");
    if (!VirtualFree(automatic, 0, 0x8000) || !VirtualFree(fixed, 0, 0x8000)) ExitProcess(9);
    HANDLE section = CreateFileMappingA((HANDLE)(UPTR)-1, NIL, 4, 0, 0x10000, NIL);
    if (!section) ExitProcess(10);
    void *view = MapViewOfFileEx(section, 2, 0, 0, 0x10000, high);
    if (view != high) {
        va_hint_report("VA-HINT FAIL fixed section view\r\n");
        ExitProcess(11);
    }
    *(volatile DWORD *)view = 0x87654321;
    automatic = VirtualAlloc(NIL, automatic_size, 0x2000, 4);
    if (!automatic) {
        va_hint_report("VA-HINT FAIL automatic reserve after fixed view\r\n");
        ExitProcess(12);
    }
    if (VirtualAlloc(automatic, 4096, 0x1000, 4) != automatic) ExitProcess(13);
    *(volatile DWORD *)automatic = 0xABCDEF01;
    if (*(volatile DWORD *)automatic != 0xABCDEF01 ||
        *(volatile DWORD *)view != 0x87654321) ExitProcess(14);
    va_hint_report("VA-HINT SECTION PASS\r\n");
    if (!VirtualFree(automatic, 0, 0x8000) || !UnmapViewOfFile(view) || !CloseHandle(section)) ExitProcess(15);
    va_hint_report("VA-HINT PASS\r\n");
    ExitProcess(0);
}
