/* Hardware exceptions after a copied native NTDLL service returns. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int LONG;
typedef int BOOL;
typedef __UINTPTR_TYPE__ UPTR;
typedef __SIZE_TYPE__ SIZE;
typedef void *HANDLE;
#define API __declspec(dllimport)
API void ExitProcess(DWORD);
API HANDLE GetStdHandle(DWORD);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API HANDLE LoadLibraryA(const char *);
API void *GetProcAddress(HANDLE, const char *);
typedef struct {
    DWORD code, flags; void *nested, *address; DWORD count;
    UPTR arguments[15];
} RECORD;
typedef struct { RECORD *record; BYTE *context; } POINTERS;
typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS;
typedef struct { LONG status; void *peb; UPTR affinity; LONG priority;
    UPTR pid, parent; } BASIC;
static void *(*alloc)(void *, SIZE, DWORD, DWORD);
static BOOL (*protect)(void *, SIZE, DWORD, DWORD *);
static void *(*add_veh)(DWORD, void *);
static DWORD (*remove_veh)(void *);
static DWORD (*error_mode)(DWORD);
static char *(*command_line)(void);
static DWORD (*module_path)(HANDLE, char *, DWORD);
static BOOL (*create_process)(const char *, char *, void *, void *, BOOL,
    DWORD, void *, const char *, void *, PROCESS *);
static HANDLE (*create_thread)(void *, SIZE, DWORD (*)(void *), void *, DWORD, DWORD *);
static DWORD (*wait)(HANDLE, DWORD);
static BOOL (*exit_code)(HANDLE, DWORD *);
static BOOL (*close_handle)(HANDLE);
static BOOL (*terminate)(HANDLE, DWORD);
static LONG (*query)(HANDLE, DWORD, void *, DWORD, DWORD *);
static DWORD failures;
static volatile DWORD seen, expected;
static WORD expected_cs;

void *memset(void *out, int value, SIZE n)
{ for (SIZE i = 0; i < n; i++) ((BYTE *)out)[i] = (BYTE)value; return out; }
static void print(const char *s)
{ DWORD n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0); }
static void require(BOOL ok, const char *label)
{
    if (ok) return;
    print("SYSCALL-EXCEPTION FAIL "); print(label); print("\n");
    ExitProcess(90);
}
static BOOL has(const char *s, const char *word)
{ for (; *s; s++) { DWORD i = 0; while (word[i] && word[i] == s[i]) i++; if (!word[i]) return 1; } return 0; }
static WORD selector(void)
{ WORD cs; __asm__ volatile ("mov %%cs,%0" : "=r"(cs)); return cs; }
static void service(void)
{
    BASIC info = {0}; DWORD bytes = 0;
    require(query((HANDLE)(UPTR)-1, 0, &info, sizeof(info), &bytes) == 0 &&
            bytes == sizeof(info) && info.pid, "copied native service");
}
static LONG handler(POINTERS *p)
{
    if (!p || !p->record || !p->context || p->record->code != expected)
        return 0;
    UPTR *rip = (UPTR *)(p->context + 0xf8);
    UPTR *rsp = (UPTR *)(p->context + 0x98);
    if (*(WORD *)(p->context + 0x38) != expected_cs ||
        *rip != (UPTR)p->record->address) failures++;
    if (expected == 0xc0000005U) {
        if (*rip || p->record->count != 2 || p->record->arguments[0] != 8 ||
            p->record->arguments[1] != 0) failures++;
        *rip = *(UPTR *)*rsp;
        *rsp += sizeof(UPTR);
    } else {
        *rip += expected == 0x80000003U ? 1 : 2;
    }
    seen++;
    return -1;
}
__declspec(noinline) static void fault(DWORD mode)
{
    if (mode == 0) __asm__ volatile ("int3");
    else if (mode == 1) __asm__ volatile ("ud2");
    else __asm__ volatile ("xor %%rax,%%rax; call *%%rax" ::: "rax", "memory");
}
static DWORD fault_worker(void *arg)
{ service(); fault((DWORD)(UPTR)arg); ExitProcess(91); return 91; }
static void handled(void)
{
    void *cookie = add_veh(1, handler);
    require(cookie != 0, "register VEH");
    /* Cover both the initial selector and the one retained after SYSCALL. */
    for (DWORD phase = 0; phase < 2; phase++) {
        if (phase) service();
        expected_cs = selector();
        for (DWORD mode = 0; mode < 3; mode++) {
            expected = mode == 0 ? 0x80000003U : mode == 1 ? 0xc000001dU : 0xc0000005U;
            seen = 0; fault(mode);
            require(seen == 1 && !failures, "hardware exception record and continuation");
            print("SYSCALL-EXCEPTION PASS handled\n");
        }
    }
    require(remove_veh(cookie) != 0, "remove VEH");
    print(expected_cs == 0x90 ? "SYSCALL-EXCEPTION CS 0090\n" :
          "SYSCALL-EXCEPTION CS host\n");
}
void mainCRTStartup(void)
{
    HANDLE k = LoadLibraryA("kernel32.dll"), nt = LoadLibraryA("ntdll.dll");
    require(k && nt, "load service libraries");
#define RESOLVE(var, name) do { var = (void *)GetProcAddress(k, name); require(var != 0, name); } while (0)
    RESOLVE(alloc, "VirtualAlloc"); RESOLVE(protect, "VirtualProtect");
    RESOLVE(add_veh, "AddVectoredExceptionHandler"); RESOLVE(remove_veh, "RemoveVectoredExceptionHandler");
    RESOLVE(error_mode, "SetErrorMode"); RESOLVE(command_line, "GetCommandLineA");
    RESOLVE(module_path, "GetModuleFileNameA"); RESOLVE(create_process, "CreateProcessA");
    RESOLVE(create_thread, "CreateThread"); RESOLVE(wait, "WaitForSingleObject");
    RESOLVE(exit_code, "GetExitCodeProcess"); RESOLVE(close_handle, "CloseHandle");
    RESOLVE(terminate, "TerminateProcess");
    error_mode(0x8003); /* No interactive error dialogs for deliberate faults. */
    BYTE *entry = GetProcAddress(nt, "NtQueryInformationProcess");
    require(entry && *(DWORD *)entry == 0xb8d18b4cU &&
            ((entry[8] == 0x0f && entry[9] == 5) ||
             (entry[18] == 0x0f && entry[19] == 5)), "native service opcode");
    BYTE *copy = alloc(0, 4096, 0x3000, 4); DWORD previous = 0;
    require(copy != 0, "allocate service copy");
    for (DWORD i = 0; i < 32; i++) copy[i] = entry[i];
    require(protect(copy, 4096, 0x20, &previous), "publish service copy");
    query = (void *)copy;
    char *args = command_line();
    if (has(args, "--child")) {
        DWORD mode = has(args, "null") ? 2 : has(args, "ud") ? 1 : 0;
        if (has(args, "worker")) {
            HANDLE thread = create_thread(0, 0, fault_worker, (void *)(UPTR)mode, 0, 0);
            require(thread != 0, "fault worker");
            wait(thread, 15000);
        } else { service(); fault(mode); }
        ExitProcess(92);
    }
    handled();
    if (has(args, "--handled")) { print("SYSCALL-EXCEPTION DONE handled\n"); ExitProcess(0); }
    char path[512]; require(module_path(0, path, sizeof(path)) != 0, "image path");
    DWORD count = has(args, "--single") ? 1 : 40;
    for (DWORD i = 0; i < count; i++) {
        DWORD mode = i % 3;
        char command[560]; DWORD p = 0, n = 0;
        command[p++] = '"'; while (path[n]) command[p++] = path[n++]; command[p++] = '"';
        const char *suffix = mode == 0 ? " --child bp" : mode == 1 ? " --child ud" : " --child null";
        while (*suffix) command[p++] = *suffix++;
        if (i & 1) { suffix = " worker"; while (*suffix) command[p++] = *suffix++; }
        command[p] = 0;
        BYTE startup[104] = {0}; *(DWORD *)startup = sizeof(startup);
        PROCESS child = {0};
        require(create_process(path, command, 0, 0, 0, 0, 0, 0, startup, &child), "create child across capacity");
        DWORD waited = wait(child.process, 10000), code = 0;
        if (waited != 0) terminate(child.process, 93);
        require(waited == 0, "faulted process signaled");
        require(wait(child.thread, 1000) == 0, "faulted primary thread signaled");
        require(exit_code(child.process, &code) && code ==
            (mode == 0 ? 0x80000003U : mode == 1 ? 0xc000001dU : 0xc0000005U), "native fault status retained");
        require(close_handle(child.thread) && close_handle(child.process), "release dead child handles");
        print("SYSCALL-EXCEPTION PASS child\n");
    }
    print("SYSCALL-EXCEPTION DONE\n"); ExitProcess(0);
}
