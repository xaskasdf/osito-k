/* QEMU-only: exercise the scheduler's abnormal-thread retirement path.
 * Internal SysV addresses are resolved from the exact test kernel ELF. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef void *HANDLE;
typedef int BOOL;
#define IMPORT __declspec(dllimport)
#define SYSV __attribute__((sysv_abi))

typedef struct {
    HANDLE process, thread;
    DWORD pid, tid;
} PROCESS_INFORMATION64;

IMPORT void ExitProcess(DWORD);
IMPORT HANDLE GetStdHandle(DWORD);
IMPORT BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT HANDLE CreateThread(void *, U64, DWORD (*)(void *), void *, DWORD, DWORD *);
IMPORT HANDLE CreateEventA(void *, BOOL, BOOL, const char *);
IMPORT BOOL SetEvent(HANDLE);
IMPORT void Sleep(DWORD);
IMPORT DWORD WaitForSingleObject(HANDLE, DWORD);
IMPORT BOOL GetExitCodeThread(HANDLE, DWORD *);
IMPORT BOOL GetExitCodeProcess(HANDLE, DWORD *);
IMPORT BOOL CloseHandle(HANDLE);
IMPORT char *GetCommandLineA(void);
IMPORT BOOL CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD,
                          void *, const char *, void *, PROCESS_INFORMATION64 *);
IMPORT HANDLE LoadLibraryA(const char *);
IMPORT void *GetProcAddress(HANDLE, const char *);
IMPORT DWORD GetLastError(void);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT HANDLE OpenProcess(DWORD, BOOL, DWORD);

void *memcpy(void *destination, const void *source, U64 size)
{
    volatile unsigned char *dst = destination;
    const volatile unsigned char *src = source;
    for (U64 i = 0; i < size; i++) dst[i] = src[i];
    return destination;
}

static void print(const char *text)
{
    DWORD size = 0, written;
    while (text[size]) size++;
    WriteFile(GetStdHandle((DWORD)-11), text, size, &written, (void *)0);
}

static void fail(unsigned code)
{
    char message[] = "TLS-REAP64 FAIL 00\r\n";
    message[16] = (char)('0' + code / 10);
    message[17] = (char)('0' + code % 10);
    print(message);
    ExitProcess(code);
    for (;;) { }
}

static U64 argument(const char **text)
{
    while (**text == ' ') (*text)++;
    U64 value = 0;
    if (!**text) fail(1);
    while (**text && **text != ' ') {
        unsigned c = (unsigned char)*(*text)++;
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else fail(1);
        value = value * 16 + c;
    }
    return value;
}

#ifdef TLS_REAP_CHILD
static int (SYSV *current_pid)(void);
static volatile int worker_pid;
static volatile U64 worker_vector;
static volatile DWORD attaches, detaches;
static const U64 template_data = 0x123456789abcdef0ULL;

static void tls_callback(void *image, DWORD reason, void *reserved)
{
    (void)image; (void)reserved;
    if (reason == 2) attaches++;
    if (reason == 3) detaches++;
}

static DWORD worker(void *event)
{
    U64 *vector;
    __asm__ volatile ("movq %%gs:0x58, %0" : "=r"(vector));
    if (!vector || !vector[7] || *(U64 *)vector[7] != template_data)
        fail(2);
    worker_vector = (U64)vector;
    worker_pid = current_pid();
    return WaitForSingleObject(event, 30000) == 0 ? 77 : 78;
}

static void api_contract(void)
{
    HANDLE kernel = LoadLibraryA("kernel32.dll");
    HANDLE crt = LoadLibraryA("MSVCR120.dll");
    BOOL (*cycles)(HANDLE, U64 *) = GetProcAddress(kernel, "QueryProcessCycleTime");
    int (*lead)(unsigned) = GetProcAddress(crt, "_ismbblead");
    if (!cycles || !lead) fail(3);
    for (unsigned c = 0; c <= 256; c++)
        if (lead(c)) fail(4);
    if (lead(~0U)) fail(5);
    U64 sentinel = 0xfeedfacecafebeefULL;
    U64 output = sentinel;
    if (!cycles((HANDLE)-1, &output) || !output || output == sentinel)
        fail(6);
    if (cycles((HANDLE)-1, (void *)0) || GetLastError() != 87) fail(7);
    if (cycles((HANDLE)0, &output) || GetLastError() != 6) fail(8);
    for (unsigned i = 0; i < 3; i++) {
        HANDLE process = OpenProcess(i == 0 ? 0 : i == 1 ? 0x400 : 0x1000,
                                      0, GetCurrentProcessId());
        if (!process) fail(9);
        output = sentinel;
        BOOL result = cycles(process, &output);
        if (i ? (!result || !output || output == sentinel) :
                (result || GetLastError() != 5 || output != sentinel)) fail(10);
        if (!CloseHandle(process)) fail(11);
    }
    print("TLS-REAP64 IMPORT-CONTRACT PASS\r\n");
}
#endif

void mainCRTStartup(void)
{
    const char *text = GetCommandLineA();
    while (*text && *text != ' ') text++;
#ifdef TLS_REAP_CHILD
    BOOL (SYSV *register_tls)(DWORD, void *, void *, U64, U64, void *, DWORD) =
        (void *)argument(&text);
    void (SYSV *unregister_tls)(void *) = (void *)argument(&text);
    current_pid = (void *)argument(&text);
    int (SYSV *kill_pid)(int) = (void *)argument(&text);
    U64 (SYSV *translate)(U64, U64) = (void *)argument(&text);
    unsigned check_imports = (unsigned)argument(&text);
    if (check_imports) api_contract();
    U64 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    void (*callbacks[])(void *, DWORD, void *) = { tls_callback };
    if (!register_tls(7, (void *)&template_data, (void *)&template_data,
                       sizeof(template_data), 4096, callbacks, 1)) fail(12);
    for (unsigned iteration = 0; iteration < 4; iteration++) {
        worker_pid = 0;
        worker_vector = 0;
        HANDLE event = CreateEventA((void *)0, 1, 0, (void *)0);
        HANDLE thread = CreateThread((void *)0, 0, worker, event, 0, (void *)0);
        if (!event || !thread) fail(13);
        for (unsigned n = 0; !worker_pid && n < 500; n++) Sleep(10);
        if (!worker_pid || worker_vector >= 0x800000000000ULL ||
            translate(cr3, worker_vector) == ~0ULL) fail(14);
        if (attaches != iteration + 1) fail(15);
        if (!iteration) {
            if (!SetEvent(event)) fail(16);
        } else {
            print("TLS-REAP64 ABNORMAL-STOP\r\n");
            if (kill_pid(worker_pid) != 0) fail(17);
        }
        DWORD code;
        if (WaitForSingleObject(thread, 15000) != 0 ||
            !GetExitCodeThread(thread, &code) ||
            code != (iteration ? ~0UL : 77)) fail(18);
        for (unsigned n = 0; translate(cr3, worker_vector) != ~0ULL && n < 100;
             n++) Sleep(10);
        if (translate(cr3, worker_vector) != ~0ULL || detaches != 1) fail(19);
        if (!CloseHandle(thread) || !CloseHandle(event)) fail(20);
    }
    unregister_tls((void *)&template_data);
    print("TLS-REAP64 CHILD PASS\r\n");
    ExitProcess(0);
#else
    char command[512] = "tlschild.exe";
    unsigned size = 12;
    while (*text && size + 1 < sizeof(command)) command[size++] = *text++;
    command[size] = 0;
    if (*text) fail(21);
    unsigned char startup[104] = {0};
    *(DWORD *)startup = sizeof(startup);
    PROCESS_INFORMATION64 child = {0};
    if (!CreateProcessA("tlschild.exe", command, (void *)0, (void *)0,
                        0, 0, (void *)0, (void *)0, startup, &child)) fail(22);
    DWORD code;
    if (WaitForSingleObject(child.process, 60000) != 0 ||
        !GetExitCodeProcess(child.process, &code) || code) fail(23);
    if (!CloseHandle(child.thread) || !CloseHandle(child.process)) fail(24);
    print("TLS-REAP64 PASS\r\n");
    ExitProcess(0);
#endif
}
