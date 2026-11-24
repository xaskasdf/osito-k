/* AttachConsole uses process identity and preserves explicit startup handles. */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
#ifdef _WIN64
typedef unsigned long long ULONG_PTR;
#define SELF "conattach64.exe"
#define STARTUP_SIZE 104
#define FLAGS_OFFSET 60
#define HANDLES_OFFSET 80
#else
typedef unsigned int ULONG_PTR;
#define SELF "conattach32.exe"
#define STARTUP_SIZE 68
#define FLAGS_OFFSET 44
#define HANDLES_OFFSET 56
#endif
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
typedef struct {
    HANDLE process, thread;
    DWORD process_id, thread_id;
} PROCESS_INFORMATION;
API void CALL ExitProcess(DWORD);
API HANDLE CALL LoadLibraryA(const char *);
API void *CALL GetProcAddress(HANDLE, const char *);
API DWORD CALL GetLastError(void);
API DWORD CALL GetCurrentProcessId(void);
API char *CALL GetCommandLineA(void);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL SetStdHandle(DWORD, HANDLE);
API BOOL CALL SetHandleInformation(HANDLE, DWORD, DWORD);
API BOOL CALL AllocConsole(void);
API BOOL CALL FreeConsole(void);
API BOOL CALL GetConsoleMode(HANDLE, DWORD *);
API void CALL OutputDebugStringA(const char *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS_INFORMATION *);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API BOOL CALL CloseHandle(HANDLE);
API HANDLE CALL CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API DWORD CALL GetFileSize(HANDLE, DWORD *);
API BOOL CALL DeleteFileA(const char *);
static BOOL (CALL *attach)(DWORD);

static void require(BOOL condition, DWORD step)
{
    if (condition) return;
    char message[] = "CONSOLE-ATTACH FAIL 00\n";
    message[21] = '0' + step % 10;
    message[20] = '0' + step / 10;
    OutputDebugStringA(message);
    ExitProcess(step);
}

static void check_parameters(BOOL attached)
{
    unsigned char *peb;
#ifdef _WIN64
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(peb));
    unsigned char *parameters = *(unsigned char **)(peb + 0x20);
    HANDLE console = *(HANDLE *)(parameters + 0x10);
    HANDLE *standard = (HANDLE *)(parameters + 0x20);
#else
    __asm__ volatile ("movl %%fs:0x30, %0" : "=r"(peb));
    unsigned char *parameters = *(unsigned char **)(peb + 0x10);
    HANDLE console = *(HANDLE *)(parameters + 0x10);
    HANDLE *standard = (HANDLE *)(parameters + 0x18);
#endif
    require((console != 0) == attached, 10);
    for (DWORD i = 0; i < 3; i++)
        require(standard[i] == GetStdHandle((DWORD)-10 - i), 11 + i);
}

static void child(char mode, DWORD parent)
{
    if (mode == 'i') {
        check_parameters(1);
        require(!attach(0) && GetLastError() == 5, 21);
        require(FreeConsole(), 22);
    }
    check_parameters(0);
    require(!attach(0) && GetLastError() == 87, 23);
    require(!attach(0x7ffffffe) && GetLastError() == 87, 24);
    require(!attach(GetCurrentProcessId()) && GetLastError() == 6, 25);
    if (mode == 'n') {
        require(!attach((DWORD)-1) && GetLastError() == 6, 26);
        require(!attach(parent) && GetLastError() == 6, 27);
        check_parameters(0);
        ExitProcess(0);
    }
    HANDLE redirected = GetStdHandle((DWORD)-11);
    if (mode == 's')
        require(SetStdHandle((DWORD)-11, (HANDLE)(ULONG_PTR)0x1234), 28);
    require(attach((DWORD)-1), 29);
    check_parameters(1);
    DWORD console_mode;
    require(GetConsoleMode(GetStdHandle((DWORD)-10), &console_mode), 30);
    require(!attach(parent) && GetLastError() == 5, 31);
    if (mode == 'v' || mode == 'b') {
        DWORD written;
        require(GetStdHandle((DWORD)-11) == redirected, 44);
        require(!WriteFile(redirected, "denied", 6, &written, 0) &&
                GetLastError() == 6, 45);
        ExitProcess(0);
    }
    if (mode == 'r') {
        require(redirected && GetStdHandle((DWORD)-11) == redirected &&
                GetStdHandle((DWORD)-12) == redirected, 32);
        DWORD written;
        require(WriteFile(redirected, "redirect", 8, &written, 0) && written == 8, 33);
    } else {
        require(GetConsoleMode(GetStdHandle((DWORD)-11), &console_mode), 34);
    }
    require(FreeConsole(), 35);
    check_parameters(0);
    require(attach(parent), 36);
    check_parameters(1);
    require(FreeConsole() && AllocConsole(), 37);
    check_parameters(1);
    if (mode == 'r') {
        require(GetStdHandle((DWORD)-11) == redirected, 38);
        DWORD written;
        require(WriteFile(redirected, "redirect", 8, &written, 0) && written == 8, 39);
    }
    ExitProcess(0);
}

static DWORD launch(char mode, DWORD flags, HANDLE redirected)
{
    char command[] = SELF " x 00000000";
    DWORD offset = sizeof(SELF);
    command[offset] = mode;
    DWORD parent = GetCurrentProcessId();
    for (int i = 7; i >= 0; i--) {
        command[offset + 2 + i] = "0123456789abcdef"[parent & 15];
        parent >>= 4;
    }
    unsigned char startup[STARTUP_SIZE] = {0};
    *(DWORD *)startup = sizeof(startup);
    if (redirected) {
        *(DWORD *)(startup + FLAGS_OFFSET) = 0x100; /* STARTF_USESTDHANDLES */
        HANDLE *standard = (HANDLE *)(startup + HANDLES_OFFSET);
        standard[0] = 0;
        standard[1] = standard[2] = redirected;
    }
    PROCESS_INFORMATION process = {0};
    require(CreateProcessA(SELF, command, 0, 0, redirected != 0 && mode != 'b', flags,
                            0, 0, startup, &process), 40);
    require(WaitForSingleObject(process.process, 15000) == 0, 41);
    DWORD status = 0xffffffff;
    require(GetExitCodeProcess(process.process, &status) && status == 0, 42);
    require(CloseHandle(process.thread) && CloseHandle(process.process), 43);
    return process.process_id;
}

void mainCRTStartup(void)
{
    attach = (BOOL (CALL *)(DWORD))GetProcAddress(
        LoadLibraryA("kernel32.dll"), "AttachConsole");
    if (!attach) {
        OutputDebugStringA("CONSOLE-ATTACH MISSING EXPORT\n");
        ExitProcess(90);
    }
    char *args = GetCommandLineA();
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    if (*args) {
        char mode = *args++;
        while (*args == ' ') args++;
        DWORD parent = 0;
        while (*args) {
            DWORD digit = (unsigned char)*args++;
            parent = (parent << 4) | (digit <= '9' ? digit - '0' : digit - 'a' + 10);
        }
        child(mode, parent);
    }
    check_parameters(1);
    require(!attach(0) && GetLastError() == 5, 50);
    launch('i', 0, 0);
    launch('d', 8, 0);
    launch('s', 0x08000000, 0);
    OutputDebugStringA("CONSOLE-ATTACH PARENT-AND-EXPLICIT PASS\n");
    require(FreeConsole(), 51);
    launch('n', 8, 0);
    require(!attach((DWORD)-1) && GetLastError() == 87, 52);
    require(AllocConsole(), 53);
    HANDLE file = CreateFileA("console-redirect.tmp", 0xc0000000, 3, 0, 2, 0, 0);
    require(file != (HANDLE)(ULONG_PTR)-1, 54);
    launch('v', 8, file);
    require(GetFileSize(file, 0) == 0, 61);
    require(SetHandleInformation(file, 1, 1), 62);
    launch('b', 8, file);
    require(GetFileSize(file, 0) == 0, 63);
    DWORD dead = launch('r', 8, file);
    require(GetFileSize(file, 0) == 16, 55);
    require(CloseHandle(file) && DeleteFileA("console-redirect.tmp"), 56);
    check_parameters(1);
    DWORD console_mode;
    require(GetConsoleMode(GetStdHandle((DWORD)-11), &console_mode), 57);
    require(FreeConsole(), 58);
    require(!attach(dead) && GetLastError() == 87, 59);
    require(AllocConsole(), 60);
    check_parameters(1);
    OutputDebugStringA("CONSOLE-ATTACH REDIRECT-AND-LIFETIME PASS\n");
    OutputDebugStringA("CONSOLE-ATTACH PASS\n");
    ExitProcess(0);
}
