/* Fill the NT process table (parent + 31 children), then exercise slot reuse. */
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef void *HANDLE;
typedef int BOOL;
#define API __declspec(dllimport)
#define CALL __attribute__((stdcall))
#define NIL ((void *)0)
#define CAPACITY 31
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
typedef struct {
    DWORD cb;
    char *reserved, *desktop, *title;
    DWORD x, y, width, height, chars_x, chars_y, fill, flags;
    WORD show, reserved_size;
    void *reserved_bytes;
    HANDLE input, output, error;
} STARTUPINFO;
API void CALL ExitProcess(DWORD);
API DWORD CALL GetLastError(void);
API char *CALL GetCommandLineA(void);
API DWORD CALL GetModuleFileNameA(HANDLE, char *, DWORD);
API HANDLE CALL GetStdHandle(DWORD);
API BOOL CALL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API BOOL CALL CreateProcessA(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, STARTUPINFO *, PROCESS_INFORMATION *);
API BOOL CALL TerminateProcess(HANDLE, DWORD);
API BOOL CALL GetExitCodeProcess(HANDLE, DWORD *);
API DWORD CALL ResumeThread(HANDLE);
API DWORD CALL WaitForSingleObject(HANDLE, DWORD);
API BOOL CALL CloseHandle(HANDLE);
API void CALL Sleep(DWORD);
static PROCESS_INFORMATION children[CAPACITY];
static STARTUPINFO startup;
static char path[512], command[540];

#ifdef _WIN64
_Static_assert(sizeof(STARTUPINFO) == 104, "STARTUPINFOA x64 ABI");
#else
_Static_assert(sizeof(STARTUPINFO) == 68, "STARTUPINFOA x86 ABI");
#endif

static void print(const char *text)
{
    DWORD length = 0, written;
    while (text[length]) length++;
    WriteFile(GetStdHandle((DWORD)-11), text, length, &written, NIL);
}

static void number(DWORD value)
{
    char text[11];
    DWORD pos = sizeof(text) - 1;
    text[pos] = 0;
    do { text[--pos] = '0' + value % 10; value /= 10; } while (value);
    print(text + pos);
}

static BOOL option(const char *text)
{
    const char *args = GetCommandLineA();
    for (DWORD i = 0; args[i]; i++) {
        if (i && args[i - 1] != ' ') continue;
        DWORD n = 0;
        while (text[n] && args[i + n] == text[n]) n++;
        if (!text[n] && (!args[i + n] || args[i + n] == ' ')) return 1;
    }
    return 0;
}

static void require(BOOL ok, const char *stage)
{
    if (ok) return;
    print("CHILD-CAPACITY FAIL "); print(stage);
    print(" error="); number(GetLastError()); print("\r\n");
    for (DWORD i = 0; i < CAPACITY; i++) {
        if (children[i].process) {
            TerminateProcess(children[i].process, 1);
            WaitForSingleObject(children[i].process, 1000);
            CloseHandle(children[i].thread);
            CloseHandle(children[i].process);
        }
    }
    ExitProcess(1);
}

static BOOL spawn(PROCESS_INFORMATION *child)
{
    DWORD n = 0, p = 0;
    command[p++] = '"';
    while (path[n]) command[p++] = path[n++];
    command[p++] = '"';
    const char suffix[] = " --child";
    for (n = 0; n < sizeof(suffix); n++) command[p++] = suffix[n];
    return CreateProcessA(path, command, NIL, NIL, 0, 4, NIL, NIL, &startup, child);
}

void mainCRTStartup(void)
{
    if (option("--child")) ExitProcess(83);
    DWORD length = GetModuleFileNameA(NIL, path, sizeof(path));
    require(length > 0 && length < sizeof(path), "module path");
    startup.cb = sizeof(startup);
    for (DWORD round = 0; round < 2; round++) {
        for (DWORD i = 0; i < CAPACITY; i++) {
            require(spawn(&children[i]), "create suspended child");
            require(children[i].pid != 0 && children[i].tid != 0, "child identity");
            for (DWORD j = 0; j < i; j++)
                require(children[i].pid != children[j].pid, "unique child pid");
            DWORD status = 0;
            require(GetExitCodeProcess(children[i].process, &status) && status == 259,
                    "child remains suspended");
            print("CHILD-CAPACITY CREATED "); number(i + 1); print("\r\n");
        }
        if (option("--limit")) {
            PROCESS_INFORMATION extra = {0};
            BOOL created = spawn(&extra);
            if (created) {
                TerminateProcess(extra.process, 1);
                WaitForSingleObject(extra.process, 5000);
                CloseHandle(extra.thread); CloseHandle(extra.process);
            }
            require(!created && GetLastError() != 0, "capacity exhaustion rejected");
            print("CHILD-CAPACITY LIMIT PASS\r\n");
        }
        for (DWORD i = 0; i < CAPACITY; i++)
            require(ResumeThread(children[i].thread) == 1, "resume child");
        for (DWORD i = 0; i < CAPACITY; i++) {
            require(WaitForSingleObject(children[i].process, 10000) == 0, "child exit");
            DWORD status = 0;
            require(GetExitCodeProcess(children[i].process, &status) && status == 83,
                    "child executed entry point");
        }
        if (option("--hold-handles")) {
            print("CHILD-CAPACITY HOLD HANDLES "); number(round + 1); print("\r\n");
            Sleep(5000);
        }
        for (DWORD i = 0; i < CAPACITY; i++) {
            DWORD status = 0;
            require(WaitForSingleObject(children[i].process, 0) == 0 &&
                    WaitForSingleObject(children[i].thread, 0) == 0 &&
                    GetExitCodeProcess(children[i].process, &status) && status == 83,
                    "retained exit handles");
            require(CloseHandle(children[i].thread), "close child thread");
            require(CloseHandle(children[i].process), "close child process");
            children[i].thread = children[i].process = NIL;
        }
        print("CHILD-CAPACITY ROUND PASS "); number(round + 1); print("\r\n");
    }
    print("CHILD-CAPACITY PASS\r\n");
    ExitProcess(0);
}
