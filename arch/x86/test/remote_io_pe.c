/* Large synchronous reads must not monopolize the foreground console task. */
typedef unsigned int DWORD;
typedef void *HANDLE;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
API HANDLE CALL GetStdHandle(DWORD);
API int CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE CALL CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API int CALL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
API DWORD CALL SetFilePointer(HANDLE, long, long *, DWORD);
API int CALL CloseHandle(HANDLE);
API void CALL Sleep(DWORD);
API void CALL ExitProcess(DWORD);

static unsigned char buffer[64 * 1024 * 1024];

static void message(const char *text, DWORD bytes)
{
    DWORD written;
    WriteFile(GetStdHandle((DWORD)-11), text, bytes, &written, 0);
}

static void fail(void)
{
    message("REMOTE-IO FAIL\n", 15);
    ExitProcess(1);
}

void mainCRTStartup(void)
{
    HANDLE file = CreateFileA("io-input.bin", 0x80000000U, 1, 0, 3, 0, 0);
    if (!file || file == (HANDLE)-1) fail();
    message("REMOTE-IO START\n", 16);
    for (unsigned round = 0; round < 4; round++) {
        DWORD count;
        if (SetFilePointer(file, 0, 0, 0) != 0) fail();
        if (!ReadFile(file, buffer, sizeof(buffer), &count, 0) || count != sizeof(buffer))
            fail();
        for (unsigned i = 0; i < sizeof(buffer); i++)
            if (buffer[i] != (unsigned char)i) fail();
    }
    if (!CloseHandle(file)) fail();
    message("REMOTE-IO PASS\n", 15);
    for (;;) Sleep(60000);
}
